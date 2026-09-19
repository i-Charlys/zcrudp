# Choosing a transport for small messages

zcrudp targets small, frequent TFV updates with caller-owned, bounded storage.
Its core is C11, has no heap allocation or socket calls, and lets an application
drive its own clock and event loop. This document separates properties of its
implementation from measurements and comparisons with other transports.

## What the source establishes

| Property | Default zcrudp build | Where to check |
| --- | --- | --- |
| Core integration | `src/rudp.c` and two headers | [Source](../src/rudp.c), [definitions](../include/protocol_rudp.h) |
| Optional multipart and scalar profiles | Separate `src/profiles.c` library | [Profile definitions](../include/protocol_profiles.h) |
| Explicit heap allocations in the core | None | [Source](../src/rudp.c) |
| Context storage on the checked ABI | 1,040 B | `sizeof` assertions in [the header](../include/protocol_rudp.h) |
| Four-channel session storage | 5,620 B | Same assertions; includes TX/RX and recovery arrays even for unreliable channels |
| Reliable TX capacity | 63 outstanding messages per channel | 64-slot ring with one slot reserved to distinguish full from empty |
| Single-context wire format | 4 B ACK or 8 B frame | [Serialization functions](../src/rudp.c) |
| Session wire format | 4 B datagram header + 8 B per record | Same serialization functions |
| Unreliable submission | No reliable TX slot or retransmission | `rudp_session_send_unreliable` |

These sizes describe the default build on the checked ABI, not a universal
memory footprint. The application also supplies socket buffers, output arrays,
timers and any security layer. No heap allocation in this library does not
guarantee bounded execution time for the whole application.

## What it does and does not isolate

Each channel has its own reliable state. A missing record on one ordered channel
does not prevent the library from delivering a fresh unreliable record on another.
Later records **on the same ordered channel** wait for the gap to be filled.
Channels still share the application's CPU time, socket, link capacity and
datagram budget. The original single-context `rudp_recv` drops out-of-order
frames; session channels retain them in a bounded RX window. ACKs are cumulative,
so retained records can still be retransmitted.

The core does not implement connection establishment, authentication,
encryption, congestion control, flow control or socket I/O. The
`RUDP_CHANNEL_FLAG_ENCRYPTED` flag is metadata, not encryption. The application
must coordinate resets and handle exhausted retry budgets.

## Small-message wire comparison

For **one** 4-byte application value in an established connection, excluding
UDP/IP/link headers, encryption and later ACK traffic:

| Encoding | UDP payload | Calculation |
| --- | ---: | --- |
| zcrudp session record | 12 B | 4 B datagram header + 8 B record |
| zcrudp original frame | 8 B | 4 B frame header + 4 B TFV |
| ENet reliable command | 12–14 B | 2–4 B packet header + 6 B command + 4 B payload |
| ENet unreliable sequenced command | 14–16 B | 2–4 B packet header + 8 B command + 4 B payload |
| KCP PUSH segment | 28 B | 24 B segment header + 4 B payload |

ENet's command and packet layouts are in its
[protocol definitions](https://github.com/lsalzman/enet/blob/master/include/enet/protocol.h).
KCP declares a 24-byte segment overhead in
[ikcp.c](https://github.com/skywind3000/kcp/blob/master/ikcp.c).
For this isolated message, the zcrudp session format uses `1 - 12/28`, or about
57%, fewer **UDP payload bytes** than a KCP PUSH segment. This is a wire-layout
calculation, not a measurement of physical traffic, CPU cost or latency.
Bundling, ACKs, retransmissions and transport settings change the result.

## Measurements in this repository

The [transport comparison](bench/comparison/README.md) documents virtual-link
experiments against ENet, ENet-zpl and KCP, with workload settings and raw CSV
data. These results apply to the specified simulated links and versions; they
are not physical-network or all-workload rankings. The
[codec measurements](bench/codec.csv) time serialization on one host and cannot
be compared directly with whole-transport latency or another library's
unmatched benchmark.

GNS and QUIC have **not** been benchmarked here. QUIC specifies secure,
multiplexed transport with streams, flow control and connection migration;
see [RFC 9000](https://www.rfc-editor.org/rfc/rfc9000.html). zcrudp's narrower
message and storage model may suit an application that already owns the network
integration. Applications needing QUIC's broader capabilities should evaluate
an implementation of QUIC rather than infer equivalence from header sizes.
