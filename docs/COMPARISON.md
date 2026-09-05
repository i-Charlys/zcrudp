# zcrudp: where the small design pays off

`zcrudp` targets tiny, frequent updates with a fixed memory budget. Its strongest
case is a transport core you can inspect, embed and drive from your own loop:
one C source file, two headers, no heap allocation calls, no external core dependencies.

This is an architectural comparison. The repository currently benchmarks zcrudp
codecs only; it does not contain matched ENet, KCP or GNS performance runs.
The protocol definitions below were checked against upstream sources on 2026-09-05.

## What you can verify today

| Property | Default zcrudp build | Evidence |
| --- | --- | --- |
| Core integration | 1 C source + 2 headers | [Source](../src/rudp.c), [headers](../include/protocol_rudp.h) |
| Explicit dynamic allocation in the core | None | Inspect the core; caller owns contexts and buffers |
| Context storage | 1,040 bytes on the measured host ABI | `make test` prints `sizeof` |
| Four-channel session storage | 4,212 bytes on that ABI | Includes four 64-slot TX arrays |
| Reliable capacity | 63 outstanding messages/channel by default | Ring reserves one slot to distinguish full/empty |
| Unified datagram, one TFV update | 12 bytes of UDP payload | 4-byte envelope + 8-byte record |
| Standalone cumulative ACK | 4 bytes of UDP payload | Includes ACK channel |
| Unreliable submission | No TX-window insertion or retransmission | Independent sequence state, bounded freshness filter |
| Reproducible measurements | CSV, SVG and environment log | [Benchmark results](bench/codec.csv) |

Storage excludes application payload history, socket/lwIP buffers, stack frames
and cryptographic wrappers. A non-reliable channel still reserves its embedded
TX array. Zero allocation does not mean zero storage or a measured worst-case
execution-time guarantee.

## Small-message wire comparison

The following is a calculation from wire definitions, **not a benchmark**.
Assumptions: an established session, one 4-byte application message in one UDP
datagram, no compression, encryption, fragmentation or extra queued commands.
Values exclude UDP/IP/link headers and subsequent standalone ACK traffic.

| Encoding | UDP payload for that message | Calculation |
| --- | ---: | --- |
| zcrudp unified reliable or unreliable | **12 B** | 4 B envelope (with one cumulative ACK) + 8 B record |
| ENet reliable command | **12–14 B** | 2–4 B packet header + 6 B command + 4 B payload |
| ENet unreliable sequenced command | **14–16 B** | 2–4 B packet header + 8 B command + 4 B payload |
| KCP PUSH segment | **28 B** | 24 B segment header + 4 B payload |

The ENet packet header has an optional sent-time field. Its reliable and
unreliable commands have different layouts. These sizes follow the
[ENet protocol definitions](https://github.com/lsalzman/enet/blob/master/include/enet/protocol.h).
KCP declares its 24-byte segment overhead in
[ikcp.c](https://github.com/skywind3000/kcp/blob/master/ikcp.c).

For this specific small-message case, zcrudp uses **57% fewer UDP-payload bytes
than a KCP PUSH segment**: `1 - 12/28`. This does not imply 57% less physical-link
traffic, CPU time or delivery latency. KCP carries additional transport fields
and supports a broader payload model. ENet's compact reliable representation is
already close to zcrudp here.

Bundling changes these totals. For zcrudp, N data records occupy `4 + 8*N`
bytes before optional explicit ACK records; those ACK records also occupy 8 bytes.
One 16-record datagram is 132 bytes. Separate datagrams would total 192 bytes
of UDP payload, plus fifteen additional UDP/IP envelopes.

## Pick the transport that matches the job

| Design | Main fit | What the zcrudp specialization buys you |
| --- | --- | --- |
| zcrudp | Small TFV updates, caller-managed memory and scheduling | A bounded core with separate reliable/unreliable state |
| ENet | General game messages, channels and fragmentation | zcrudp narrows the payload model and storage policy |
| KCP | ARQ for application-provided transport I/O | zcrudp includes an unreliable path and channel envelope |
| Valve GameNetworkingSockets | Broader game networking integration | zcrudp keeps its core integration much smaller in scope |
| QUIC | Secure multiplexed transport | zcrudp delegates security and connection management to its integration |

ENet's command set includes channels, unreliable delivery and fragmentation;
see its [protocol definitions](https://github.com/lsalzman/enet/blob/master/include/enet/protocol.h).
KCP supplies ARQ, an output callback and allocator hooks in its
[implementation](https://github.com/skywind3000/kcp/blob/master/ikcp.c).
Valve documents reliable/unreliable messaging, fragmentation, encryption and
P2P features in the [GNS repository](https://github.com/ValveSoftware/GameNetworkingSockets).
QUIC specifies streams, flow control, loss recovery and secure transport in
[RFC 9000](https://www.rfc-editor.org/rfc/rfc9000.html).
QUIC is a protocol, not a single implementation with a universal memory footprint.

## Where the specialization stops

Reliable delivery in a zcrudp channel waits for the missing sequence and discards
ahead-of-order data. Channels have separate sequencing, but share the host link,
CPU and send budget. The unreliable freshness filter also has a configurable
maximum forward jump; applications must handle long gaps and session resets.

The repository does not demonstrate built-in cryptography, NAT traversal,
congestion control, general bulk-message reassembly or measured STM32 deployment.
The ENCRYPTED capability flag is not an implementation of encryption. These
capabilities matter when choosing a production transport.

Four-byte TFV records can express control messages. They are not evidence of
an efficient bulk KV-cache transfer implementation, nor of superiority to RDMA,
QUIC or TCP for that workload.

## What a defensible speed comparison must measure

A codec microbenchmark and a full transport send path perform different work.
Before publishing a speedup against another library, add a matched two-peer
harness and record:

1. Exact revisions, compiler flags, CPU, memory policy and transport settings.
2. Identical message sizes, send rates and reliability semantics.
3. No-loss and loss/reordering scenarios, equal pacing and network conditions.
4. CPU cost per delivered message, p50/p95/p99 delivery delay, goodput and retransmitted bytes.
5. Peak memory including queues, plus allocation counts after initialization.
6. Both the tiny-message workload and larger payloads that test the specialization's limits.

Current [CSV](bench/codec.csv) and [run log](bench/environment.txt) establish a
reproducible zcrudp codec baseline. Competitor speed and memory ratios remain
unmeasured; the published numerical advantage above is strictly a wire-format
calculation for a stated workload.
