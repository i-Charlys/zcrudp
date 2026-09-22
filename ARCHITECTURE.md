# zcrudp architecture

This document describes the code currently implemented in
`include/protocol_rudp.h` and `src/rudp.c`. Future wire changes are tracked in
`TODO.md` and are not described here as existing behavior.

The application owns the UDP socket, the clock, output buffers and object
lifetime. The core uses fixed-size storage and performs no heap allocation.

## Protocol layers

zcrudp currently exposes two related interfaces:

```text
Application / optional profiles
              |
      rudp_session_s                 multichannel datagrams
              |
      rudp_context_s                 one reliable sliding window
              |
        byte codecs                  explicit network-byte-order packing
              |
       caller-owned UDP I/O
```

`rudp_context_s` is the small single-window ARQ engine. `rudp_session_s` owns
one context per channel and adds multiplexing, unreliable records, reliable RX
reassembly, scheduling, QoS metadata and adaptive timeout state. The two wire
formats are explicit alternatives; a receiver must know which one it expects.
UDP payload length alone is not a format negotiation mechanism.

## Current wire formats

The single-context API carries one TFV packet at a time:

```text
Standalone ACK (4 B): seq_num:2 (unused) | ack:2
Frame          (8 B): seq_num:2 | ack:2 | TFV payload:4
```

The session API can bundle records from several channels:

```text
Datagram (4 + 8*N B)
  header (4 B): ack:2 | ack_channel:1 | count:1
  record (8 B), repeated count times:
    channel_id:1 | flags:1 | seq_num:2 | TFV payload:4
```

`rudp_datagram_header_s` describes the datagram once. `rudp_record_s` describes
one data item or explicit ACK inside that datagram. It is therefore different
from `rudp_session_s`, which is local protocol state and is never serialized.

The header ACK applies to `ack_channel`. An ACK record can acknowledge another
channel. `count == 0` is a header-only ACK. ACK value `N` means that every
sequence before `N` was received in order and `N` is the next expected value.

All multibyte fields are packed explicitly in network byte order. Raw C
structure memory, including `tfv_packet_u.raw`, is not a portable wire format.

## State and memory

```text
rudp_session_s
  channels[RUDP_MAX_CHANNELS] : rudp_channel_s
    flags, channel_id, priority, dscp
    unreliable TX/RX sequence state and pending ACK state
    ctx : rudp_context_s
      tx_buffer[RUDP_WINDOW_SIZE] : rudp_slot_s
        frame : rudp_frame_s = rudp_header_s + tfv_packet_u
        timestamp, state, retries, fast_retransmit, tx_count
      head, tail, TX/RX sequences, duplicate ACK and liveness state
    rx_buffer[RUDP_WINDOW_SIZE] : tfv_packet_u
    rx_present[] : bitmap of retained reliable RX records
    timeout_backoffs[] and adaptive RTT/RTO state
  active_channels, rr_cursor
```

With the default 64-slot window and four channels on the checked ABI:

| Object | Calculation | Size |
| --- | ---: | ---: |
| `rudp_slot_s` | frame 8 + timestamp 4 + four 1-byte fields | 16 B |
| `rudp_context_s` | `64 * 16 + 16` | 1,040 B |
| `rudp_channel_s` | `align4(1,040 + 12 + 5*64 + ceil(64/8)) + 24` | 1,404 B |
| `rudp_session_s` | `4 * 1,404 + 4` | 5,620 B |

The exact formulas and default-size `_Static_assert` checks live beside the
structures in `protocol_rudp.h`. Changing `RUDP_WINDOW_SIZE`,
`RUDP_MAX_CHANNELS`, compiler ABI or packing options can change public object
sizes. The library and its caller must use matching compile settings. Every
session channel currently reserves its reliable TX and RX arrays, including a
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
occupied positions. `drain_channel()` releases only the contiguous prefix and
advances the cumulative ACK only when records are copied to caller-owned
output. The single-context `rudp_recv()` has no such reassembly buffer and
drops out-of-order frames.

## Send, receive and recovery paths

```text
Single context
  rudp_init
    -> rudp_send -> rudp_get_slot_frame -> rudp_pack_frame -> caller UDP send
    -> caller UDP receive -> rudp_unpack_frame -> rudp_recv -> rudp_recv_ack_ex
    -> rudp_tick -> expired slot indices -> caller retransmission

Multichannel session
  rudp_session_init -> rudp_session_config_channel
    -> rudp_session_send_reliable -> per-channel rudp_send
    -> rudp_session_send_unreliable -> one packed 12-byte datagram
    -> rudp_session_build_datagram -> caller UDP send
    -> caller UDP receive -> rudp_session_process_datagram[_at]
         -> validate the complete datagram before mutation
         -> session_ack -> rudp_recv_ack_ex
         -> retain reliable gaps / filter unreliable sequence
         -> drain_channel -> caller records
    -> rudp_session_poll -> drain records left by output backpressure
```

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
| `RUDP_CHANNEL_FLAG_ORDERED` | Metadata only. Reliable session delivery is already always ordered. |
| `RUDP_CHANNEL_FLAG_ENCRYPTED` | Metadata only. No encryption or authentication is performed. |

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

Optional multipart, stream, scalar and motion profiles live in
`src/profiles.c`; their storage is caller-owned. See `docs/PHASE4.md` and
`docs/ADAPTIVE_RECOVERY.md`.
