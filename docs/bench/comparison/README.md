# Transport comparison: zcrudp, ENet, ENet (zpl) and KCP

This benchmark runs the actual protocol implementations, not reconstructed codecs.
It measures reliable ordered delivery of 4-byte messages over one shared virtual
datagram-link implementation. ENet and ENet-zpl's clock and socket backend are virtualized;
protocol engines and KCP's source are unmodified.

## Reproduce

```bash
# Network access is needed on first run to fetch pinned source archives.
# Python 3.12+, curl and a C compiler are required; plotting uses Matplotlib.
make compare COMPARE_PYTHON='uv run --with matplotlib python'

# No plotting dependency needed for the integration/regression checks:
make test-compare

# Replot the existing data without rebuilding or rerunning engines:
uv run --with matplotlib python bench/run_comparison.py --plot-only

# Rerun the separate initial-RTO sensitivity experiment and its graph:
uv run --with matplotlib python bench/rto_sensitivity.py
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
- One-way link delay is 1 ms (local LAN), 10 ms (standard WAN), 20 ms (high-jitter WAN),
  or 125 ms (250 ms ping WAN). Jitter is uniformly distributed (0..5 ms for standard WAN,
  0..40 ms for high-jitter, and 0..10 ms for 250 ms ping with loss). There is no bandwidth limit.
- Independent datagram loss applies in both directions, including ACKs and retries.
  Seeds 1..5 reset for each measured run. The PRNG is shared in design, but engines
  emit different datagram sequences and therefore do not lose identical messages.
- Connection establishment is excluded. ENet engines connect without loss on the
  scenario's delay/jitter path before counters and the loss RNG are reset.
- The runner stops after all messages are delivered and acknowledged, a protocol
  disconnect, or 120 seconds of simulated time. No failed run is silently removed.

| Profile | Transport settings |
| --- | --- |
| zcrudp | Default 64-slot TX ring (63 usable); adaptive recovery explicitly enabled, initial RTO 100 ms, base bounds 10..2,000 ms; only timer expirations increase backoff; unchanged attempt limit; bundler serviced every tick |
| ENet | Default reliability/timeout settings; one peer and one channel; MTU 1,400; no bandwidth cap |
| ENet-zpl | Fork of ENet (zpl-c/enet); single-header variant; default reliability/timeout settings; MTU 1,400 |
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
The focused p99 and paced wire-cost charts use separate linear scales for each
scenario to make nearby values legible. They draw zcrudp, ENet and ENet-zpl as
bars and print KCP-fast's value in each panel. KCP-default remains in the full
plots and CSV. Compare numbers across panels, not bar lengths: their scales
differ. The 240 Hz clean cases have equal p99 values, and KCP-fast has the
lowest p99 in the high-jitter case. The paced wire-cost chart also shows that
zcrudp uses more UDP payload bytes than ENet in the clean 250 ms ping case.
The CSV retains delivered counts, elapsed time and partial-run measurements for
diagnosis (`complete=0`). Partial measurements must not be treated as successful
workload completion. Protocol invariant violations abort the runner entirely.

## Results with opt-in adaptive recovery

The no-loss saturation cases show equal delivered goodput for zcrudp, ENet and ENet-zpl
under the shared admission limit. This equality is partly imposed by the 63-message
cap and must not be presented as equal native/default maximum throughput. zcrudp emits
fewer UDP-payload bytes for this small-message workload. KCP results change substantially
with its update profile.

223 of 225 runs complete (KCP-default timed out on 2 of 5 seeds in `ping250-loss` due to conservative window stalls).

| 240 Hz scenario | zcrudp p50 / p99 | ENet p50 / p99 | ENet-zpl p50 / p99 | UDP payload bytes (zcrudp / ENet / ENet-zpl) |
| --- | ---: | ---: | ---: | ---: |
| 10 ms delay, clean | 10 / 10 ms | 10 / 10 ms | 10 / 10 ms | 38,400 / 57,970 / 57,942 |
| 10 ms delay, 1% loss, 5 ms jitter | 13 / 46 ms | 13 / 47 ms | 13 / 48 ms | 40,124 / 58,692 / 58,672 |
| 10 ms delay, 5% loss, 5 ms jitter | 14 / 57 ms | 15 / 167 ms | 14 / 378 ms | 47,248 / 62,096 / 62,596 |
| 20 ms delay, 2% loss, 40 ms jitter | 54 / 189 ms | 58 / 338 ms | 57 / 203 ms | 45,984 / 59,296 / 59,290 |
| Ping 250 ms (125 ms delay), clean | 125 / 125 ms | 125 / 125 ms | 125 / 125 ms | 76,800 / 57,886 / 57,834 |
| Ping 250 ms (125 ms delay), 2% loss, 10 ms jitter | 208 / 379 ms | 410 / 803 ms | 338 / 673 ms | 45,260 / 49,664 / 49,718 |

Values are medians over five seeds. The byte counts include both directions and
retries. Key takeaways:
- At 5% loss, zcrudp's adaptive profile delivers a p99 of 57 ms compared with 167 ms for ENet and 378 ms for ENet-zpl, with lower wire overhead.
- Under heavy jitter (40 ms), zcrudp delivers p99 of 189 ms vs 338 ms for ENet (-44%) while saving 22% datagram payload bytes.
- At 250 ms ping with 2% loss, zcrudp achieves p99 of 379 ms vs 803 ms for ENet (-53%) and 673 ms for ENet-zpl (-44%).
- In clean conditions (no loss), all three engines track the physical link delay exactly (10 ms and 125 ms).

The clean 250 ms ping case exposes a tuning limit of the published zcrudp
profile. Its initial RTO is 100 ms, below the roughly 250 ms ACK round trip.
The sender therefore retransmits before the first ACK arrives. For seed 1,
zcrudp sends 9,600 datagrams and 76,800 UDP payload bytes for 2,400 delivered
messages, versus 4,800 datagrams and 38,400 bytes in the clean 10 ms case.
The four datagrams per message are consistent with original data, its ACK,
an unnecessary retransmission and a duplicate ACK. Retransmitted slots are
excluded from RTT estimation, so this scenario does not teach the adaptive
estimator the longer path delay. Raising the configured initial RTO above the
expected round trip would avoid this particular early timeout; that alternative
configuration is not part of the recorded comparison.

## Initial-RTO sensitivity at 250 ms ping

The [separate sensitivity graph](rto-sensitivity.svg) and
[20 raw zcrudp runs](rto-sensitivity.csv) compare the published 100 ms initial
RTO with 300 ms on the same two 250 ms ping scenarios. Five seeds and the same
2,400-message, 240 Hz workload are used in each cell. ENet's values in the
graph come from the original [results CSV](results.csv).

| Scenario | zcrudp initial RTO | Completed | Median p99 | Median UDP payload bytes / delivered message |
| --- | ---: | ---: | ---: | ---: |
| 0% loss | 100 ms | 5/5 | 125 ms | 32.0 B |
| 0% loss | 300 ms | 5/5 | 125 ms | 16.0 B |
| 2% loss, 0–10 ms jitter | 100 ms | 5/5 | 379 ms | 18.9 B |
| 2% loss, 0–10 ms jitter | 300 ms | 5/5 | 1,429 ms | 9.2 B |

At 300 ms, a clean ACK arrives before the first timeout, eliminating the
unnecessary retransmission and allowing an RTT sample. With actual loss, the
longer starting timer delays repair and increases tail latency. This is a
configuration tradeoff, not evidence that either RTO wins on every path.
The original 225-run dataset and its graphs retain the 100 ms configuration.

These findings are evidence of a specific improvement, not a claim that
zcrudp is universally faster. The run does not evaluate real network stacks,
finite-bandwidth congestion, multi-channel traffic, authentication or larger messages.

## Artifacts

- [All 225 runs](results.csv)
- [Versions, checksums, build command and machine](environment.json)
- [Throughput](throughput.svg)
- [p50 latency](latency-p50.svg), [full p99 latency](latency-p99.svg),
  [focused p99 latency](latency-p99-detail.svg) (p95 is in CSV)
- [Saturation datagram cost](wire-cost.svg), [paced datagram cost](wire-cost-paced.svg)
- [Initial-RTO sensitivity graph](rto-sensitivity.svg) and [raw runs](rto-sensitivity.csv)
- [Host simulation cost](host-cost.svg)

The source revisions and archive checksums are pinned in
[`run_comparison.py`](../../../bench/run_comparison.py). Implementation references:
[ENet](https://github.com/lsalzman/enet/tree/5a9c537fd464b3c6d3c55e1d3bd47588faf71b42),
[ENet (zpl-c)](https://github.com/zpl-c/enet/tree/8b43b92591fa9458662262a598b092ecc5132865),
[KCP](https://github.com/skywind3000/kcp/tree/b1a7a2101dcbb96017681a500d6b82bbe5a88766).
