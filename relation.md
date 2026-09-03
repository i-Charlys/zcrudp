# zcrudp Architectural Map & Function Relationships

This document details the data structure hierarchy, call graph data flows, and step-by-step lifecycle execution of the zcrudp protocol engine.

---

## 1. In-Memory Structure Hierarchy

```text
+---------------------------------------------------------------------------------------------+
|                                   rudp_context_s (1036 B)                                   |
|                                                                                             |
|  +---------------------------------------------------------------------------------------+  |
|  |                         tx_buffer[64] (Ring Buffer: 1024 B)                           |  |
|  |                                                                                       |  |
|  |  +---------------------------------------------------------------------------------+  |  |
|  |  |                            rudp_slot_s (Slots 0..63: 16 B)                      |  |  |
|  |  |                                                                                 |  |  |
|  |  |  +---------------------------------------------------------------------------+  |  |  |
|  |  |  |                         rudp_frame_s (Network Frame: 8 B)                 |  |  |  |
|  |  |  |                                                                           |  |  |  |
|  |  |  |  +-------------------------------+ +-----------------------------------+  |  |  |  |
|  |  |  |  |      rudp_header_s (4 B)      | |        tfv_packet_u (4 B)         |  |  |  |  |
|  |  |  |  |  - seq_num (uint16_t - 2 B)   | |  - type  (uint8_t  - 1 B)         |  |  |  |  |
|  |  |  |  |  - ack     (uint16_t - 2 B)   | |  - flags (uint8_t  - 1 B)         |  |  |  |  |
|  |  |  |  |                               | |  - value (uint16_t - 2 B)         |  |  |  |  |
|  |  |  |  +-------------------------------+ +-----------------------------------+  |  |  |  |
|  |  |  +---------------------------------------------------------------------------+  |  |  |
|  |  |                                                                                 |  |  |
|  |  |  - timestamp (uint32_t - 4 B)  : Last transmission timestamp in milliseconds    |  |  |
|  |  |  - state     (uint8_t  - 1 B)  : RUDP_SLOT_FREE (0) or RUDP_SLOT_IN_FLIGHT (1)  |  |  |
|  |  |  - retries   (uint8_t  - 1 B)  : Retransmission retry counter                   |  |  |
|  |  |  - reserved  (uint8_t[2] - 2 B): 16-byte alignment padding                      |  |  |
|  |  +---------------------------------------------------------------------------------+  |  |
|  +---------------------------------------------------------------------------------------+  |
|                                                                                             |
|  Protocol State Machine Control Variables (12 B):                                           |
|  - head                (uint16_t - 2 B) : Next write index in tx_buffer                     |
|  - tail                (uint16_t - 2 B) : Oldest unacknowledged in-flight index             |
|  - current_seq_num     (uint16_t - 2 B) : Next sequence number to assign (TX)               |
|  - expected_seq_num    (uint16_t - 2 B) : Next sequence number expected (RX)                |
|  - last_ack_received   (uint16_t - 2 B) : Last cumulative ACK received from peer            |
|  - duplicate_ack_count (uint8_t  - 1 B) : Duplicate ACK counter (Fast Retransmit)           |
|  - state               (uint8_t  - 1 B) : RUDP_STATE_DISCONNECTED (0) / CONNECTED (1)       |
+---------------------------------------------------------------------------------------------+
```

---

## 2. Global Dataflow & Function Call Graph

```text
       +--------------------------------------------------------+
       |                   APPLICATION / GAME                   |
       +-------+-------------------^--------------------+-------+
               |                   |                    |
   1. rudp_send()                  | 3. rudp_recv()     | 4. rudp_tick()
   (Queue payload)                 | (Ingest frame)     | (Scan expirations)
               |                   |                    |
               v                   |                    v
     +-------------------+         |          +-------------------+
     |  tx_buffer[head]  |         |          | tx_buffer[tail..] |
     |  (Store slot)     |         |          | (Scan timeouts)   |
     +---------+---------+         |          +---------+---------+
               |                   |                    |
               |                   |                    | Returns out_indices[]
               v                   |                    v
     +-------------------+         |          +-------------------+
     |  rudp_pack_frame  |         |          | Retransmission of |
     |  (Serialize 8 B   |         |          | expired slot via  |
     |   Big-Endian)     |         |          | rudp_pack_frame   |
     +---------+---------+         |          +---------+---------+
               |                   |                    |
               v                   |                    v
    =====================================================================
                             NETWORK / UDP SOCKET
    =====================================================================
                                   ^
                                   | Raw bytes from UDP socket
                                   |
                         +---------+----------+
                         | rudp_unpack_frame  |
                         | (Deserialize 8 B)  |
                         +---------+----------+
                                   |
                                   v
                         +--------------------+
                         |    rudp_recv()     |
                         +---------+----------+
                                   |
                    +--------------+--------------+
                    |                             |
                    v                             v
        [Extract ACK field]             [Check frame.seq_num]
                    |                             |
                    v                             v
         +--------------------+         If seq == expected_seq_num:
         |   rudp_recv_ack    |         - Deliver to application
         | (Free tail slots)  |         - expected_seq_num++
         +--------------------+
```

---

## 3. Function Specifications

### 1. `rudp_init(ctx)`
* Role: Initializes the RUDP context to a clean, active state.
* Mutations:
  - `memset(ctx->tx_buffer, 0, sizeof(ctx->tx_buffer))`
  - `head = 0`, `tail = 0`
  - `current_seq_num = 0`, `expected_seq_num = 0`
  - `last_ack_received = 0xFFFF` (sentinel preventing startup off-by-one collisions with initial ACK=0)
  - `state = RUDP_STATE_CONNECTED`

---

### 2. `rudp_send(ctx, packet, now)`
* Role: Queues an application payload (`tfv_packet_u`) into the sliding transmission window.
* Invariants: Window must not be full (`(head + 1) % 64 != tail`) and `state == RUDP_STATE_CONNECTED`.
* Actions:
  1. Writes to `ctx->tx_buffer[ctx->head]`.
  2. Sets `frame.header.seq_num = ctx->current_seq_num++`.
  3. Piggybacks acknowledgment: `frame.header.ack = ctx->expected_seq_num`.
  4. Marks `slot->state = RUDP_SLOT_IN_FLIGHT`, `slot->retries = 0`, `slot->timestamp = now`.
  5. Advances `head = (head + 1) % 64`.

---

### 3. `rudp_pack_frame(frame, out_buf, max_len)`
* Role: Serializes a `rudp_frame_s` into 8 raw network bytes using Big-Endian byte ordering.
* Byte Output:
  - `out_buf[0..1]` = `seq_num` (16-bit MSB/LSB)
  - `out_buf[2..3]` = `ack` (16-bit MSB/LSB)
  - `out_buf[4]`    = `type` (8-bit)
  - `out_buf[5]`    = `flags` (8-bit)
  - `out_buf[6..7]` = `value` (16-bit MSB/LSB)

---

### 4. `rudp_unpack_frame(in_buf, in_len, out_frame)`
* Role: Deserializes 8 raw network bytes into a native `rudp_frame_s` struct.
* Precondition: `in_len >= 8`.

---

### 5. `rudp_recv(ctx, frame, out_packet)`
* Role: Primary ingestion entrypoint for received frames.
* Actions:
  1. Ingests piggybacked ACK via `rudp_recv_ack(ctx, frame->header.ack)` to free confirmed in-flight TX slots.
  2. Evaluates payload sequence number:
     - If `frame->header.seq_num == ctx->expected_seq_num`:
       - Copies `*out_packet = frame->packet`.
       - Advances `ctx->expected_seq_num++`.
       - Returns `1` (new in-order packet delivered).
     - Otherwise: Returns `0` (duplicate or out-of-order packet dropped).

---

### 6. `rudp_recv_ack(ctx, ack_num)`
* Role: Processes cumulative ACKs under the N+1 convention.
* Actions:
  1. Window Validation: Rejects future ACKs (`(ack_num - current_seq_num) > 0`) or stale ACKs older than `tail`.
  2. Cumulative Release:
     - While `tail != head` and `(ack_num - slot[tail].seq_num) > 0`:
       - Sets `slot[tail].state = RUDP_SLOT_FREE`.
       - Advances `tail = (tail + 1) % 64`.
  3. Fast Retransmit (Tri-ACK) State Machine:
     - If `tail` advanced: Resets `duplicate_ack_count = 0`, updates `last_ack_received = ack_num`.
     - If `tail` is stalled and `ack_num == last_ack_received`:
       - Increments `duplicate_ack_count++`.
       - When `duplicate_ack_count == 3`: Flags slot at `tail` for immediate retransmission.

---

### 7. `rudp_tick(ctx, now, timeout, out_indices, max_indices)`
* Role: Periodic clock driver handling packet timeouts and connection lifecycle.
* Actions:
  1. If `ctx->state == RUDP_STATE_DISCONNECTED`: Returns `-2` immediately (inert state).
  2. Scans slots from `tail` to `head`.
  3. If a slot is `IN_FLIGHT` and `now - slot->timestamp > timeout` (or Fast Retransmit flag is active):
     - Increments `slot->retries++`.
     - If `slot->retries > RUDP_MAX_RETRIES`: Transitions `ctx->state = RUDP_STATE_DISCONNECTED`.
     - Updates `slot->timestamp = now`.
     - Appends slot index to `out_indices[]`.
  4. Returns the total count of expired slots to retransmit.

---

## 4. End-to-End Walkthrough: Alice to Bob Data Exchange

```text
 ALICE (Sender)                                                   BOB (Receiver)
 ---------------------------------------------------------------------------------
 1. Alice queues input:
    tfv_packet_u msg = {MOVE, 0, 100};
    rudp_send(&alice_ctx, msg, 1000);
    -> tx_buffer[0] = seq:0, ack:0 (IN_FLIGHT)
    
 2. Alice serializes to 8 bytes:
    rudp_pack_frame(&slot.frame, buf, 8);
    sendto(socket, buf, 8, ...);
                                       ===============>
                                        (Network Wire)
                                                      3. Bob receives 8 bytes:
                                                         recvfrom(socket, buf, 8, ...);
                                                         rudp_unpack_frame(buf, 8, &frame);
                                                         
                                                      4. Bob processes frame:
                                                         rudp_recv(&bob_ctx, &frame, &msg);
                                                         - rudp_recv_ack() processes ack:0
                                                         - seq:0 == expected:0 -> ACCEPTED
                                                         - expected_seq_num becomes 1
                                                         
                                                      5. Bob replies to Alice:
                                                         rudp_send(&bob_ctx, bob_msg, 1020);
                                                         -> tx_buffer[0] = seq:0, ack:1 (Piggybacked)
                                                         
                                                         rudp_pack_frame(&bob_slot, buf, 8);
                                                         sendto(socket, buf, 8, ...);
                                       <===============
 6. Alice receives reply:
    rudp_unpack_frame(buf, 8, &frame);
    rudp_recv(&alice_ctx, &frame, &msg);
    -> rudp_recv_ack(&alice_ctx, 1);
       - ack:1 acknowledges Alice's packet 0
       - alice_ctx.tail advances to 1
       - alice_ctx.tx_buffer[0] is freed
```
