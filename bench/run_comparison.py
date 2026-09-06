"""Fetch pinned competitors, build unmodified engines, run and plot comparisons.

Optional host tooling only. Core builds never download third-party dependencies.
"""
import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
DEPS = ROOT / "build/compare-deps"
VERSIONS = {
    "enet": ("lsalzman", "enet", "5a9c537fd464b3c6d3c55e1d3bd47588faf71b42",
             "537d42f31b0c0d27f5a9635f6c224f2451c1c1484bfde50e94c884e96181c290"),
    "enet-zpl": ("zpl-c", "enet", "8b43b92591fa9458662262a598b092ecc5132865",
                 "355d615f088d82ae4f01623a82b2b4d5dce2a504a57ffaea9eb701b2da3e007e"),
    "kcp": ("skywind3000", "kcp", "b1a7a2101dcbb96017681a500d6b82bbe5a88766",
            "291d08cd1fba918645a12078cbb6dfffa0825c57e33c0d0ec093604621bf3292"),
}
FIELDS = "library,seed,messages,offered_rate,delay_ms,loss_percent,delivered,elapsed_sim_ms,goodput_msg_s,p50_ms,p95_ms,p99_ms,wire_bytes,wire_packets,lost_packets,harness_ns_per_delivery,complete".split(",")
LIBRARIES = ["zcrudp", "ENet", "ENet-zpl", "KCP-default", "KCP-fast"]
SCENARIOS = [
    ("saturated-local", 0, 1, 0, 0),
    ("saturated-wan", 0, 10, 0, 0),
    ("saturated-loss", 0, 10, 1, 5),
    ("240hz-clean", 240, 10, 0, 0),
    ("240hz-loss1", 240, 10, 1, 5),
    ("240hz-loss5", 240, 10, 5, 5),
    ("240hz-jitter", 240, 20, 2, 40),
    ("ping250-clean", 240, 125, 0, 0),
    ("ping250-loss", 240, 125, 2, 10),
]


def fetch(name):
    org, repo, revision, digest = VERSIONS[name]
    archive = DEPS / f"{name}.tar.gz"
    if not archive.exists():
        subprocess.run(["curl", "-fsSL", f"https://codeload.github.com/{org}/{repo}/tar.gz/{revision}",
                        "-o", str(archive)], check=True)
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == digest, f"Checksum mismatch: {archive}"
    # Always rebuild sources from the verified archive, never trust a cached edit.
    with tarfile.open(archive) as tar:
        tar.extractall(DEPS, filter="data")
    return DEPS / f"{repo}-{revision}"


def build():
    DEPS.mkdir(parents=True, exist_ok=True)
    enet, enet_zpl, kcp = fetch("enet"), fetch("enet-zpl"), fetch("kcp")
    compiler = os.environ.get("CC", "gcc")
    flags = ["-O2", "-std=c11", "-fno-lto", "-UNDEBUG", "-I" + str(ROOT / "include"),
             "-I" + str(ROOT / "bench"),
             "-I" + str(kcp), "-I" + str(enet / "include"), "-I" + str(enet_zpl / "include")]
    sources = [ROOT / "bench/compare_transport.c", ROOT / "bench/compare_enet_zpl.c",
               ROOT / "src/rudp.c", kcp / "ikcp.c"]
    sources += [enet / f"{name}.c" for name in ("callbacks", "compress", "host", "list", "packet", "peer", "protocol")]
    command = [compiler, *flags, *map(str, sources), "-o", str(ROOT / "build/compare_transport")]
    subprocess.run(command, check=True)
    return command, subprocess.check_output([compiler, "--version"], text=True).splitlines()[0]


def plots(rows, output):
    os.environ.setdefault("MPLCONFIGDIR", str(ROOT / "build/matplotlib-cache"))
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    colors = ["#2563eb", "#0f766e", "#059669", "#64748b", "#d97706"]
    labels = {
        "saturated-local": "Local: 1 ms, 0% loss",
        "saturated-wan": "WAN: 10 ms, 0% loss",
        "saturated-loss": "WAN: 10 ms, 1% loss",
        "240hz-clean": "240 Hz: 10 ms\n0% loss",
        "240hz-loss1": "240 Hz: 10 ms\n1% loss",
        "240hz-loss5": "240 Hz: 10 ms\n5% loss",
        "240hz-jitter": "240 Hz: 20 ms, 2% loss\n±40 ms jitter",
        "ping250-clean": "Ping 250 ms: 125 ms\n0% loss",
        "ping250-loss": "Ping 250 ms: 125 ms\n2% loss + jitter",
    }

    def draw(filename, title, metric, ylabel, scenarios, divisor=1):
        fig, ax = plt.subplots(figsize=(14.5 if len(scenarios) > 3 else 10.5, 5.8), layout="constrained")
        width = 0.15
        for index, library in enumerate(LIBRARIES):
            values, lower, upper, failures = [], [], [], []
            for scenario in scenarios:
                group = [row for row in rows if row["library"] == library and row["scenario"] == scenario]
                failures.append(sum(row["complete"] != "1" for row in group))
                selected = [float(row[metric])/divisor for row in group]
                median = statistics.median(selected) if selected else 0
                values.append(median if not failures[-1] else 0)
                lower.append(median-min(selected) if (selected and not failures[-1]) else 0)
                upper.append(max(selected)-median if (selected and not failures[-1]) else 0)
            positions = [i + (index-2.0)*width for i in range(len(scenarios))]
            bars = ax.bar(positions, values, width, color=colors[index], label=library,
                          yerr=[lower, upper], capsize=2)
            ax.bar_label(bars, labels=["" if n else (f"{v:.3f}" if v < 1 else f"{v:.1f}")
                                      for n, v in zip(failures, values)], fontsize=7, padding=3)
            for x, n in zip(positions, failures):
                if n:
                    ax.text(x, .03, f"{n} failed", transform=ax.get_xaxis_transform(), ha="center", fontsize=7, color=colors[index])
        ax.set_xticks(range(len(scenarios)), [labels[s] for s in scenarios], fontsize=8.5)
        ax.set_ylabel(ylabel); ax.set_title(title, loc="left", weight="bold", pad=42)
        if metric in ("p50_ms", "p99_ms"):
            ax.set_yscale("log")
            ax.set_ylim(bottom=.7)
            ax.set_ylabel(ylabel + " — log scale")
        ax.spines[["top", "right"]].set_visible(False)
        ax.grid(axis="y", alpha=.2); ax.set_axisbelow(True)
        ax.legend(ncols=5, loc="upper center", bbox_to_anchor=(.5, 1.06), frameon=False)
        fig.supxlabel("Real engines, virtual datagram link • 4-byte reliable ordered messages • median over seeds, whiskers=min/max\n"
                      "1 ms cadence • 63-message admission cap • no OS/NIC or bandwidth limit • failed groups have no metric bar", fontsize=9)
        fig.savefig(output / filename, metadata={"Date": None})
        fig.savefig(ROOT / "build" / filename.replace(".svg", ".png"), dpi=120)
        plt.close(fig)

    saturated = [s[0] for s in SCENARIOS[:3]]
    paced = [s[0] for s in SCENARIOS[3:]]
    draw("throughput.svg", "Delivered goodput under saturation — higher is better", "goodput_msg_s", "Thousands of delivered messages / simulated second", saturated, 1000)
    draw("latency-p50.svg", "Median delivery delay at 240 Hz — lower is better", "p50_ms", "p50 delivery delay (simulated ms)", paced)
    draw("latency-p99.svg", "Tail delivery delay at 240 Hz — lower is better", "p99_ms", "p99 delivery delay (simulated ms)", paced)
    draw("host-cost.svg", "Host execution cost of the simulation — lower is better", "harness_ns_per_delivery", "Wall-clock ns / delivered message (engines + adapters + simulator)", saturated)
    normalized = [{**row, "bytes_per_delivery": float(row["wire_bytes"])/int(row["delivered"])} for row in rows]
    original = rows
    rows = normalized
    draw("wire-cost.svg", "Datagram traffic per delivered message — lower is better", "bytes_per_delivery", "UDP payload bytes / delivered message (both directions)", saturated)
    rows = original


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--messages", type=int, default=2400)
    parser.add_argument("--seeds", type=int, default=5)
    parser.add_argument("--output", type=Path, default=ROOT / "docs/bench/comparison")
    parser.add_argument("--no-plots", action="store_true")
    parser.add_argument("--plot-only", action="store_true")
    args = parser.parse_args()
    assert 1 <= args.messages <= 60000 and 1 <= args.seeds <= 100
    if args.plot_only:
        with (args.output / "results.csv").open() as f:
            plots(list(csv.DictReader(f)), args.output)
        return
    command, compiler = build()
    rows = []
    for scenario, rate, delay, loss, jitter in SCENARIOS:
        for engine in range(len(LIBRARIES)):
            for seed in range(1, args.seeds+1):
                run = subprocess.run([str(ROOT / "build/compare_transport"), str(engine), str(args.messages),
                                      str(rate), str(delay), str(loss), str(seed), str(jitter)], capture_output=True, text=True, timeout=60)
                assert run.returncode in (0, 1) and run.stdout, (scenario, engine, seed, run.stdout, run.stderr)
                row = dict(zip(FIELDS, next(csv.reader(io.StringIO(run.stdout)))), scenario=scenario)
                assert (row["complete"] == "1") == (run.returncode == 0)
                assert int(row["delivered"]) <= args.messages
                if row["complete"] == "1":
                    assert int(row["delivered"]) == args.messages
                assert int(row["p50_ms"]) <= int(row["p95_ms"]) <= int(row["p99_ms"])
                if int(row["delivered"]):
                    assert int(row["p50_ms"]) >= delay
                if loss == 0:
                    assert row["lost_packets"] == "0"
                rows.append(row)
            failures = sum(row["complete"] != "1" for row in rows[-args.seeds:])
            print(f"{scenario}: {LIBRARIES[engine]} ({args.seeds-failures}/{args.seeds} completed)", flush=True)
    args.output.mkdir(parents=True, exist_ok=True)
    with (args.output / "results.csv").open("w") as f:
        writer = csv.DictWriter(f, ["scenario", *FIELDS]); writer.writeheader(); writer.writerows(rows)
    cpu = "unknown"
    if Path("/proc/cpuinfo").exists():
        cpu = next((line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines() if line.startswith("model name")), cpu)
    environment = {"host": platform.platform(), "cpu": cpu, "compiler": compiler, "build_command": command,
                   "sources": VERSIONS, "zcrudp_source_sha256": hashlib.sha256((ROOT / "src/rudp.c").read_bytes()).hexdigest(),
                   "zcrudp_header_sha256": hashlib.sha256((ROOT / "include/protocol_rudp.h").read_bytes()).hexdigest(),
                   "harness_sha256": hashlib.sha256((ROOT / "bench/compare_transport.c").read_bytes()).hexdigest(),
                   "enet_zpl_bridge_sha256": hashlib.sha256((ROOT / "bench/compare_enet_zpl.c").read_bytes()).hexdigest(),
                   "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                   "messages": args.messages, "seeds": args.seeds, "simulation": "1ms ticks; unlimited bandwidth; jitter 0..40ms",
                   "kcp_fast": "ikcp_nodelay(1,10,2,1); default window sizes", "zcrudp_rto_ms": 100,
                   "zcrudp_recovery": "adaptive; initial=100ms min=10ms max_base=2000ms; timeout-only backoff"}
    (args.output / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")
    if not args.no_plots:
        plots(rows, args.output)
    print(f"Saved {len(rows)} validated runs to {args.output}")


if __name__ == "__main__":
    main()
