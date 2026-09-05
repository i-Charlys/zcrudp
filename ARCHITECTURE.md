# zcrudp Architecture

This document describes the design and implementation details of the `zcrudp` (Zero-allocation C Reliable UDP) protocol.

## Protocol Design

The protocol operates with zero dynamic memory allocation, targeting high-performance game network state synchronization and embedded devices.

### Multi-Tier Frame Format

1. **Tier 1 (4-byte Standalone ACK & Control)**:
   - Wire Size: 4 bytes (`rudp_header_s`).
   - Format: `seq_num` (16 bits, 0 for pure ACK) + `ack` (16 bits, expected cumulative sequence number).
   - Functions: `rudp_pack_ack()`, `rudp_unpack_ack()`, `rudp_unpack_header()`.

2. **Tier 2 (8-byte Standard Game Frame)**:
   - Wire Size: 8 bytes (`rudp_frame_s`).
   - Format: 4-byte header (`rudp_header_s`) + 4-byte payload (`tfv_packet_u`).
   - Functions: `rudp_pack_frame()`, `rudp_unpack_frame()`.

3. **Atomic Building Blocks (4-byte Primitives)**:
   - `rudp_pack_header()` / `rudp_unpack_header()`: Serializes / deserializes 4-byte header.
   - `rudp_pack_payload()` / `rudp_unpack_payload()`: Serializes / deserializes 4-byte TFV payload.

### 32-bit TFV (Type-Flags-Value) Payload
Represented by the `tfv_packet_u` union in `include/protocol_tfv.h`:
- **Type**: 8 bits (Command / message opcode)
- **Flags**: 8 bits (Metadata flags)
- **Value**: 16 bits (Signed or unsigned numeric payload / entity ID)

---

## Protocol Data Flow & Symmetry (TX / RX Pipeline)

```text
 SENDER WORKFLOW (Game -> Network)         RECEIVER WORKFLOW (Network -> Game)
 ─────────────────────────────────         ───────────────────────────────────
 1. Game Data: tfv_packet_u                1. Raw Bytes from UDP socket
         │                                         │
         ▼                                         ▼
 2. rudp_send(ctx, packet, now);           2. bytes == 4: rudp_unpack_ack() -> rudp_recv_ack()
    (Assigns seq, ack, queues in tx_buf)      bytes == 8: rudp_unpack_frame() -> rudp_recv()
         │                                         │
         ▼                                         ▼
 3. rudp_pack_frame(&frame, buf, 8);       3. In-order packet delivered to game!
    (Serializes 8B Big-Endian for socket)     (Duplicate/out-of-order safely handled)
```

---

## Sliding Window & Memory Layout

Reliability is managed using a ring buffer of `RUDP_WINDOW_SIZE` slots (default 64, configurable up to 32768 as a power of 2).

## Sliding Window & Memory Layout

Reliability is managed using a ring buffer of `RUDP_WINDOW_SIZE` slots (default 64, configurable up to 32768 as a power of 2).

### Exact Memory Footprint (1,040 Bytes Context / 4,212 Bytes Session)

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                      RUDP_CONTEXT_S (Total: 1040 Bytes)                     │
├─────────────────────────────────────────────────────────────────────────────┤
│  tx_buffer[64] : 64 slots * 16 bytes = 1024 Bytes                           │
│  ─────────────────────────────────────────────────────────────────────────  │
│  head                : uint16_t (2B) - Write index in tx_buffer             │
│  tail                : uint16_t (2B) - Oldest in-flight index in tx_buffer  │
│  current_seq_num     : uint16_t (2B) - Next sequence number to transmit     │
│  expected_seq_num    : uint16_t (2B) - Next in-order sequence expected (RX) │
│  last_ack_received   : uint16_t (2B) - Last seen cumulative ACK (Tri-ACK)   │
│  duplicate_ack_count : uint8_t  (1B) - Fast Retransmit counter (saturating) │
│  state               : uint8_t  (1B) - RUDP_STATE_CONNECTED / DISCONNECTED │
│  last_rx_time        : uint32_t (4B) - Timestamp of last RX (liveness)      │
└─────────────────────────────────────────────────────────────────────────────┘
```

Each slot in `tx_buffer` (`rudp_slot_s`) is strictly aligned to 16 bytes:
- `frame`: 8 bytes (`rudp_header_s` + `tfv_packet_u`)
- `timestamp`: 4 bytes (`uint32_t`, send timestamp in ms)
- `state`: 1 byte (`RUDP_SLOT_FREE` or `RUDP_SLOT_IN_FLIGHT`)
- `retries`: 1 byte (`uint8_t`, count of retransmissions)
- `fast_retransmit`: 1 byte (`uint8_t`, Tri-ACK single-trigger flag)
- `tx_count`: 1 byte (`uint8_t`, wire transmission count; 0 = pending initial TX)

### Multi-Channel Session Hierarchy (`rudp_session_s` - 4,212 Bytes)

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                      RUDP_SESSION_S (Total: 4212 Bytes)                     │
├─────────────────────────────────────────────────────────────────────────────┤
│  channels[4]       : 4 * 1052 bytes = 4208 Bytes                            │
│  active_channels   : uint8_t (1B) - Number of configured channels           │
│  rr_cursor         : uint8_t (1B) - Fair round-robin egress cursor          │
│  reserved[2]       : uint8_t[2] (2B) - Strict 32-bit alignment padding      │
└─────────────────────────────────────────────────────────────────────────────┘
```

Each channel (`rudp_channel_s` - 1,052 bytes) embeds its own dedicated sliding window context plus state flags:
- `flags` (1B) + `channel_id` (1B)
- `last_unreliable_seq` (2B) + `has_unreliable_seq` (1B) + `ack_pending` (1B)
- `last_ack_sent` (2B) + `next_unreliable_seq` (2B) + `reserved` (2B)
- `ctx` (1040B `rudp_context_s`)

---

## Unified Wire Datagram Format

A single UDP datagram can bundle multiple messages across distinct channels up to MTU:

```text
+----------------------------------+---------------------------------------+
| DATAGRAM HEADER (4 Bytes)        | BUNDLED RECORDS (count x 8 Bytes)     |
+-----------------+--------+-------+---------------------------------------+
| ack (2B)        | ack_ch | count | Record 0 (8B) | Record 1 (8B) | ...   |
| (Cumulative ACK)| (1B)   | (1B)  | (Channel A)   | (Channel B)   |       |
+-----------------+--------+-------+---------------+---------------+-------+
```

### Record Format (8 Bytes)
- `channel_id` (1B): Target channel identifier (0 to `RUDP_MAX_CHANNELS - 1`).
- `flags` (1B): `RUDP_RECORD_FLAG_RELIABLE`, `RUDP_RECORD_FLAG_UNRELIABLE`, or `RUDP_RECORD_FLAG_ACK`.
- `seq_num` (2B): 16-bit sequence number (reliable sliding window or unreliable monotonic sequence).
- `payload` (4B): 32-bit structured Type-Flags-Value (`tfv_packet_u`).

---

## Reliability Engine & State Machine

### Cumulative ACKs (N+1 Convention)
The protocol strictly uses cumulative acknowledgments with $N+1$ semantics:
- When a peer receives packet $N$, it sends `ACK = N + 1` (meaning: *"I have received everything up to $N$, and am now expecting $N+1$."*).
- An incoming `ACK = A` slides `tail` forward, freeing all slots with `seq_num < A`.

### Window Floor & Stale ACK Protection
In `rudp_recv_ack_ex()`, delayed or reordered ACKs that fall behind the oldest in-flight packet (`tail`) are rejected:
```c
if ((int16_t)(ack_num - tail_seq) < 0) {
    return RUDP_ERR_OUT_OF_WINDOW;
}
```

### Fast Retransmit (Tri-ACK State Machine)
- **Tri-ACK trigger**: When the window is stalled (`tail != head`) and the same ACK arrives 3 times consecutively (`duplicate_ack_count == 3`), the oldest packet at `tail` is flagged for immediate retransmission (`slot->fast_retransmit = RUDP_FAST_RETRANSMIT_PENDING`).
- **Gap-triggered ACKs**: When an out-of-order packet arrives (sequence gap), the receiver immediately re-arms `ack_pending = 1` to generate duplicate ACKs, enabling prompt Tri-ACK recovery without waiting for RTO timeout.
- **Saturating Counter**: `duplicate_ack_count` saturates at 255 to prevent uint8 wraparound back to 3.
- **Passive ACK Gate**: Piggybacked ACKs on incoming data frames do not count toward Fast Retransmit (RFC 5681).

### Binary Exponential Backoff
Retransmission timeout in `rudp_tick()` and `rudp_session_build_datagram()` applies binary exponential backoff:
$$\text{RTO} = \text{base\_timeout} \times 2^{\min(\text{retries}, 6)}$$

### Dead Peer Detection & Zombie Protection
- Retransmissions are tracked per slot (`slot->retries++`).
- When `retries > RUDP_MAX_RETRIES` (default 10):
  - `ctx->state` transitions to `RUDP_STATE_DISCONNECTED`.
  - `rudp_tick()` returns `rudp_tick_result_s` with `status = RUDP_ERR_DISCONNECTED` while preserving the count of already collected expired indices.
  - Subsequent calls to `rudp_tick()` and `rudp_send()` are inert and return `RUDP_ERR_DISCONNECTED`.
  - To reconnect, the application explicitly calls `rudp_reset(ctx)` or `rudp_session_reset(session)`.

---

## Physical Layer Realities & The "Zero-Wait" Bundling Principle

### The Ethernet Minimum Frame Paradox (84-byte Wire Floor)
Under the IEEE 802.3 Ethernet standard, any MAC frame carrying less than 46 bytes of IP payload is automatically padded with zeros up to 64 bytes (CSMA/CD collision detection floor). Adding the preamble, SFD, and Inter-Packet Gap (IPG) results in an exact physical wire footprint of **84 bytes**:
- 4-byte UDP datagram  -> 32B IPv4/UDP + 14B MAC padding + 20B L1 = **84 bytes** (4.76% wire efficiency).
- 8-byte UDP datagram  -> 36B IPv4/UDP + 10B MAC padding + 20B L1 = **84 bytes** (9.52% wire efficiency).

### Transmission Time vs. Inter-Frame Latency
A common theoretical misconception is to artificially buffer packets across frames to avoid Ethernet hardware padding. The physical timing proves this is an anti-pattern for real-time applications:
- **Wire transmission delay of an 84-byte frame on a 1 Gbps link**:
  $$T_{tx} = \frac{84 \times 8 \text{ bits}}{10^9 \text{ bps}} = 0.672 \;\mu\text{s} \quad (672 \text{ nanoseconds})$$
- **Inter-frame waiting time for the next game tick**:
  - At 60 Hz: $16.67 \text{ ms} = 16,670 \;\mu\text{s}$ ($24,800\times$ slower than sending immediately).
  - At 128 Hz: $7.81 \text{ ms} = 7,810 \;\mu\text{s}$ ($11,620\times$ slower).

### Architectural Principle: "Intra-Tick Bundling, Never Cross-Tick Delay"
`zcrudp` adheres strictly to the **Anti-Nagle Principle**:
1. **Zero Artificial Egress Delay**: An isolated urgent packet is transmitted immediately. Padding insertion is performed in hardware by the Network Interface Card (NIC) at zero CPU cost and sub-microsecond wire latency.
2. **Intra-Tick Bundling Only**: MTU batching (up to 1400 bytes) aggregates *only* messages produced synchronously within the *same game frame / tick* (e.g. 1 position update + 1 reliable action + 1 ACK). It never delays egress waiting for future game ticks.
