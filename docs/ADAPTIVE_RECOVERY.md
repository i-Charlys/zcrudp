# Adaptive recovery without a wire-format extension

This opt-in session profile adds fixed-point RTT estimation and separates
timeout backoff from fast retransmission attempts. It does not add SACK, FEC,
congestion control, receive credit, or a new wire format. Legacy context APIs
and the default fixed-timeout session mode retain their timing policy.

## Integration

```c
rudp_session_init(&session);
rudp_session_config_channel(&session, 0,
    RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED);
/* Check the return values in application code. */
rudp_session_config_recovery(&session, 0, 100, 10, 2000);

/* Receive: use a monotonic, wrapping uint32_t millisecond clock. */
int count = rudp_session_process_datagram_at(&session, bytes, length,
                                            records, capacity, now_ms);
/* Consume delivered records; use session_poll when the output fills. */

/* Send: queue with session_send_reliable, then use the session bundler. */
int length_out = rudp_session_build_datagram(&session, 0, bytes, sizeof bytes,
                                            now_ms, 100);
```

Configure only when both channel TX and RX windows are empty. Bounds obey
`1 <= minimum <= initial <= maximum <= 60000` milliseconds. The maximum bounds
the **base** RTO, not its exponentially backed-off value. Values 100/10/2000
are the published benchmark profile, not universal optimal settings.

With adaptive mode enabled, the bundler's timeout argument is ignored for that
channel (even zero). Untimed receive still processes data and ACKs but cannot
learn RTT. Do not also call legacy `rudp_tick()` on a channel serviced by the
bundler: the legacy API neither owns the new recovery state nor knows when a
custom integration actually sent a frame.

`reset_channel` preserves configured bounds but clears samples/counters and
sets base RTO to the configured maximum, conservatively. To restore a chosen
initial value, reconfigure after reset. `session_init` returns to fixed mode.
As before, externally serialize access; coordinate resets with the peer.

## Sampling and timers

- Reuse the existing slot timestamp, written at initial bundler encoding.
  Time includes subsequent socket/simulator queuing and remote processing;
  it is not a pure physical propagation measurement.
- Accept only valid progressing cumulative ACKs. Skip an entire newly ACKed
  range if it includes an unsent or retransmitted slot (conservative Karn rule).
- Select the newest message in the eligible range, at most once per current
  smoothed RTT. Duplicate, future, malformed or untimed ACKs do not train RTT.
- First sample initializes SRTT to R and deviation to R/2. Subsequent updates
  use integer fixed-point SRTT x8 and deviation x4, with weights 1/8 and 1/4.
- Base RTO is SRTT + max(1 ms, 4*deviation), clamped to configured bounds.
  Zero-millisecond samples are conservatively treated as one millisecond;
  samples exceeding 60 seconds are ignored. Clock differences are unsigned.
- Every retransmission still counts toward the existing retry limit. Only a
  timer-triggered retransmission increases the per-slot timeout exponent.
  Fast repair does not inflate it. Exponents remain capped at six.
- Fixed-mode bundler timeout multiplication now saturates instead of wrapping
  for very large caller values. The legacy context tick is otherwise unchanged.

Timeout expiry remains strict `elapsed > timeout`. No new repeated-fast-repair
trigger is introduced; a lost fast repair can use the adaptive timeout fallback.
Application backpressure and cumulative ACK ambiguity can still bias or delay
samples. This is not an implementation of the full TCP timer standard or RACK.

## Memory and compatibility

TX slots remain 16 bytes, context remains 1,040 bytes at window 64. Each channel
adds 64 timeout-exponent bytes and six uint32_t estimator/configuration fields:
**88 bytes/channel, 352 bytes/session**, statically reserved even in fixed mode.
Default channel/session sizes are now **1,404 / 5,620 bytes**. For other window
sizes, counter storage scales with `RUDP_WINDOW_SIZE`. Rebuild all consumers:
the in-memory ABI changes, but the UDP wire format does not.

## Results and tradeoffs

Published medians over five seeds, 2,400 four-byte reliable ordered messages,
240 Hz, 10 ms one-way delay and 0..5 ms jitter in lossy cases:

| Metric | Phase 4 fixed | Adaptive | ENet |
| --- | ---: | ---: | ---: |
| p99, 1% loss | 52 ms | 46 ms | 47 ms |
| p99, 5% loss | 263 ms | 57 ms | 167 ms |
| UDP payload bytes, 5% loss | 43,324 | 47,248 | 62,096 |

At 5% loss this is about 78% lower p99 than phase 4 and 66% lower than ENet,
but about 9% more traffic than phase 4. These are combined-policy results,
not separate attribution of gains to RTT versus backoff. The no-loss saturation
goodput remains equal to ENet under the common admission limit. There is no
claim about physical NIC throughput, burst-correlated loss or congestion.

`make test` and `make asan` include sampling, Karn rejection, malformed/future
ACKs, clock wrap, timer boundaries, lost fast repair, reset and fixed-mode checks.
`make test-recovery-stress` exercises seeds 1..100 for both 1% and 5% loss.
`make test-tools` checks real UDP peers in both fixed and adaptive modes.
The demo accepts `--adaptive 1`; `--timeout` sets its initial RTO (bounds 10..60000).

[Current data and graphs](bench/comparison/README.md) ·
[Preserved phase-4 results](bench/phase4-baseline/README.md).
