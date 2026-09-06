# Completed phase-4 baseline (fixed recovery)

These CSV/environment files were preserved before implementing adaptive recovery.
They record all 120 completed runs with bounded RX storage, fixed 100 ms base
timeout and the previous shared retransmission/backoff policy. They are not
overwritten by the current comparison runner.

The current harness explicitly enables adaptive recovery with initial/min/max
base RTO 100/10/2000 ms and passes the receive timestamp to the new API.
The workload, service cadence, admission limit and competitors are unchanged.
Compare simulated metrics, not noisy host-wall-time differences across runs.

[Measured before/after tradeoffs](../../ADAPTIVE_RECOVERY.md).
