# zcrudp Architecture

This document describes the design and implementation details of the `zcrudp` (Zero-allocation C Reliable UDP) protocol.

## Protocol Design

The protocol is designed to be lightweight and efficient, fitting within a fixed-size 8-byte frame.

### 32-bit TLV
The data payload is a 32-bit Type-Length-Value (TLV) packet, represented by the `tlv_packet_u` union in `include/protocol_tlv.h`. It uses a bitfield to pack information into 4 bytes:
- **Type**: 8 bits
- **Flags**: 4 bits
- **Value**: 20 bits

### RUDP Frame
The RUDP frame (`rudp_frame_s`) is exactly 8 bytes long:
- **Header (4 bytes)**: `rudp_header_s`
    - `seq_num` (16 bits): Sequence number of the packet.
    - `ack` (16 bits): Acknowledgment number (cumulative).
- **Payload (4 bytes)**: `tlv_packet_u`

## Sliding Window

Reliability is managed using a sliding window mechanism implemented with a circular buffer.

### Circular Buffer (`tx_buffer`)
The `rudp_context_s` structure maintains a `tx_buffer` of `RUDP_WINDOW_SIZE` (default 64) slots.
- **Head Pointer**: Points to the next available slot for a new transmission.
- **Tail Pointer**: Points to the oldest unacknowledged packet in the window.

The window is considered full when `(head + 1) % RUDP_WINDOW_SIZE == tail`.

## Reliability Mechanism

### Cumulative ACKs
The protocol uses cumulative acknowledgments. When an ACK with number `N` is received, all packets with sequence numbers up to and including `N` are considered acknowledged.

### Rollover Safety
Sequence numbers are 16-bit unsigned integers (`uint16_t`). To handle wraparound (e.g., comparing `65535` and `0`), the protocol uses signed 16-bit arithmetic:
```c
if ((int16_t)(ack_num - seq_num) >= 0) {
    // ack_num is ahead of or equal to seq_num
}
```
This ensures that the comparison remains valid even when the sequence number rolls over.

## Retransmission Logic

Retransmissions are handled by the `rudp_tick()` function, which should be called periodically by the application.

### Timestamping
Each slot in the `tx_buffer` (`rudp_slot_s`) stores a `timestamp` of when it was last sent.
- When `rudp_tick()` is called, it iterates from `tail` to `head`.
- If a slot is `IN_FLIGHT` and `(current_time - timestamp) > timeout`, it is marked for retransmission.
- The `timestamp` is updated to the current time to prevent immediate repeated retransmissions.

## Memory Layout

The project is designed for zero dynamic memory allocation. All structures are intended to be allocated statically or on the stack.

### Alignment and Padding
- `rudp_slot_s` contains a `rudp_frame_s` (8 bytes), a `state` (1 byte), and a `timestamp` (4 bytes).
- Total size is 13 bytes, but compilers typically pad this to 16 bytes for alignment.
- The `RUDP_PACKED` macro (using `__attribute__((packed))`) can be enabled if strict memory layout is required, though it may impact performance on some architectures.

### Data Structures
- `rudp_context_s`: The main state container, holding the `tx_buffer` and window pointers.
- `rudp_slot_s`: Internal storage for the sliding window, including metadata for retransmission.
