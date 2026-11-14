# zcrudp architecture

This document describes the code currently implemented in
the public module headers and the implementation modules in `src/`.
`protocol_rudp.h` remains a compatibility facade. See [MODULES.md](docs/MODULES.md)
for link dependencies, source lists and the optional caller-owned RX storage ABI. Future wire changes are tracked in
`TODO.md` and are not described here as existing behavior.

The application owns the UDP socket, the clock, output buffers and object
lifetime. The core uses fixed-size storage and performs no heap allocation.

## Protocol layers

zcrudp currently exposes two related interfaces:

Applications and optional profiles call the multichannel session API or the
single-context reliability API. Both use explicit byte codecs; the caller provides
UDP I/O.

`rudp_context_s` is the small single-window ARQ engine. `rudp_session_s` owns
one context per channel and adds multiplexing, unreliable records, reliable RX
reassembly, scheduling, QoS metadata and adaptive timeout state. The two wire
formats are explicit alternatives; a receiver must know which one it expects.
UDP payload length alone is not a format negotiation mechanism.

## Current wire formats

The single-context API carries one TFV packet at a time:

| Standalone item | Wire fields in order | Size |
| --- | --- | --- |
| ACK | Unused sequence (2 B), ACK (2 B) | 4 B |
| Frame | Sequence (2 B), ACK (2 B), TFV payload (4 B) | 8 B |

The session API can bundle records from several channels:

| Session item | Wire fields in order | Size |
| --- | --- | --- |
| Header | ACK (2 B), ACK channel (1 B), record count (1 B) | 4 B |
| Record | Channel (1 B), flags (1 B), sequence (2 B), TFV payload (4 B) | 8 B |

A datagram contains one header and `count` records: `4 + 8 * count` bytes.

`rudp_datagram_header_s` describes the datagram once. `rudp_record_s` describes
one data item or explicit ACK inside that datagram. It is therefore different
from `rudp_session_s`, which is local protocol state and is never serialized.

The header ACK applies to `ack_channel`. An ACK record can acknowledge another
channel. `count == 0` is a header-only ACK. ACK value `N` means that every
sequence before `N` was received in order and `N` is the next expected value.

All multibyte fields are packed explicitly in network byte order. Raw C
structure memory, including `tfv_packet_u.raw`, is not a portable wire format.

## State and memory

A `rudp_session_s` contains `channels[RUDP_MAX_CHANNELS]`, `active_channels`
and `rr_cursor`. Each `rudp_channel_s` holds channel flags, ID, priority, DSCP,
unreliable sequence state, pending ACK state and a `rudp_context_s`.

The context owns `tx_buffer[RUDP_WINDOW_SIZE]` of `rudp_slot_s`, ring indices,
sequence numbers, duplicate ACK and liveness state. Each slot contains a
`rudp_frame_s` (header and TFV payload), timestamp, state, retries,
`fast_retransmit` and `tx_count`.

Channels also hold reliable RX storage and its presence bitmap, timeout backoffs
and adaptive RTT/RTO state. The default layout embeds RX arrays; external RX mode
attaches caller-owned storage through pointers.

With the default 64-slot window and four channels on the checked ABI:

| Object | Calculation | Size |
| --- | ---: | ---: |
| `rudp_slot_s` | frame 8 + timestamp 4 + four 1-byte fields (packed backoff + fast/FEC flags) | 16 B |
| `rudp_context_s` | `64 * 16 + 16` | 1,040 B |
| `rudp_channel_s` | `align4(1,040 + 12 + 4*64 + ceil(64/8)) + 24` | 1,340 B |
| `rudp_session_s` | `4 * 1,340 + 4` | 5,364 B |

The exact formulas and default-size `_Static_assert` checks live beside the
structures in `rudp_types.h`. Changing `RUDP_WINDOW_SIZE`,
`RUDP_MAX_CHANNELS`, compiler ABI or packing options can change public object
sizes. The library and its caller must use matching compile settings. Every
default-layout session channel reserves its reliable TX and RX arrays, including a
channel used only for unreliable traffic.

### Why the TX ring keeps one slot free

`head` is the next write position and `tail` is the oldest unacknowledged
position. `head == tail` represents an empty ring. If all 64 physical slots
were allowed to fill, the same equality would also represent a full ring.
`rudp_send()` therefore refuses the write whose next `head` would equal
`tail`. A 64-slot ring can hold 63 outstanding reliable messages. This avoids
an extra occupancy counter and keeps the empty/full test constant and local.

### RX storage is different

`rx_buffer` is local reassembly memory. A session channel stores reliable
records that arrived ahead of a missing sequence; these bytes are not a second
UDP payload and are not used by unreliable delivery. `rx_present` marks the
occupied positions. `rudp_rx_drain()` releases only the contiguous prefix and
advances the cumulative ACK only when records are copied to caller-owned
output. The single-context `rudp_recv()` has no such reassembly buffer and
drops out-of-order frames.

## Send, receive and recovery paths

For a single context, initialize with `rudp_init()`, enqueue with `rudp_send()`,
retrieve frames with `rudp_get_slot_frame()`, and serialize with `rudp_pack_frame()`.
Decode received frames with `rudp_unpack_frame()` before `rudp_recv()`, which
processes passive ACKs through `rudp_recv_ack_ex()`. `rudp_tick()` returns expired
slot indices for caller-managed retransmission.

For sessions, initialize and configure channels before enqueueing reliable messages.
The unreliable send API packs a single 12-byte datagram; the session builder bundles
reliable records and ACKs. The caller sends the resulting bytes. Reception validates
the complete datagram before ACK processing or state mutation, retains reliable
gaps, filters unreliable sequences and drains contiguous records to caller output.
`rudp_session_poll()` drains records retained by output backpressure.

The session builder emits pending ACKs before data, sorts channels by priority,
rotates equal priorities with `rr_cursor`, and clamps output to
`RUDP_DEFAULT_MTU`. A reliable slot is emitted only when it is new, marked for
fast retransmission, or expired. With fixed recovery, the caller supplies the
timeout. With opt-in adaptive recovery, each channel uses its `rto_ms` and
separate bounded timeout-backoff array.

Three deliberate duplicate ACKs mark the oldest slot for fast retransmission.
Passive duplicate ACK values piggybacked on unrelated data do not increment
that counter. Timeout retransmissions use bounded exponential backoff. Passing
`timeout == 0` in fixed mode deliberately makes every active slot eligible on
each build. Exceeding `RUDP_MAX_RETRIES` disconnects the context.

Adaptive RTT sampling is session-only and opt-in. The caller must use
`rudp_session_process_datagram_at()` with a monotonic millisecond timestamp.
Karn's rule rejects a cumulative RTT sample if any newly acknowledged slot was
retransmitted. Reset preserves configured recovery bounds and resets the
estimate to the configured maximum.

## Channel flags and stored hints

The public flags do not all select implemented behavior:

| Flag | Current effect |
| --- | --- |
| `RUDP_CHANNEL_FLAG_UNRELIABLE` | Value zero: absence of the reliable capability. It cannot be tested as a bit, and the unreliable send API does not inspect it. |
| `RUDP_CHANNEL_FLAG_RELIABLE` | Required by `rudp_session_send_reliable()`. |
| `RUDP_CHANNEL_FLAG_ORDERED` | When set alongside `RUDP_CHANNEL_FLAG_RELIABLE`, delivers reliable records strictly in sequence order. Omitting it on a `RELIABLE` channel selects zero-HoL reliable unordered delivery. |
| `RUDP_CHANNEL_FLAG_ENCRYPTED` | Integration hint for `zcrudp::crypto` (`rudp_crypto.h` ChaCha20-Poly1305 AEAD envelope); base session codecs do not encrypt implicitly. |

The `dscp` field set by `rudp_session_set_qos()` is also a hint: the socket
owner must apply it to outgoing packets. These inert values remain public for
now so current callers compile, while their final semantics or removal are
tracked in `TODO.md`.

## Liveness, restart and security limits

`rudp_touch()` records a valid receive time and `rudp_is_alive()` checks it,
but the current receive functions do not call `rudp_touch()` automatically.
The integration must do so after successful validation. Automatic liveness is
planned in `TODO.md`.

The current wire format has no establishment handshake, random initial
sequence negotiation or authenticated reset. A restarted peer can therefore
require coordinated calls to `rudp_session_reset()`, and the first accepted
unreliable record currently establishes its receive baseline. The forward-jump
bound protects later unreliable records but does not protect that first one.

The base protocol also has no peer authentication, cryptographic integrity,
encryption, congestion controller or socket I/O. Randomized sequence anchors
can reduce blind injection probability, but cannot authenticate a peer that
can observe traffic. Use an authenticated wrapper such as Noise, DTLS or
WireGuard on untrusted networks. See `SECURITY.md` and the concrete follow-up
work in `TODO.md`.

Optional security, transport-nextgen, stream, scalar and motion modules live in
`src/crypto.c` (`zcrudp::crypto`), `src/nextgen.c` (`zcrudp::nextgen`),
`src/stream.c`, `src/scalar.c` and `src/motion.c`; their storage is caller-owned.
Sessions also support intra-tick unreliable batching via `rudp_session_queue_unreliable()`
and `rudp_session_has_pending_work()`, plus multi-language bindings in `bindings/`.
See `docs/PHASE4.md` and `docs/ADAPTIVE_RECOVERY.md`.
