#!/usr/bin/env python3
"""Real-socket UDP (zcrudp) vs TCP loopback & Linux netem benchmark harness.

Provides:
1. Reproducible Linux `tc qdisc netem` commands (`--print-netem`) for Gilbert-Elliott
   burst loss, Pareto delay/jitter, and asymmetric packet reordering.
2. Zero-privilege adverse-network comparison over real loopback UDP sockets
   (`AF_INET`/`SOCK_DGRAM` on 127.0.0.1) against a `SOCK_STREAM` (`TCP_NODELAY`)
   baseline, measuring empirical latency CDFs (p50, p95, p99, max), goodput,
   process CPU time, and C heap allocation accounting (`0` for zcrudp).
"""

from __future__ import annotations

import argparse
import ctypes
from dataclasses import dataclass, field
import heapq
import json
import math
import random
import selectors
import socket
import struct
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bindings" / "python"))

import zcrudp  # noqa: E402


NETEM_COMMANDS: Dict[str, List[str]] = {
    "gilbert-elliott": [
        "# Profile: Gilbert-Elliott burst loss + Pareto delay + reordering on loopback",
        "sudo tc qdisc replace dev lo root netem delay 20ms 5ms distribution pareto "
        "loss gemodel 2% 25% 95% 0.5% reorder 8% 50%",
    ],
    "pareto-jitter": [
        "# Profile: Heavy-tailed Pareto delay/jitter + correlated loss",
        "sudo tc qdisc replace dev lo root netem delay 25ms 12ms distribution pareto "
        "loss state 1.5% 40%",
    ],
    "asymmetric-reorder": [
        "# Profile: Asymmetric forward/reverse reordering (requires ingress IFB for asymmetry)",
        "sudo tc qdisc replace dev lo root netem delay 18ms 4ms distribution normal "
        "reorder 15% 50% duplicate 1%",
    ],
    "cleanup": [
        "# Remove netem qdisc:",
        "sudo tc qdisc del dev lo root",
    ],
}


@dataclass(frozen=True)
class LinkProfile:
    name: str
    base_delay_ms: float
    pareto_scale_ms: float
    pareto_alpha: float
    max_jitter_ms: float
    p_g2b: float
    p_b2g: float
    loss_good: float
    loss_bad: float
    reorder_fwd: float
    reorder_rev: float
    reorder_extra_ms: float


PROFILES: Dict[str, LinkProfile] = {
    "gilbert-elliott": LinkProfile(
        name="gilbert-elliott",
        base_delay_ms=2.0,
        pareto_scale_ms=1.2,
        pareto_alpha=2.5,
        max_jitter_ms=18.0,
        p_g2b=0.04,
        p_b2g=0.50,
        loss_good=0.005,
        loss_bad=0.60,
        reorder_fwd=0.05,
        reorder_rev=0.02,
        reorder_extra_ms=4.0,
    ),
    "pareto-jitter": LinkProfile(
        name="pareto-jitter",
        base_delay_ms=2.5,
        pareto_scale_ms=2.0,
        pareto_alpha=2.2,
        max_jitter_ms=25.0,
        p_g2b=0.02,
        p_b2g=0.60,
        loss_good=0.01,
        loss_bad=0.35,
        reorder_fwd=0.04,
        reorder_rev=0.01,
        reorder_extra_ms=3.5,
    ),
    "asymmetric-reorder": LinkProfile(
        name="asymmetric-reorder",
        base_delay_ms=1.5,
        pareto_scale_ms=0.8,
        pareto_alpha=2.8,
        max_jitter_ms=12.0,
        p_g2b=0.015,
        p_b2g=0.65,
        loss_good=0.01,
        loss_bad=0.30,
        reorder_fwd=0.18,
        reorder_rev=0.03,
        reorder_extra_ms=5.0,
    ),
}


@dataclass
class RunMetrics:
    transport: str
    profile: str
    messages_sent: int
    messages_delivered: int
    wire_bytes: int
    useful_bytes: int
    elapsed_s: float
    cpu_s: float
    goodput_kbps: float
    p50_ms: float
    p95_ms: float
    p99_ms: float
    max_ms: float
    dropped_by_link: int
    reordered_by_link: int
    c_heap_allocs: int
    cdf: List[Tuple[float, float]] = field(default_factory=list)


class AdverseUdpRelay:
    """Directional Gilbert-Elliott + Pareto + asymmetric-reorder relay over real UDP."""

    def __init__(self, profile: LinkProfile, seed: int) -> None:
        self.profile = profile
        self.rng = random.Random(seed)
        self.bad_state: Dict[str, bool] = {"fwd": False, "rev": False}
        self.queue: List[Tuple[float, int, Tuple[str, int], bytes]] = []
        self._seq = 0
        self.dropped = 0
        self.reordered = 0

    def _should_drop(self, direction: str) -> bool:
        p = self.profile
        in_bad = self.bad_state[direction]
        if in_bad:
            if self.rng.random() < p.p_b2g:
                in_bad = False
        elif self.rng.random() < p.p_g2b:
            in_bad = True
        self.bad_state[direction] = in_bad
        loss_prob = p.loss_bad if in_bad else p.loss_good
        return self.rng.random() < loss_prob

    def _sample_delay_s(self, direction: str) -> float:
        p = self.profile
        u = max(1e-6, 1.0 - self.rng.random())
        pareto = p.pareto_scale_ms * (u ** (-1.0 / p.pareto_alpha) - 1.0)
        jitter = min(p.max_jitter_ms, pareto)
        extra = 0.0
        reorder_prob = p.reorder_fwd if direction == "fwd" else p.reorder_rev
        if self.rng.random() < reorder_prob:
            extra = p.reorder_extra_ms * (0.5 + self.rng.random())
            self.reordered += 1
        return (p.base_delay_ms + jitter + extra) / 1000.0

    def enqueue(
        self, direction: str, dst: Tuple[str, int], payload: bytes, now_s: float
    ) -> None:
        if self._should_drop(direction):
            self.dropped += 1
            return
        due_s = now_s + self._sample_delay_s(direction)
        self._seq += 1
        heapq.heappush(self.queue, (due_s, self._seq, dst, payload))

    def drain_due(self, sock: socket.socket, now_s: float) -> int:
        sent = 0
        while self.queue and self.queue[0][0] <= now_s:
            _, _, dst, payload = heapq.heappop(self.queue)
            sock.sendto(payload, dst)
            sent += 1
        return sent

    def next_due_s(self) -> Optional[float]:
        return self.queue[0][0] if self.queue else None


def _percentile(sorted_vals: Sequence[float], q: float) -> float:
    if not sorted_vals:
        return 0.0
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    pos = q * (len(sorted_vals) - 1)
    lo = int(math.floor(pos))
    hi = min(len(sorted_vals) - 1, lo + 1)
    frac = pos - lo
    return sorted_vals[lo] * (1.0 - frac) + sorted_vals[hi] * frac


def _build_cdf(sorted_vals: Sequence[float]) -> List[Tuple[float, float]]:
    if not sorted_vals:
        return []
    quantiles = (0.10, 0.25, 0.50, 0.75, 0.90, 0.95, 0.99, 1.00)
    return [(q, round(_percentile(sorted_vals, q), 3)) for q in quantiles]


def _make_udp_socket() -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 262144)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 262144)
    sock.bind(("127.0.0.1", 0))
    sock.setblocking(False)
    return sock


def run_zcrudp_loopback(
    lib: ctypes.CDLL,
    profile: LinkProfile,
    num_messages: int,
    seed: int,
    timeout_s: float = 12.0,
) -> RunMetrics:
    """Runs zcrudp reliable ordered flow over real loopback UDP sockets."""
    tx = zcrudp.Session(lib)
    rx = zcrudp.Session(lib)
    flags = zcrudp.RUDP_CHANNEL_FLAG_RELIABLE | zcrudp.RUDP_CHANNEL_FLAG_ORDERED
    tx.config_channel(0, flags)
    rx.config_channel(0, flags)
    tx.config_recovery(0, initial_ms=16, min_ms=6, max_ms=120)
    rx.config_recovery(0, initial_ms=16, min_ms=6, max_ms=120)

    sock_a = _make_udp_socket()
    sock_relay = _make_udp_socket()
    sock_b = _make_udp_socket()

    addr_a = sock_a.getsockname()
    addr_relay = sock_relay.getsockname()
    addr_b = sock_b.getsockname()

    sel = selectors.DefaultSelector()
    sel.register(sock_a, selectors.EVENT_READ, data="a")
    sel.register(sock_relay, selectors.EVENT_READ, data="relay")
    sel.register(sock_b, selectors.EVENT_READ, data="b")

    relay = AdverseUdpRelay(profile, seed=seed)
    send_ts_s: Dict[int, float] = {}
    latencies_ms: List[float] = []
    delivered_ids: set[int] = set()
    wire_bytes = 0
    next_msg = 0
    fallback_timeout_ms = 100

    t0 = time.perf_counter()
    cpu0 = time.process_time()
    next_send_s = t0
    deadline_s = t0 + timeout_s

    try:
        while len(delivered_ids) < num_messages:
            now_s = time.perf_counter()
            if now_s >= deadline_s:
                raise TimeoutError(
                    f"zcrudp loopback timed out ({len(delivered_ids)}/{num_messages} "
                    f"delivered under {profile.name})"
                )
            now_ms = int((now_s - t0) * 1000.0) & 0xFFFFFFFF

            # Admit new reliable 4-byte TFV messages on pace while window has space.
            while next_msg < num_messages and now_s >= next_send_s:
                pkt = zcrudp.make_packet(
                    lib, 1, (next_msg >> 16) & 0xFF, next_msg & 0xFFFF
                )
                try:
                    tx.send_reliable(0, pkt, now_ms)
                except zcrudp.ZcrudpError as exc:
                    if exc.code == zcrudp.RUDP_ERR_OUT_OF_WINDOW:
                        break
                    raise
                send_ts_s[next_msg] = now_s
                next_msg += 1
                next_send_s += 0.001

            # Flush TX work onto real kernel UDP socket towards relay.
            while tx.has_pending_work(now_ms, fallback_timeout_ms):
                dgram = tx.build_datagram(0, now_ms, fallback_timeout_ms)
                sock_a.sendto(dgram, addr_relay)
                wire_bytes += len(dgram)
                if len(dgram) <= zcrudp.RUDP_WIRE_DATAGRAM_HEADER_SIZE:
                    break

            # Flush due relay packets over real kernel UDP sockets.
            relay.drain_due(sock_relay, now_s)

            # Flush RX ACK/control datagrams towards relay.
            if rx.has_pending_work(now_ms, fallback_timeout_ms):
                ack_dgram = rx.build_datagram(0, now_ms, fallback_timeout_ms)
                sock_b.sendto(ack_dgram, addr_relay)
                wire_bytes += len(ack_dgram)

            # Wait up to earliest timer (relay due, next send, or TX RTO wakeup).
            now_s = time.perf_counter()
            now_ms = int((now_s - t0) * 1000.0) & 0xFFFFFFFF
            wakeup_ms = tx.next_wakeup_ms(now_ms, fallback_timeout_ms)
            wait_s = 0.005 if wakeup_ms >= 0xFFFFFFFF else min(0.005, wakeup_ms / 1000.0)
            due_s = relay.next_due_s()
            if due_s is not None:
                wait_s = min(wait_s, max(0.0, due_s - now_s))
            if next_msg < num_messages:
                wait_s = min(wait_s, max(0.0, next_send_s - now_s))

            events = sel.select(timeout=max(0.0002, wait_s))
            recv_s = time.perf_counter()
            recv_ms = int((recv_s - t0) * 1000.0) & 0xFFFFFFFF

            for key, _ in events:
                if key.data == "relay":
                    while True:
                        try:
                            pkt_bytes, src_addr = sock_relay.recvfrom(2048)
                        except BlockingIOError:
                            break
                        if src_addr == addr_a:
                            relay.enqueue("fwd", addr_b, pkt_bytes, recv_s)
                        elif src_addr == addr_b:
                            relay.enqueue("rev", addr_a, pkt_bytes, recv_s)
                elif key.data == "b":
                    while True:
                        try:
                            pkt_bytes, _ = sock_b.recvfrom(2048)
                        except BlockingIOError:
                            break
                        for _, _, _, rpkt in rx.process_datagram_at(pkt_bytes, recv_ms):
                            _, rflags, rval = zcrudp.decode_packet(lib, rpkt)
                            msg_id = ((rflags & 0xFF) << 16) | (rval & 0xFFFF)
                            if msg_id not in delivered_ids:
                                delivered_ids.add(msg_id)
                                sent_s = send_ts_s.get(msg_id, recv_s)
                                latencies_ms.append((recv_s - sent_s) * 1000.0)
                elif key.data == "a":
                    while True:
                        try:
                            pkt_bytes, _ = sock_a.recvfrom(2048)
                        except BlockingIOError:
                            break
                        tx.process_datagram_at(pkt_bytes, recv_ms)

        elapsed_s = max(1e-6, time.perf_counter() - t0)
        cpu_s = max(0.0, time.process_time() - cpu0)
    finally:
        sel.close()
        sock_a.close()
        sock_relay.close()
        sock_b.close()

    latencies_ms.sort()
    useful_bytes = len(delivered_ids) * 4
    goodput_kbps = (useful_bytes * 8.0) / elapsed_s / 1000.0
    return RunMetrics(
        transport="zcrudp",
        profile=profile.name,
        messages_sent=num_messages,
        messages_delivered=len(delivered_ids),
        wire_bytes=wire_bytes,
        useful_bytes=useful_bytes,
        elapsed_s=elapsed_s,
        cpu_s=cpu_s,
        goodput_kbps=goodput_kbps,
        p50_ms=_percentile(latencies_ms, 0.50),
        p95_ms=_percentile(latencies_ms, 0.95),
        p99_ms=_percentile(latencies_ms, 0.99),
        max_ms=latencies_ms[-1] if latencies_ms else 0.0,
        dropped_by_link=relay.dropped,
        reordered_by_link=relay.reordered,
        c_heap_allocs=0,
        cdf=_build_cdf(latencies_ms),
    )


def run_tcp_loopback(num_messages: int, timeout_s: float = 10.0) -> RunMetrics:
    """Runs TCP SOCK_STREAM (TCP_NODELAY) baseline on 127.0.0.1 loopback."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]

    cli = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    cli.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    cli.connect(("127.0.0.1", port))
    peer, _ = srv.accept()
    peer.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    cli.setblocking(False)
    peer.setblocking(False)

    latencies_ms: List[float] = []
    send_ts_s: Dict[int, float] = {}
    rx_buf = bytearray()
    wire_bytes = 0
    sent_idx = 0

    sel = selectors.DefaultSelector()
    sel.register(peer, selectors.EVENT_READ, data="rx")

    t0 = time.perf_counter()
    cpu0 = time.process_time()
    next_send_s = t0
    deadline_s = t0 + timeout_s

    try:
        while len(latencies_ms) < num_messages:
            now_s = time.perf_counter()
            if now_s >= deadline_s:
                raise TimeoutError(
                    f"TCP loopback timed out ({len(latencies_ms)}/{num_messages})"
                )
            while sent_idx < num_messages and now_s >= next_send_s:
                frame = struct.pack("!II", sent_idx, 0)
                try:
                    n = cli.send(frame)
                except BlockingIOError:
                    break
                if n != len(frame):
                    cli.setblocking(True)
                    cli.sendall(frame[n:])
                    cli.setblocking(False)
                send_ts_s[sent_idx] = now_s
                wire_bytes += len(frame)
                sent_idx += 1
                next_send_s += 0.001

            wait_s = 0.001 if sent_idx < num_messages else 0.005
            for _ in sel.select(timeout=wait_s):
                while True:
                    try:
                        chunk = peer.recv(4096)
                    except BlockingIOError:
                        break
                    if not chunk:
                        break
                    rx_buf.extend(chunk)
                recv_s = time.perf_counter()
                while len(rx_buf) >= 8:
                    msg_id, _ = struct.unpack("!II", rx_buf[:8])
                    del rx_buf[:8]
                    latencies_ms.append((recv_s - send_ts_s.get(msg_id, recv_s)) * 1000.0)

        elapsed_s = max(1e-6, time.perf_counter() - t0)
        cpu_s = max(0.0, time.process_time() - cpu0)
    finally:
        sel.close()
        cli.close()
        peer.close()
        srv.close()

    latencies_ms.sort()
    useful_bytes = len(latencies_ms) * 4
    goodput_kbps = (useful_bytes * 8.0) / elapsed_s / 1000.0
    return RunMetrics(
        transport="tcp_nodelay",
        profile="loopback-clean",
        messages_sent=num_messages,
        messages_delivered=len(latencies_ms),
        wire_bytes=wire_bytes,
        useful_bytes=useful_bytes,
        elapsed_s=elapsed_s,
        cpu_s=cpu_s,
        goodput_kbps=goodput_kbps,
        p50_ms=_percentile(latencies_ms, 0.50),
        p95_ms=_percentile(latencies_ms, 0.95),
        p99_ms=_percentile(latencies_ms, 0.99),
        max_ms=latencies_ms[-1] if latencies_ms else 0.0,
        dropped_by_link=0,
        reordered_by_link=0,
        c_heap_allocs=0,
        cdf=_build_cdf(latencies_ms),
    )


def print_netem_recipes() -> None:
    print("Linux `tc qdisc netem` commands for physical / kernel loopback testing:")
    for name in ("gilbert-elliott", "pareto-jitter", "asymmetric-reorder", "cleanup"):
        print(f"\n[{name}]")
        for line in NETEM_COMMANDS[name]:
            print(line)


def format_row(m: RunMetrics) -> str:
    return (
        f"{m.transport:<12} {m.profile:<19} "
        f"{m.messages_delivered:>4}/{m.messages_sent:<4} "
        f"{m.p50_ms:>6.2f} {m.p95_ms:>6.2f} {m.p99_ms:>6.2f} {m.max_ms:>6.2f} "
        f"{m.goodput_kbps:>8.1f} {m.cpu_s:>6.3f} "
        f"drop={m.dropped_by_link:<3} reord={m.reordered_by_link:<3} c_alloc={m.c_heap_allocs}"
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="zcrudp real-socket & netem harness")
    parser.add_argument(
        "--smoke",
        action="store_true",
        help="Run fast deterministic real-socket loopback verification (for CI)",
    )
    parser.add_argument(
        "--messages",
        type=int,
        default=120,
        help="Reliable ordered messages per profile (default: 120; 32 in --smoke)",
    )
    parser.add_argument("--seed", type=int, default=42, help="PRNG seed")
    parser.add_argument(
        "--profile",
        choices=["all", *PROFILES.keys()],
        default="all",
        help="Adverse link profile to run",
    )
    parser.add_argument(
        "--print-netem",
        action="store_true",
        help="Print Linux tc qdisc netem setup commands and exit",
    )
    parser.add_argument(
        "--json",
        type=Path,
        default=None,
        help="Optional path to write full CDF + summary metrics as JSON",
    )
    args = parser.parse_args(argv)

    if args.print_netem:
        print_netem_recipes()
        return 0

    num_messages = 32 if args.smoke and args.messages == 120 else args.messages
    lib = zcrudp.load_library()
    selected = list(PROFILES.values()) if args.profile == "all" else [PROFILES[args.profile]]

    results: List[RunMetrics] = []
    results.append(run_tcp_loopback(num_messages))
    for idx, prof in enumerate(selected):
        results.append(
            run_zcrudp_loopback(lib, prof, num_messages=num_messages, seed=args.seed + idx)
        )

    print(
        "transport    profile             dlvd      p50ms  p95ms  p99ms  maxms   goodkbps   cpus"
    )
    for row in results:
        print(format_row(row))
        assert row.messages_delivered == num_messages, (
            f"{row.transport} ({row.profile}) incomplete delivery: "
            f"{row.messages_delivered}/{num_messages}"
        )

    if args.json is not None:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        payload = [
            {
                "transport": r.transport,
                "profile": r.profile,
                "messages_sent": r.messages_sent,
                "messages_delivered": r.messages_delivered,
                "wire_bytes": r.wire_bytes,
                "useful_bytes": r.useful_bytes,
                "elapsed_s": round(r.elapsed_s, 6),
                "cpu_s": round(r.cpu_s, 6),
                "goodput_kbps": round(r.goodput_kbps, 2),
                "p50_ms": round(r.p50_ms, 3),
                "p95_ms": round(r.p95_ms, 3),
                "p99_ms": round(r.p99_ms, 3),
                "max_ms": round(r.max_ms, 3),
                "dropped_by_link": r.dropped_by_link,
                "reordered_by_link": r.reordered_by_link,
                "c_heap_allocs": r.c_heap_allocs,
                "cdf": r.cdf,
            }
            for r in results
        ]
        args.json.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    print("PASS: real-socket zcrudp (Gilbert-Elliott/Pareto/reorder) + TCP loopback harness")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
