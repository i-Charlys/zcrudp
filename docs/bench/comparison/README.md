# Transport comparison: zcrudp, ENet and KCP

This benchmark runs the actual protocol implementations, not reconstructed codecs.
It measures reliable ordered delivery of 4-byte messages over one shared virtual
datagram-link implementation. ENet's clock and socket backend are replaced; its
protocol engine and KCP's source are unmodified.

## Reproduce

```bash
# Network access is needed on first run to fetch pinned source archives.
# Python 3.12+, curl and a C compiler are required; plotting uses Matplotlib.
make compare COMPARE_PYTHON='uv run --with matplotlib==3.11.1 python'

# No plotting dependency needed for the integration/regression checks:
make test-compare

# Replot the existing data without rebuilding or rerunning engines:
uv run --with matplotlib==3.11.1 python bench/run_comparison.py --plot-only
```

Alternatively, install Matplotlib in your existing Python environment and run
`make compare`. Neither competitors nor plotting packages become dependencies
of the zcrudp core. Archives stay under the ignored `build/compare-deps/` folder;
SHA-256 checks are enforced before extracting. Original licenses are retained there.

## Controlled workload

- 2,400 messages per run; one established connection, one reliable ordered channel.
- Identical four-byte content, validated exactly once and in order on delivery.
- At most 63 unacknowledged/queued application messages admitted to each engine.
  Internal transport windows and algorithms retain their profile settings.
- Every endpoint gets one service pass per 1 ms simulated tick; no sleeps or sockets.
- Common MTU limit of 1,400 bytes. Engines perform their own bundling and ACKs.
- One-way link delay is 1 ms (local) or 10 ms (WAN). Lossy cases also add
  independent uniform integer jitter of 0..5 ms. There is no bandwidth limit.
- Independent datagram loss applies in both directions, including ACKs and retries.
  Seeds 1..5 reset for each measured run. The PRNG is shared in design, but engines
  emit different datagram sequences and therefore do not lose identical messages.
- Connection establishment is excluded. ENet connects without loss on the
  scenario's delay/jitter path before counters and the loss RNG are reset.
- The runner stops after all messages are delivered and acknowledged, a protocol
  disconnect, or 120 seconds of simulated time. No failed run is silently removed.

| Profile | Transport settings |
| --- | --- |
| zcrudp | Default 64-slot TX ring (63 usable); adaptive recovery explicitly enabled, initial RTO 100 ms, base bounds 10..2,000 ms; only timer expirations increase backoff; unchanged attempt limit; bundler serviced every tick |
| ENet | Default reliability/timeout settings; one peer and one channel; MTU 1,400; no bandwidth cap |
| KCP-default | Default update interval, ARQ and congestion behavior; message mode; MTU 1,400 |
| KCP-fast | `ikcp_nodelay(kcp, 1, 10, 2, 1)`; message mode; other window settings unchanged |

Different update/timeout/congestion policies are deliberate and disclosed.
KCP-fast is included because comparing only to KCP's conservative defaults would
not represent its low-latency configuration. These profiles are not a search for
the optimal configuration of each engine. No congestion-control equivalence is claimed.

## Metrics and graph interpretation

**Goodput:** delivered messages divided by simulated time from first application
submission through last delivery. The saturation cases always offer enough work
to fill the common admission limit. These graphs show protocol behavior under
the given RTT/window/timer constraints, not maximum CPU throughput or NIC capacity.
Multiply messages/s by 32 for application bits/s in this four-byte workload.

**p50/p95/p99 delay:** nearest-rank percentiles of per-message simulated delay.
At 240 Hz the origin is the scheduled generation time, so application backpressure
is included. Saturation-case latency (CSV) starts at actual admission because
there is no offered-rate schedule. Do not compare those two latency origins as
if they were the same workload.

**Datagram traffic:** emitted UDP payload bytes in both directions, including
lost datagrams, retransmissions and ACKs through final acknowledgement. UDP/IP,
Ethernet, encryption and connection setup are excluded. This is not Ethernet airtime.

**Host cost:** `CLOCK_MONOTONIC` wall time per delivered message for the complete
simulation loop, including engines, adapters, heap-queue copies and validation.
Initialization, handshake, final sorting and destruction are excluded. These
host-specific values are not isolated library CPU timings; process scheduling
affects them. No CPU affinity or frequency pinning is applied.

Bars are medians across five runs; whiskers show min/max. Latency graphs use a
logarithmic scale. If any seed fails, that library/scenario has a failure label
and no metric bar: survivor-only latency would give a misleading impression.
The CSV retains delivered counts, elapsed time and partial-run measurements for
diagnosis (`complete=0`). Partial measurements must not be treated as successful
workload completion. Protocol invariant violations abort the runner entirely.

## Results with opt-in adaptive recovery

The no-loss saturation cases show equal delivered goodput for zcrudp and ENet
under the shared admission limit. zcrudp emits fewer UDP-payload bytes for this
small-message workload. KCP results change substantially with its update profile.

All 120 runs complete. Phase 4 removed the original discard/retransmission cascade;
the new profile additionally enables measured RTT and separates timeout backoff
from fast-repair attempts. This deliberately changes the zcrudp recovery policy,
not the offered workload, link, service cadence, admission cap or competitor settings.
The harness changes only configure the profile and supply the receive timestamp.
Both the [pre-RX baseline](../before-phase4/README.md) and
[completed phase-4 baseline](../phase4-baseline/README.md) are preserved.
`make test-compare` gates ten paced-loss runs; `make test-recovery-stress` gates 200.

| 240 Hz scenario | zcrudp p50 / p99 | ENet p50 / p99 | zcrudp / ENet UDP payload bytes |
| --- | ---: | ---: | ---: |
| No loss | 10 / 10 ms | 10 / 10 ms | 38,400 / 57,970 |
| 1% loss + jitter | 13 / 46 ms | 13 / 47 ms | 40,124 / 58,692 |
| 5% loss + jitter | 14 / 57 ms | 15 / 167 ms | 47,248 / 62,096 |

Values are medians over five seeds. The byte counts include both directions and
retries. At 5% loss the adaptive profile has lower p99 than ENet here, while using
less UDP-payload traffic. Compared with phase 4, zcrudp emits about 9% more bytes
and uses another 352 bytes of fixed session state (5,620 bytes total). Success on
these seeds is not proof of recovery under every loss pattern. No independent
ablation claim is made: these results evaluate both recovery changes together.

These findings are evidence of a specific improvement, not a claim that
zcrudp is universally faster. The run does not evaluate real network stacks,
finite-bandwidth congestion, multi-channel traffic, authentication or larger messages.

## Artifacts

- [All 120 runs](results.csv)
- [Versions, checksums, build command and machine](environment.json)
- [Throughput](throughput.svg)
- [p50 latency](latency-p50.svg), [p99 latency](latency-p99.svg) (p95 is in CSV)
- [Datagram cost](wire-cost.svg)
- [Host simulation cost](host-cost.svg)

The source revisions and archive checksums are pinned in
[`run_comparison.py`](../../../bench/run_comparison.py). Implementation references:
[ENet](https://github.com/lsalzman/enet/tree/5a9c537fd464b3c6d3c55e1d3bd47588faf71b42),
[KCP](https://github.com/skywind3000/kcp/tree/b1a7a2101dcbb96017681a500d6b82bbe5a88766).
