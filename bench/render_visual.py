"""Build a real-engine trace and an offline HTML replay; optionally capture a GIF.
Core/trace generation: C compiler + Python stdlib. GIF: Playwright/Chromium + Pillow.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def generate(output, capture):
    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    compiler = os.environ.get("CC", "cc")
    command = [compiler, "-Iinclude", "-Wall", "-Wextra", "-std=c11", "-pedantic",
               "-Werror", "-O2", "-UNDEBUG", "src/rudp.c", "bench/trace_rudp.c",
               "-o", str(build / "trace_rudp")]
    subprocess.run(command, cwd=ROOT, check=True)
    raw = subprocess.check_output([str(build / "trace_rudp")], cwd=ROOT, text=True)
    # Running twice checks deterministic replay, independently of rendering.
    assert raw == subprocess.check_output([str(build / "trace_rudp")], cwd=ROOT, text=True)
    trace = json.loads(raw)
    trace["provenance"] = {"scenario": "scripted loss and reorder; explanatory, not a benchmark",
        "build_command": command, "compiler": subprocess.check_output([compiler, "--version"], text=True).splitlines()[0],
        "sha256": {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in
                   ("src/rudp.c", "include/protocol_rudp.h", "include/protocol_tfv.h",
                    "bench/trace_rudp.c", "bench/visualizer.html", "bench/render_visual.py")}}
    assert trace["snapshots"][-1]["delivered"] == 8
    assert any(s["buffer"] for s in trace["snapshots"])
    output.mkdir(parents=True, exist_ok=True)
    data = json.dumps(trace, separators=(",", ":"))
    (output / "trace.json").write_text(data + "\n")
    template = (ROOT / "bench/visualizer.html").read_text()
    assert template.count("__TRACE_DATA__") == 1
    (output / "index.html").write_text(template.replace("__TRACE_DATA__", data))
    if capture:
        from PIL import Image
        from playwright.sync_api import sync_playwright
        frames = []
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(headless=True)
            page = browser.new_page(viewport={"width": 992, "height": 900}, device_scale_factor=1)
            errors = []
            page.on("pageerror", lambda error: errors.append(str(error)))
            page.add_init_script("window.captureMode = true")
            page.goto((output / "index.html").resolve().as_uri())
            page.wait_for_function("typeof window.renderAt === 'function'")
            for t in range(0, trace["duration"] + 1, 3):
                state = page.evaluate("t => window.renderAt(t)", t)
                assert state == trace["snapshots"][t]
                png = page.locator("#stage").screenshot(animations="disabled")
                frames.append(Image.open(io.BytesIO(png)).convert("RGB"))
                if t == 120:
                    (output / "poster.png").write_bytes(png)
            page.locator("#seek").evaluate("el => { el.value = 120; el.dispatchEvent(new Event('input')); }")
            assert page.locator("#time").text_content() == "120 ms"
            page.get_by_role("button", name="Replay", exact=True).click()
            assert page.locator("#time").text_content() == "0 ms"
            assert not errors, errors
            browser.close()
        # Shared palette avoids per-frame palette flicker. Runtime ~13 seconds.
        palette = frames[40].quantize(colors=128)
        quantized = [f.quantize(palette=palette, dither=Image.Dither.NONE) for f in frames]
        durations = [180] * len(quantized)
        durations[-1] = 4200
        quantized[0].save(output / "recovery.gif", save_all=True, append_images=quantized[1:],
                          duration=durations, loop=0, optimize=True, disposal=1)
        with Image.open(output / "recovery.gif") as gif:
            assert gif.n_frames > 30 and gif.size == (960, 570)
        print(f"GIF: {(output / 'recovery.gif').stat().st_size:,} bytes; browser interactions passed")
    print(f"PASS: deterministic trace, offline replay saved to {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "docs/visual")
    parser.add_argument("--gif", action="store_true")
    args = parser.parse_args()
    generate(args.output, args.gif)
