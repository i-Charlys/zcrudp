"""Automated mutation harness: verify core & Phase 5 tests kill safety/protocol mutants."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CC = os.environ.get("CC", "cc").split()
CFLAGS = ["-Iinclude", "-Wall", "-Wextra", "-std=c11", "-pedantic", "-O1", "-UNDEBUG"]
SRC = [
    "src/codec.c", "src/reliability.c", "src/session.c", "src/session_tx.c",
    "src/session_rx.c", "src/rx.c", "src/io.c", "src/crypto.c",
    "src/stream.c", "src/scalar.c", "src/motion.c", "src/nextgen.c",
]
SUITES = [
    "tests/test_phase7.c", "tests/test_phase6.c", "tests/test_phase5.c",
    "tests/test_phase4.c", "tests/test_rudp.c",
]

MUTANTS = [
    (
        "crypto-tag-bypass",
        "src/crypto.c",
        "uint32_t ok = ct_tag_equal(computed_tag, tag);",
        "uint32_t ok = 1U; (void)ct_tag_equal(computed_tag, tag);",
    ),
    (
        "crypto-replay-bypass",
        "src/crypto.c",
        "if ((ctx->rx_replay_bitmap >> diff) & UINT64_C(1)) {",
        "if (0 && ((ctx->rx_replay_bitmap >> diff) & UINT64_C(1))) {",
    ),
    (
        "sack-unsent-validation-bypass",
        "src/session_rx.c",
        "if (s->state == RUDP_SLOT_FREE || s->tx_count == 0U) {",
        "if (s->state == RUDP_SLOT_FREE) {",
    ),
    (
        "fack-multiloss-threshold-broken",
        "src/session_rx.c",
        "s->retries == 0U && sacked_above >= 3U) {",
        "s->retries == 0U && sacked_above >= 99U) {",
    ),
    (
        "sack-karn-retransmit-filter-bypass",
        "src/session_rx.c",
        "if (b == highest_sack_bit && timed && chan->rto_min_ms > 0U &&\n            s->tx_count == 1U && s->retries == 0U) {",
        "if (0 && b == highest_sack_bit && timed && chan->rto_min_ms > 0U &&\n            s->tx_count == 1U && s->retries == 0U) {",
    ),
    (
        "fec-reconstruction-broken",
        "src/rx.c",
        ".payload = rudp_payload_xor(parity_rec->payload, known),",
        ".payload = parity_rec->payload, /* broken xor */",
    ),
    (
        "unordered-dedup-bypass",
        "src/rx.c",
        "if (channel->rx_present[slot / 8] & mask) {\n        channel->ack_pending = 1;",
        "if (0 && (channel->rx_present[slot / 8] & mask)) {\n        channel->ack_pending = 1;",
    ),
    (
        "nextgen-aba-gen-check-bypass",
        "src/nextgen.c",
        "if (!d->allocated || d->release_pending || d->gen != extent.gen) {",
        "if (!d->allocated || d->release_pending) {",
    ),
    (
        "nextgen-sliding-fec-xor-broken",
        "src/nextgen.c",
        "rhs ^= gf256_mul_u32(rx->payloads[idx].raw, c);",
        "rhs ^= 0U; /* broken gf256 */",
    ),
]


def build_and_run(work: Path) -> bool:
    for idx, suite in enumerate(SUITES):
        exe = work / f"suite_{idx}"
        sources = SRC if "phase" in suite else SRC[:8]
        extra = ["tests/fuzz_rudp.c"] if suite.endswith("test_phase6.c") else []
        cmd = [*CC, *CFLAGS, "-I" + str(work / "include"),
               *[str(work / s) for s in sources], str(work / suite),
               *[str(work / e) for e in extra], "-o", str(exe)]
        res = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if res.returncode != 0:
            return False
        run = subprocess.run([str(exe)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if run.returncode != 0:
            return False
    return True


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="zcrudp-mut-") as tmpdir:
        work = Path(tmpdir)
        for sub in ("include", "src", "tests"):
            shutil.copytree(ROOT / sub, work / sub)
        assert build_and_run(work), "Unmutated baseline failed in mutation harness"
        killed = 0
        for name, rel_path, needle, replacement in MUTANTS:
            target = work / rel_path
            original = target.read_text()
            assert needle in original, f"Mutant anchor missing for {name}"
            target.write_text(original.replace(needle, replacement, 1))
            survived = build_and_run(work)
            target.write_text(original)
            assert not survived, f"SURVIVED MUTANT: {name}"
            killed += 1
        print(f"PASS: {killed}/{len(MUTANTS)} protocol & security mutants killed")


if __name__ == "__main__":
    main()
