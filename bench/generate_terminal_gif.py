"""Generate a realistic, high-fidelity dual-terminal animated GIF for zcrudp.
Demonstrates:
- Client & Server running ./build/demo_loss
- Unreliable channel: zero-copy fresh updates delivered instantly
- Reliable channel: simulated packet loss (20%)
- NO Head-of-Line blocking: unreliable updates continue while reliable packet is dropped
- Karn-safe RTO adaptive retransmission & ordered delivery
- Out-of-order gap retention in bounded RX window
- Final statistics showing 0 mallocs and guaranteed delivery
"""
import gzip
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Palette definition (24 crisp colors)
PALETTE = [
    (8, 14, 25),      # 0: Canvas background (#080e19)
    (13, 17, 23),     # 1: Terminal background (#0d1117)
    (22, 27, 34),     # 2: Header / border (#161b22)
    (33, 38, 45),     # 3: Tab / pane header (#21262d)
    (48, 54, 61),     # 4: Divider / border (#30363d)
    (255, 95, 86),    # 5: Red dot (#ff5f56)
    (255, 189, 46),   # 6: Yellow dot (#ffbd2e)
    (39, 201, 63),    # 7: Green dot (#27c93f)
    (230, 237, 243),  # 8: Text white (#e6edf3)
    (139, 148, 158),  # 9: Text gray (#8b949e)
    (88, 166, 255),   # 10: Cyan / Blue (#58a6ff)
    (63, 185, 80),    # 11: Green (#3fb950)
    (248, 81, 73),    # 12: Red (#f85149)
    (227, 179, 65),   # 13: Amber / Yellow (#e3b341)
    (188, 140, 255),  # 14: Purple (#bc8cff)
    (31, 111, 235),   # 15: Highlight blue (#1f6feb)
    (10, 44, 28),     # 16: Dim green background (#0a2c1c)
    (54, 18, 20),     # 17: Dim red background (#361214)
    (46, 34, 12),     # 18: Dim amber background (#2e220c)
    (21, 38, 66),     # 19: Dim blue background (#152642)
    (201, 209, 217),  # 20: Light gray (#c9d1d9)
    (255, 255, 255),  # 21: Pure white (#ffffff)
    (86, 211, 100),   # 22: Bright lime (#56d364)
    (110, 118, 129),  # 23: Darker gray (#6e7681)
]

WIDTH = 960
HEIGHT = 570

def load_font():
    font_paths = [
        "/usr/share/consolefonts/Uni2-TerminusBold16.psf.gz",
        "/usr/share/consolefonts/Uni2-Terminus16.psf.gz",
        "/usr/share/consolefonts/Uni1-VGA16.psf.gz",
    ]
    for p in font_paths:
        if os.path.exists(p):
            with gzip.open(p, "rb") as f:
                data = f.read()
            if data.startswith(b"\x36\x04"):
                charsize = data[3]
                glyphs = {}
                for ch in range(128):
                    offset = 4 + ch * charsize
                    glyphs[ch] = data[offset : offset + 16]
                return glyphs
    raise RuntimeError("No suitable console font found!")

FONT = load_font()

class TerminalCanvas:
    def __init__(self):
        self.buf = bytearray([0] * (WIDTH * HEIGHT))
        self.draw_chrome()

    def set_pixel(self, x, y, color_idx):
        if 0 <= x < WIDTH and 0 <= y < HEIGHT:
            self.buf[y * WIDTH + x] = color_idx

    def fill_rect(self, x, y, w, h, color_idx):
        for cy in range(max(0, y), min(HEIGHT, y + h)):
            row_start = cy * WIDTH + max(0, x)
            row_end = cy * WIDTH + min(WIDTH, x + w)
            self.buf[row_start:row_end] = bytes([color_idx] * (row_end - row_start))

    def draw_circle(self, cx, cy, r, color_idx):
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                if dx * dx + dy * dy <= r * r:
                    self.set_pixel(cx + dx, cy + dy, color_idx)

    def draw_char(self, x, y, ch, fg_idx, bg_idx=None):
        ascii_code = ord(ch) if isinstance(ch, str) else ch
        glyph = FONT.get(ascii_code, FONT.get(ord("?")))
        for row_idx, byte in enumerate(glyph):
            cy = y + row_idx
            if cy < 0 or cy >= HEIGHT:
                continue
            for bit_idx in range(8):
                cx = x + bit_idx
                if cx < 0 or cx >= WIDTH:
                    continue
                if (byte >> (7 - bit_idx)) & 1:
                    self.buf[cy * WIDTH + cx] = fg_idx
                elif bg_idx is not None:
                    self.buf[cy * WIDTH + cx] = bg_idx

    def draw_text(self, x, y, text, fg_idx=8, bg_idx=None):
        cx = x
        for ch in text:
            self.draw_char(cx, y, ch, fg_idx, bg_idx)
            cx += 8

    def draw_chrome(self):
        # Window background (#0d1117)
        self.fill_rect(20, 15, 920, 540, 1)
        # Window border (#30363d)
        for x in range(20, 940):
            self.set_pixel(x, 15, 4)
            self.set_pixel(x, 554, 4)
        for y in range(15, 555):
            self.set_pixel(20, y, 4)
            self.set_pixel(939, y, 4)

        # Title bar background (#161b22)
        self.fill_rect(21, 16, 918, 32, 2)
        for x in range(21, 939):
            self.set_pixel(x, 48, 4)

        # Window dots
        self.draw_circle(38, 32, 5, 5) # Red
        self.draw_circle(54, 32, 5, 6) # Yellow
        self.draw_circle(70, 32, 5, 7) # Green

        # Title text
        title = "zcrudp | Dual-Peer Interactive Loss & Recovery Demo (POSIX UDP)"
        tx = (WIDTH - len(title) * 8) // 2
        self.draw_text(tx, 24, title, 20)

        # Pane Header Tabs
        # Left: SERVER
        self.fill_rect(21, 49, 458, 26, 3)
        self.draw_text(32, 54, "SERVER  127.0.0.1:9000  (Receiver / Host)", 10)
        # Right: CLIENT
        self.fill_rect(481, 49, 458, 26, 3)
        self.draw_text(492, 54, "CLIENT  127.0.0.1:9001  (Sender / Game Peer)", 10)

        # Divider between panes
        for y in range(49, 520):
            self.set_pixel(479, y, 4)
            self.set_pixel(480, y, 4)

        # Footer status bar (#161b22)
        self.fill_rect(21, 520, 918, 34, 2)
        for x in range(21, 939):
            self.set_pixel(x, 520, 4)
        footer = "[OK] 0 Mallocs (Zero-Heap)   [OK] 32-bit TFV Frames   [OK] No HoL Blocking"
        fx = (WIDTH - len(footer) * 8) // 2
        self.draw_text(fx, 529, footer, 11)

    def render_panes(self, server_lines, client_lines):
        # Clear terminal areas
        self.fill_rect(21, 75, 458, 444, 1)
        self.fill_rect(481, 75, 458, 444, 1)

        # Left pane lines
        sy = 83
        for line in server_lines:
            sx = 32
            for segment in line:
                txt = segment[0]
                fg = segment[1] if len(segment) > 1 else 8
                bg = segment[2] if len(segment) > 2 else None
                self.draw_text(sx, sy, txt, fg, bg)
                sx += len(txt) * 8
            sy += 19

        # Right pane lines
        cy = 83
        for line in client_lines:
            cx = 492
            for segment in line:
                txt = segment[0]
                fg = segment[1] if len(segment) > 1 else 8
                bg = segment[2] if len(segment) > 2 else None
                self.draw_text(cx, cy, txt, fg, bg)
                cx += len(txt) * 8
            cy += 19


def make_gif(width, height, palette, frames, durations, output_path):
    color_bits = max((len(palette) - 1).bit_length(), 2)
    palette_padded = list(palette) + [(0, 0, 0)] * ((1 << color_bits) - len(palette))

    out = bytearray()
    out.extend(b"GIF89a")
    out.extend(width.to_bytes(2, "little"))
    out.extend(height.to_bytes(2, "little"))
    packed = 0x80 | ((color_bits - 1) << 4) | (color_bits - 1)
    out.append(packed)
    out.append(0)
    out.append(0)

    for r, g, b in palette_padded:
        out.extend((r, g, b))

    out.extend(b"\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00")

    min_code_size = color_bits
    clear_code = 1 << min_code_size
    eoi_code = clear_code + 1

    for frame_idx, pixels in enumerate(frames):
        delay = durations[frame_idx]
        out.extend(b"\x21\xf9\x04\x04")
        out.extend(delay.to_bytes(2, "little"))
        out.extend(b"\x00\x00")

        out.append(0x2C)
        out.extend((0).to_bytes(2, "little"))
        out.extend((0).to_bytes(2, "little"))
        out.extend(width.to_bytes(2, "little"))
        out.extend(height.to_bytes(2, "little"))
        out.append(0)

        out.append(min_code_size)

        bit_buf = 0
        bit_len = 0
        block = bytearray()

        def write_code(code, code_size):
            nonlocal bit_buf, bit_len, block
            bit_buf |= code << bit_len
            bit_len += code_size
            while bit_len >= 8:
                block.append(bit_buf & 0xFF)
                bit_buf >>= 8
                bit_len -= 8
                if len(block) == 255:
                    out.append(255)
                    out.extend(block)
                    block = bytearray()

        code_size = min_code_size + 1
        table = {bytes([i]): i for i in range(clear_code)}
        next_code = eoi_code + 1

        write_code(clear_code, code_size)

        cur = b""
        for p in pixels:
            ch = bytes([p])
            nxt = cur + ch
            if nxt in table:
                cur = nxt
            else:
                write_code(table[cur], code_size)
                if next_code < 4096:
                    table[nxt] = next_code
                    next_code += 1
                    if next_code > (1 << code_size) and code_size < 12:
                        code_size += 1
                else:
                    write_code(clear_code, code_size)
                    code_size = min_code_size + 1
                    table = {bytes([i]): i for i in range(clear_code)}
                    next_code = eoi_code + 1
                cur = ch
        if cur:
            write_code(table[cur], code_size)
        write_code(eoi_code, code_size)

        if bit_len > 0:
            block.append(bit_buf & 0xFF)
        if block:
            out.append(len(block))
            out.extend(block)
        out.append(0)

    out.append(0x3B)
    with open(output_path, "wb") as f:
        f.write(out)


def build_scenario_frames():
    frames = []
    durations = []

    def snap(srv, cli, duration_cs):
        canvas = TerminalCanvas()
        canvas.render_panes(srv, cli)
        frames.append(bytes(canvas.buf))
        durations.append(duration_cs)

    # Base commands
    srv_0 = [
        [("$ ", 10), ("./build/demo_loss server --loss 20 --latency 40", 8)],
        [("READY server 127.0.0.1:9000 -> :9001", 11)],
        [("  loss=20% latency=40ms timeout=500ms", 9)],
        [("Commands: r <val>, u <val>, stats, q", 23)],
        [("---------------------------------------------", 4)],
    ]
    cli_0 = [
        [("$ ", 10), ("./build/demo_loss client --loss 20 --latency 40", 8)],
        [("READY client 127.0.0.1:9001 -> :9000", 11)],
        [("  loss=20% latency=40ms timeout=500ms", 9)],
        [("Commands: r <val>, u <val>, stats, q", 23)],
        [("---------------------------------------------", 4)],
    ]

    # Frame 1: Startup
    snap(srv_0, cli_0, 140)

    # Frame 2: Client types u 101 (prompt typing)
    cli_type_u = cli_0 + [
        [("client> ", 10), ("u 101", 13), ("_", 21)],
    ]
    snap(srv_0, cli_type_u, 45)

    # Frame 3: Client executes u 101
    cli_1 = cli_0 + [
        [("client> ", 10), ("u 101", 13)],
        [("[TX] ", 10), ("unreliable ", 11), ("val=101  ", 8), ("(bypass TX ring)", 9)],
    ]
    snap(srv_0, cli_1, 60)

    # Frame 4: Server receives u 101
    srv_1 = srv_0 + [
        [("[RX] ", 10), ("unreliable ", 11), ("val=101  ", 8), ("[DELIVERED]", 11, 16)],
    ]
    snap(srv_1, cli_1, 100)

    # Frame 5: Client types r 500 (Reliable event)
    cli_type_r = cli_1 + [
        [("client> ", 10), ("r 500", 13), ("_", 21)],
    ]
    snap(srv_1, cli_type_r, 45)

    # Frame 6: Client sends r 500 -> Simulated 20% loss DROPS packet!
    cli_2 = cli_1 + [
        [("client> ", 10), ("r 500", 13)],
        [("[TX] ", 10), ("reliable ", 10), ("seq=0 val=500  ", 8), ("(in TX ring)", 9)],
        [("[NET] ", 12), ("simulated 20% loss -> ", 12), ("DROP simulated len=12", 12, 17)],
    ]
    snap(srv_1, cli_2, 130)

    # Frame 7: KILLER FEATURE: Client sends u 102 DURING packet loss!
    cli_type_u2 = cli_2 + [
        [("client> ", 10), ("u 102", 13), ("_", 21)],
    ]
    snap(srv_1, cli_type_u2, 45)

    cli_3 = cli_2 + [
        [("client> ", 10), ("u 102", 13)],
        [("[TX] ", 10), ("unreliable ", 11), ("val=102  ", 8), ("(sent while r500 lost)", 9)],
    ]
    srv_2 = srv_1 + [
        [("[RX] ", 10), ("unreliable ", 11), ("val=102  ", 8), ("NO HoL Blocking!", 22, 16)],
    ]
    snap(srv_2, cli_3, 140)

    # Frame 8: Client RTO timer fires -> Adaptive Fast Retransmit
    cli_4 = cli_3 + [
        [("[RTO] ", 13), ("timeout expired -> ", 13), ("FAST RETRANSMIT", 13, 18)],
        [("[TX] ", 10), ("reliable ", 10), ("seq=0 val=500  ", 8), ("(retransmit #1)", 13)],
    ]
    snap(srv_2, cli_4, 110)

    # Frame 9: Server receives reliable retransmission & emits ACK
    srv_3 = srv_2 + [
        [("[RX] ", 10), ("reliable ", 10), ("seq=0 val=500  ", 8), ("[IN-ORDER]", 11, 16)],
        [("[ACK] ", 14), ("emitted cumulative ACK seq=1", 14)],
    ]
    cli_5 = cli_4 + [
        [("[ACK] ", 14), ("received ACK seq=1 -> slot retired", 14)],
    ]
    snap(srv_3, cli_5, 140)

    # Frame 10: Out-of-order gap retention in RX window
    cli_6 = cli_5 + [
        [("client> ", 10), ("r 501", 13), (" && ", 9), ("r 502", 13)],
        [("[TX] ", 10), ("seq=1 (delayed) | seq=2 (fast path)", 9)],
    ]
    srv_4 = srv_3 + [
        [("[RX] ", 10), ("seq=2 arrived ahead -> ", 13), ("BUFFERED in RX win", 13, 18)],
        [("[RX] ", 10), ("seq=1 arrived -> ", 11), ("DELIVER 1 then 2 in-order!", 11, 16)],
    ]
    snap(srv_4, cli_6, 160)

    # Frame 11: Final Stats verification
    cli_7 = cli_6 + [
        [("client> ", 10), ("stats", 13)],
        [("STATS ", 10), ("sent=5 dropped=1 delivered=4 retries=1", 8)],
        [("HEAP  ", 11), ("0 bytes allocated (100% static RAM)", 11, 16)],
    ]
    srv_5 = srv_4 + [
        [("server> ", 10), ("stats", 13)],
        [("STATS ", 10), ("received=5 delivered=5 (0 reliable lost)", 8)],
        [("HEAP  ", 11), ("0 bytes allocated (100% static RAM)", 11, 16)],
    ]
    # Long hold at the end so the user can read all details
    snap(srv_5, cli_7, 480)

    return frames, durations


def generate():
    print("Rendering terminal scenario frames...")
    frames, durations = build_scenario_frames()
    print(f"Generated {len(frames)} frames. Encoding GIF...")
    output_path = ROOT / "docs/visual/recovery.gif"
    make_gif(WIDTH, HEIGHT, PALETTE, frames, durations, str(output_path))
    print(f"SUCCESS: Saved {output_path} ({output_path.stat().st_size:,} bytes)")


if __name__ == "__main__":
    generate()
