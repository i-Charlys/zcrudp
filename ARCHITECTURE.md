# zcrudp architecture

This describes the current implementation in `include/protocol_rudp.h` and
`src/rudp.c`. The application owns sockets, time, buffers and session lifetime.
The core uses fixed-size storage and makes no heap allocation.

## Two explicit wire formats

The original single-context format carries one TFV packet at a time:

```text
Standalone ACK (4 B): seq_num:2 (unused) | ack:2
Frame          (8 B): seq_num:2 | ack:2 | TFV:4
```

The session format adds channels and can bundle records in one UDP datagram:

```text
Datagram (4 + 8*N B)
  header (4 B): ack:2 | ack_channel:1 | count:1
  record (8 B), repeated count times:
    channel_id:1 | flags:1 | seq_num:2 | TFV:4
```

`rudp_datagram_header_s` describes the whole datagram. Each `rudp_record_s`
describes one data item or explicit ACK inside it. `rudp_session_s` is local
memory holding channel state; it is not serialized. An ACK in the datagram
header applies to `ack_channel`; explicit ACK records can acknowledge other
channels. `count == 0` represents a header-only ACK. Both peers must agree on
which format they use; do not select one merely from UDP payload length.
Multi-byte wire fields are written in network byte order. Do not send the raw
in-memory bytes of a C structure, including `tfv_packet_u.raw`.

## State and memory

```text
rudp_session_s
  channels[RUDP_MAX_CHANNELS] : rudp_channel_s
    flags, channel_id, priority, dscp
    unreliable TX/RX sequence state; ACK state
    ctx : rudp_context_s
      tx_buffer[RUDP_WINDOW_SIZE] : rudp_slot_s
        frame : rudp_frame_s = rudp_header_s + tfv_packet_u
        timestamp, state, retries, fast_retransmit, tx_count
      head, tail, TX/RX sequence and ACK state, liveness state
    rx_buffer[RUDP_WINDOW_SIZE] : tfv_packet_u
    rx_present[] : bitmap of occupied reliable RX slots
    timeout_backoffs[] and adaptive RTT/RTO state
  active_channels, rr_cursor
```

With the default 64-slot window and four channels on the checked host ABI:

| Object | Calculation | Size |
| --- | ---: | ---: |
| `rudp_slot_s` | frame 8 + timestamp 4 + four 1-byte fields | 16 B |
| `rudp_context_s` | 64 × 16 + 12 bytes of sequence/state fields + `last_rx_time` 4 | 1,040 B |
| `rudp_channel_s` | 12 bytes before context + 1,040 context + 64 × 4 RX payloads + 8 bitmap + 64 backoff counters + 24 RTT/RTO fields | 1,404 B |
| `rudp_session_s` | 4 × 1,404 + 4 bytes of session fields | 5,620 B |

The header contains `_Static_assert` checks for these sizes. Different compile
settings change the sizes and must match between the library and its users.
Even an unreliable-only session channel reserves the embedded TX/RX arrays.
No heap allocation does not mean no memory cost.

The TX ring uses `head` as the next write position and `tail` as the oldest
unacknowledged position. `head == tail` means empty. Before writing, `rudp_send`
rejects the message if advancing `head` would make it equal to `tail`; that
equality would otherwise also describe a full ring. Thus 64 physical slots hold
at most 63 outstanding reliable messages. ACKs advance `tail` and free slots.

`rx_buffer` has a different role: a session channel retains reliable records
received ahead of a missing sequence. `rx_present` tracks the occupied slots.
The receiver delivers consecutive records with `drain_channel` and advances
the cumulative ACK only on delivery. Unreliable records bypass this buffer and
the reliable TX ring. The original `rudp_recv` discards out-of-order frames;
it does not use the session RX buffer.

## Sending and receiving

```text
Single context:
  rudp_init -> rudp_send -> rudp_get_slot_frame -> rudp_pack_frame -> UDP
  UDP -> rudp_unpack_frame -> rudp_recv -> rudp_recv_ack_ex
  timer -> rudp_tick -> slot indices for application retransmission

Multichannel session:
  rudp_session_init -> rudp_session_config_channel
  rudp_session_send_reliable -> rudp_send -> rudp_session_build_datagram -> UDP
  rudp_session_send_unreliable -----------------------------------------> UDP
  UDP -> rudp_session_process_datagram[_at]
      -> process_datagram -> session_ack -> rudp_recv_ack_ex
                          -> drain_channel -> application records
  rudp_session_poll -> drain_channel (when output capacity was exhausted)
```

ACKs use the next-expected-sequence convention: ACK `N` confirms records before
`N`. Three deliberate duplicate ACKs can request a fast retransmission. Timeouts
use bounded exponential backoff; after the retry limit, the context becomes
disconnected. A reliable ordered channel can wait for a missing record, while
another channel can continue delivering. Shared network capacity and application
scheduling still affect all channels. Adaptive RTT estimation is opt-in via
`rudp_session_config_recovery` and timed receive calls.

## Scope

The core implements neither congestion control nor connection establishment,
authentication, encryption or socket I/O. The `RUDP_CHANNEL_FLAG_ENCRYPTED`
flag does not encrypt data. The integration must coordinate resets and reject
stale traffic from an old session. Optional multipart and scalar profiles live
in `src/profiles.c`; see [Phase 4](docs/PHASE4.md) and
[adaptive recovery](docs/ADAPTIVE_RECOVERY.md).
