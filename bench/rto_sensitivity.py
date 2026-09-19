"""Measure and plot the initial-RTO tradeoff on the 250 ms RTT scenarios."""
import csv
import io
import statistics
import subprocess

from run_comparison import FIELDS, ROOT, build

OUTPUT = ROOT / "docs/bench/comparison"
SCENARIOS = (("ping250-clean", 0, 0), ("ping250-loss", 2, 10))
RTOS = (100, 300)
SEEDS = range(1, 6)


def measure():
    build()
    rows = []
    for scenario, loss, jitter in SCENARIOS:
        for rto in RTOS:
            for seed in SEEDS:
                command = [str(ROOT / "build/compare_transport"), "0", "2400", "240",
                           "125", str(loss), str(seed), str(jitter), str(rto)]
                run = subprocess.run(command, capture_output=True, text=True, timeout=60)
                assert run.returncode == 0, (command, run.stdout, run.stderr)
                row = dict(zip(FIELDS, next(csv.reader(io.StringIO(run.stdout)))))
                assert row["complete"] == "1" and int(row["delivered"]) == 2400
                rows.append({"scenario": scenario, "initial_rto_ms": rto, **row})
    OUTPUT.mkdir(parents=True, exist_ok=True)
    with (OUTPUT / "rto-sensitivity.csv").open("w", newline="") as file:
        writer = csv.DictWriter(file, ["scenario", "initial_rto_ms", *FIELDS])
        writer.writeheader()
        writer.writerows(rows)
    return rows


def plot(rows):
    import os
    os.environ.setdefault("MPLCONFIGDIR", str(ROOT / "build/matplotlib-cache"))
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    with (OUTPUT / "results.csv").open() as file:
        original = list(csv.DictReader(file))
    fig, axes = plt.subplots(2, 2, figsize=(11, 7.5), layout="constrained")
    profiles = (("zcrudp · 100 ms", "#2563eb", 100),
                ("zcrudp · 300 ms", "#7c3aed", 300),
                ("ENet · default", "#0f766e", None))
    for row_index, (scenario, _, _) in enumerate(SCENARIOS):
        for column, (metric, title, unit) in enumerate((
                (lambda row: float(row["p99_ms"]), "p99 delivery delay", "ms"),
                (lambda row: float(row["wire_bytes"]) / int(row["delivered"]),
                 "UDP payload per delivery", "B/message"))):
            ax = axes[row_index, column]
            values = []
            for label, color, rto in profiles:
                group = ([row for row in rows if row["scenario"] == scenario and
                          int(row["initial_rto_ms"]) == rto] if rto is not None else
                         [row for row in original if row["scenario"] == scenario and
                          row["library"] == "ENet"])
                assert len(group) == 5 and all(row["complete"] == "1" for row in group)
                values.append(statistics.median(metric(row) for row in group))
            limit = max(values) * 1.22
            for index, ((label, color, _), value) in enumerate(zip(profiles, values)):
                ax.barh(index, value, color=color, height=.66)
                display = f"{value:.0f} ms" if unit == "ms" else f"{value:.1f} B"
                ax.text(value + limit*.015, index, display, va="center", fontsize=9)
            ax.set_yticks(range(3), [profile[0] for profile in profiles], fontsize=9)
            ax.invert_yaxis()
            ax.set_xlim(0, limit)
            ax.set_title(("250 ms ping · 0% loss" if row_index == 0 else
                          "250 ms ping · 2% loss, 0–10 ms jitter") + "\n" + title,
                         loc="left", fontsize=10, weight="bold")
            ax.set_xlabel(unit)
            ax.grid(axis="x", alpha=.2)
            ax.set_axisbelow(True)
            ax.spines[["top", "right"]].set_visible(False)
    fig.suptitle("Initial retransmission timeout: cost and recovery tradeoff",
                 fontsize=15, weight="bold")
    fig.supxlabel("2,400 reliable 4-byte messages at 240 Hz · five seeds · virtual link · lower is better",
                  fontsize=9)
    fig.savefig(OUTPUT / "rto-sensitivity.svg", metadata={"Date": None})
    fig.savefig(ROOT / "build/rto-sensitivity.png", dpi=120)
    plt.close(fig)


if __name__ == "__main__":
    plot(measure())
