"""Optional deterministic regression checks for the comparison harness."""
import csv
import io
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "build/compare_transport"


def run(engine, loss):
    result = subprocess.run([str(BIN), str(engine), "200", "0", "10", str(loss), "42"],
                            text=True, capture_output=True, timeout=20)
    assert result.returncode == 0, (result.stdout, result.stderr)
    row = next(csv.reader(io.StringIO(result.stdout)))
    assert int(row[6]) == 200 and row[16] == "1"
    assert 10 <= int(row[9]) <= int(row[10]) <= int(row[11])
    if loss == 0:
        assert row[14] == "0"
    # Ignore measured wall-clock runtime; all simulated results must replay.
    return row[:15] + row[16:]


for engine in range(5):
    for loss in (0, 1):
        assert run(engine, loss) == run(engine, loss)
print("PASS: five engine profiles, reliable ordered delivery, loss, deterministic replay")

# Recovery gate: unchanged workload; adaptive timer policy explicitly enabled in harness.
seeds = int(os.environ.get("RECOVERY_SEEDS", "5"))
assert 1 <= seeds <= 1000
for loss in (1, 5):
    for seed in range(1, seeds + 1):
        result = subprocess.run([str(BIN), "0", "2400", "240", "10", str(loss), str(seed)],
                                text=True, capture_output=True, timeout=30)
        assert result.returncode == 0, (result.stdout, result.stderr)
        row = next(csv.reader(io.StringIO(result.stdout)))
        assert int(row[6]) == 2400 and row[16] == "1", row
print(f"PASS: paced 240 Hz, 2,400 messages, 1%/5% loss, {seeds} seeds each")
