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

### Exact Memory Footprint (1036 Bytes)

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                      RUDP_CONTEXT_S (Total: 1036 Bytes)                     │
├─────────────────────────────────────────────────────────────────────────────┤
│  tx_buffer[64] : 64 slots * 16 bytes = 1024 Bytes                           │
│  ─────────────────────────────────────────────────────────────────────────  │
│  current_seq_num     : uint16_t (2B) - Next sequence number to transmit     │
│  expected_seq_num    : uint16_t (2B) - Next in-order sequence expected (RX) │
│  last_ack_received   : uint16_t (2B) - Last seen cumulative ACK (Tri-ACK)   │
│  head                : uint16_t (2B) - Write index in tx_buffer             │
│  tail                : uint16_t (2B) - Oldest in-flight index in tx_buffer  │
│  duplicate_ack_count : uint8_t  (1B) - Fast Retransmit counter              │
│  state               : uint8_t  (1B) - RUDP_STATE_CONNECTED / DISCONNECTED │
└─────────────────────────────────────────────────────────────────────────────┘
```

Each slot in `tx_buffer` (`rudp_slot_s`) is strictly aligned to 16 bytes:
- `frame`: 8 bytes (`rudp_header_s` + `tfv_packet_u`)
- `timestamp`: 4 bytes (`uint32_t`, send timestamp in ms)
- `state`: 1 byte (`RUDP_SLOT_FREE` or `RUDP_SLOT_IN_FLIGHT`)
- `retries`: 1 byte (`uint8_t`, count of retransmissions)
- `fast_retransmit`: 1 byte (`uint8_t`, Tri-ACK single-trigger flag)
- `reserved`: 1 byte (Explicit padding)

---

## Reliability Engine & State Machine

### Cumulative ACKs (N+1 Convention)
The protocol strictly uses cumulative acknowledgments with $N+1$ semantics:
- When a peer receives packet $N$, it sends `ACK = N + 1` (meaning: *"I have received everything up to $N$, and am now expecting $N+1$."*).
- An incoming `ACK = A` slides `tail` forward, freeing all slots with `seq_num < A`.

### Window Floor & Stale ACK Protection
In `rudp_recv_ack()`, delayed or reordered ACKs that fall behind the oldest in-flight packet (`tail`) are rejected:
```c
if ((int16_t)(ack_num - tail_seq) < 0) {
    return RUDP_ERR_OUT_OF_WINDOW;
}
```

### Fast Retransmit (Tri-ACK State Machine)
- Tri-ACK trigger: When the window is stalled (`tail != head`) and the same ACK arrives 3 times consecutively (`duplicate_ack_count == 3`), the oldest packet at `tail` is flagged for immediate retransmission (`slot->fast_retransmit = RUDP_FAST_RETRANSMIT_PENDING`).
- Anti-storm lock: Fast retransmission triggers strictly on `== 3` to prevent cascading storms on subsequent duplicate ACKs (count 4+).
- Dynamic ACK refresh: During retransmission in `rudp_tick()`, `slot->frame.header.ack` is dynamically refreshed to `ctx->expected_seq_num` so peers are updated with latest reception state.

### Dead Peer Detection & Zombie Protection
- Retransmissions are tracked per slot (`slot->retries++`).
- When `retries > RUDP_MAX_RETRIES` (default 10):
  - `ctx->state` transitions to `RUDP_STATE_DISCONNECTED`.
  - `rudp_tick()` returns `rudp_tick_result_s` with `status = RUDP_ERR_DISCONNECTED` while preserving the count of already collected expired indices.
  - Subsequent calls to `rudp_tick()` and `rudp_send()` are inert and return `RUDP_ERR_DISCONNECTED`, preventing uint8 counter overflow from resurrecting dead sessions.
  - The application can call `rudp_get_unacked_slots()` to inspect unacknowledged in-flight packets and execute game rollbacks.
  - To reconnect, the application explicitly calls `rudp_reset(ctx)`.

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
