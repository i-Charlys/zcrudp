# Phase 4: bounded receive and optional profiles

The core remains C11, caller-owned, and free of heap allocation. Compile
`src/rudp.c` as before; add `src/profiles.c` and `protocol_profiles.h` only when
using multipart or scalar profiles. No background thread or implicit socket I/O
is introduced. These APIs require external serialization per session/context.

## Reliable receive window

Each session channel now stores `RUDP_WINDOW_SIZE` four-byte payloads and a
presence bitmap. Sequence distance bounds slot reuse; duplicate copies never
overwrite a retained payload. Delivery remains exactly-once and ordered within
a session lifetime, with separate sequence spaces across channels. Reset must
be coordinated with the peer and stale traffic excluded by the integration.

ACKs remain cumulative, indicating the next **application-delivered** sequence.
This is selective out-of-order storage, **not a selective-ACK wire extension**;
senders can still retransmit retained records. The legacy `rudp_recv()` context
API retains its old behavior; use the session API for RX buffering.

`rudp_session_process_datagram()` may deliver buffered records as well as records
from the current datagram. When its output array fills, call
`rudp_session_poll()` repeatedly until zero, consuming each returned batch, then
emit pending ACKs. This also drains data without another incoming UDP packet.
Backpressure never advances an ACK past undelivered data. A peer/application
that stops draining can still exhaust the sender's retry budget.

At phase-4 completion, the host ABI was: context 1,040 B; channel 1,316 B; session
5,268 B (previously 4,212 B). The additional 1,056 B is fixed RX storage, not
heap memory. Unreliable-only session channels still reserve the static arrays;
standalone scalar profiles below avoid constructing a session altogether.
This changes the in-memory ABI: rebuild all users with matching configuration.
The unified wire format is unchanged and interoperates with older receivers.
The subsequent [adaptive recovery implementation](ADAPTIVE_RECOVERY.md) adds
88 B per channel: the current session is 5,620 B, with unchanged 16 B TX slots.

## Priority and socket QoS

`rudp_session_set_qos(session, channel, priority, dscp)` sets an 8-bit priority
(smaller is more urgent; default 128) and a DSCP hint (0..63). The bundler sends
pending ACKs first, then eligible reliable records in strict priority order.
Ties rotate between datagrams. Lower-priority traffic can starve under continuous
urgent load; this is not weighted fair queuing. Priority applies before encoding,
not to datagrams already handed to a driver or the simulated delay queue.

The socket owner applies DSCP. The POSIX demo supports `--dscp 46`, using
`setsockopt(IPPROTO_IP, IP_TOS)` with `46 << 2` for that socket. With mixed-channel
bundles, a single UDP/IP datagram has one traffic class: choose the most urgent
represented channel's hint, or separate classes into different sockets. Core
metadata alone does not modify a socket. Networks may ignore/rewrite markings;
no bandwidth or latency guarantee is implied.

## Tier 3 multipart stream

Use one dedicated **reliable + ordered** channel per stream. A TFV descriptor
`type=0xff, flags=0xd3, value=byte_length` precedes `ceil(byte_length/4)` chunks.
Chunks carry four arbitrary bytes via explicit network-order TFV serialization;
the final chunk is zero-padded and its padding is not delivered. Never serialize
host `packet.raw` memory directly: that would change byte order across hosts.

Initialize `rudp_stream_tx_s tx = {0}`, then call `rudp_stream_start(&tx, data,
length)` (0..65,535 B). Call `rudp_stream_pump(&tx, session, channel, now)` while
servicing the usual network loop. Return 0 means TX backpressure; 1 means every
chunk is copied into TX slots and the source can be released. It does **not**
mean remote delivery. The source must remain valid and unchanged until then.
There is one active message per TX stream; no unrelated TFV messages may be
interleaved on that channel.

Initialize `rudp_stream_rx_s rx = {0}`, assign caller-owned `data` and `capacity`,
and feed only payloads delivered by the session to `rudp_stream_receive()`.
Return 1 announces a complete message in `rx.data` with `rx.length` bytes; read
it before feeding the next descriptor. Zero means still assembling. An oversized
descriptor returns `RUDP_ERR_BUFFER_FULL`; subsequent chunks are drained without
writing, and the same error is returned at their end. The following descriptor
can then start a new message. Keep the RX buffer/configuration stable while a
message is active. On transport reset, reset both stream states as well.

This supports bounded message reassembly, not resumable files or arbitrary-size
bulk transfer. Floats require an application-defined representation (for example
IEEE binary32 bytes with explicit endianness); C11 alone does not guarantee it.

## Explicit standalone unreliable profiles

Bind the profile and its scalar meaning out of band: a dedicated socket/port or
an enclosing application's explicit discriminator. **Never infer profile from
4/8-byte length**: those lengths collide with legacy ACK/frame formats. These
profiles do not enter `rudp_session_process_datagram()` and do not piggyback ACKs.
Both peers must agree on profile, reset, sampling period and scalar units.

| Profile | Network-order fields | API |
| --- | --- | --- |
| Compact4 | seq16, value16 | `rudp_scalar_encode/decode(..., false, ...)` |
| Rolling8 | seq16, value16, delta16, previous_valid16 | `rudp_scalar_encode/decode(..., true, ...)` |

A compact scalar is **not an unchanged four-byte TFV**: its semantic type/flags
are supplied by the binding, leaving room for a real 16-bit sequence. Sequence
ordering uses the modular half-range rule; reset/rebind after ambiguous gaps
(32,768 sample intervals), and never mix profiles mid-stream. There is no
authentication, replay protection across sessions, or lossless delivery promise.

Rolling8 stores `delta = current - previous` modulo 65,536, with control 0 on the
first sample and 1 thereafter. The decoder can output the missing previous
sample followed by the current one; provide space for two samples. Recovery
happens **when the next datagram arrives**, without a retransmission round trip,
not at zero elapsed time. Longer loss bursts leave earlier samples missing.
Late/duplicate datagrams produce no output; insufficient output capacity does
not change sequence state, so the caller may retry the same bytes.

## Adaptive redundancy

`rudp_motion_encode()` produces one or two identical Rolling8 datagrams in a
caller-provided 16-byte output array. Emit each eight-byte piece separately.
The scalar history estimates modular velocity, acceleration and jerk at a fixed
application sampling period. After warmup, crossing either configured magnitude
threshold upward requests a duplicate; zero disables that threshold. The receiver
deduplicates the identical sequence numbers. No ACK or TX window is involved.

This heuristic adds traffic at selected transitions; it does not guarantee smooth
motion. Correlated loss may remove both copies. It is scalar-domain arithmetic,
not a physics engine; thresholds require application tuning. Its benefit has
unit coverage but is **not measured by the reliable-only ENet/KCP benchmark**.

## Verification and measured scope

`make test` includes RX wraparound/gaps/duplicates/backpressure/reset/isolation,
priority ordering, and all profiles. Multipart tests include 65,535-byte messages,
reversed chunks, data/ACK loss, backpressure and bounded oversized-message discard.
`make asan` covers the same C tests under ASan/UBSan. `make test-tools` exercises
real POSIX UDP peers and DSCP option handling. `make test-compare` gates the ten
2,400-message, 240 Hz, 1%/5% loss scenarios with the original five seeds.

There is no datacenter, RDMA, congestion-control, security or production-readiness
certification implied by completion of this phase.
