#include "protocol_rudp.h"
#include <string.h>

/*
 * ============================================================================
 * RUDP MEMORY & PACKET ARCHITECTURE
 * ============================================================================
 *
 * 1. RUDP_CONTEXT_S (The "Engine" - Full Protocol State Machine)
 * +-------------------+------------+------------+---------------------------+
 * | current_seq_num   | head (2B)  | tail (2B)  | tx_buffer (64 Slots)      |
 * | (Next seq to use) | (Write idx)| (Read idx) | (~1 KB Ring Buffer)       |
 * +-------------------+------------+------------+---------------------------+
 *                                                              |
 *                                                              v
 * 2. TX_BUFFER (Circular Array - 64 Slots / Sliding Window)
 * +---------------+---------------+---------------+ ... +---------------+
 * |    Slot 0     |    Slot 1     |    Slot 2     |     |    Slot 63    |
 * +---------------+---------------+---------------+ ... +---------------+
 *         |
 *         v
 * 3. RUDP_SLOT_S (The "Storage Container" - 16B aligned)
 * +---------------------+-----------------------+-----------------------------+
 * | state (1B)          | timestamp (4B)        | FRAME (8B)                  |
 * | (FREE / IN_FLIGHT)  | (Sent time in ms)     | (Network Wire Packet)       |
 * +---------------------+-----------------------+-----------------------------+
 *   <------------- Local Memory ------------->    <------ Wire Payload ------>
 *                                                              |
 *                                                              v
 * 4. RUDP_FRAME_S (The "Wire Packet" - Exactly 8 Bytes on Network)
 * +-------------------------------------+-------------------------------------+
 * |             HEADER (4B)             |             PACKET (4B)             |
 * +------------------+------------------+---------+-----------+---------------+
 * | seq_num (2B)     | ack (2B)         | type    | flags     | value         |
 * | (16-bit sequence)| (Cumulative ACK) | (8-bit) | (8-bit)   | (16-bit val)  |
 * +------------------+------------------+---------+-----------+---------------+
 *   <------- Protocol Control ------->    <-------- TFV Data Payload ------->
 *
 * ============================================================================
 * 5. PROTOCOL DATA FLOW & SYMMETRY (TX / RX PIPELINE)
 * ============================================================================
 *
 *  SENDER WORKFLOW (Game -> Network)         RECEIVER WORKFLOW (Network -> Game)
 *  ─────────────────────────────────         ───────────────────────────────────
 *  1. Game Data: tfv_packet_u                1. Raw Bytes from UDP socket
 *          │                                         │
 *          ▼                                         ▼
 *  2. rudp_send(ctx, packet, now);           2. rudp_unpack_frame(buf, len, &frame);
 *     (Assigns seq, ack, queues in tx_buf)      (Converts raw bytes to rudp_frame_s)
 *          │                                         │
 *          ▼                                         ▼
 *  3. rudp_pack_frame(&frame, buf, 8);       3. rudp_recv(ctx, &frame, &out_packet);
 *     (Serializes 8B Big-Endian for socket)     (Processes incoming ACK, drops dups,
 *                                                and delivers tfv_packet_u to game!)
 * ============================================================================
 */
/**
 * @brief Initializes a RUDP context.
 *
 * @param ctx The RUDP context to initialize.
 * @return 0 on success, -1 on failure.
 */

int rudp_init(rudp_context_s *ctx) {
    if (!ctx) {
        return RUDP_ERR_INVALID_ARG;
    }

    memset(ctx->tx_buffer, 0, sizeof(ctx->tx_buffer));

    ctx->head = 0;
    ctx->tail = 0;
    ctx->current_seq_num = 0;
    ctx->expected_seq_num = 0;
    ctx->last_ack_received = 0xFFFF; // Initialize to an invalid sequence number
    ctx->duplicate_ack_count = 0;
    ctx->state = RUDP_STATE_CONNECTED;

    return RUDP_OK;
}

int rudp_reset(rudp_context_s *ctx) {
    if (!ctx) {
        return RUDP_ERR_INVALID_ARG;
    }
    return rudp_init(ctx);
}

/**
 * @brief Sends a packet over RUDP.
 *
 * @param ctx The RUDP context.
 * @param packet The packet to send.
 * @param now Current timestamp in milliseconds.
 * @return RUDP_OK on success, or negative error code on failure.
 */
int rudp_send(rudp_context_s *ctx, tfv_packet_u packet, uint32_t now) {
    if (!ctx) {
        return RUDP_ERR_INVALID_ARG;
    }
    if (ctx->state != RUDP_STATE_CONNECTED) {
        return RUDP_ERR_DISCONNECTED;
    }
    if (((ctx->head + 1) & (RUDP_WINDOW_SIZE - 1)) == ctx->tail) {
        return RUDP_ERR_BUFFER_FULL;
    }

    rudp_slot_s *slot = &ctx->tx_buffer[ctx->head];

    slot->frame.packet = packet;
    slot->frame.header.seq_num = ctx->current_seq_num;
    slot->frame.header.ack = ctx->expected_seq_num; // Automatic ACK piggybacking
    slot->state = RUDP_SLOT_IN_FLIGHT;
    slot->retries = 0;
    slot->fast_retransmit = RUDP_FAST_RETRANSMIT_OFF;
    slot->timestamp = now;

    ctx->current_seq_num = (ctx->current_seq_num + 1);
    ctx->head = (ctx->head + 1) & (RUDP_WINDOW_SIZE - 1);

    return RUDP_OK;
}

int rudp_recv(rudp_context_s *ctx, const rudp_frame_s *frame, tfv_packet_u *out_packet) {
    if (!ctx || !frame || !out_packet) {
        return RUDP_ERR_INVALID_ARG;
    }

    rudp_recv_ack(ctx, frame->header.ack); // Process the cumulative ACK from incoming frame

    /* Check if the incoming sequence number matches the expected sequence number */
    if (frame->header.seq_num == ctx->expected_seq_num) {
        *out_packet = frame->packet; // Deliver packet payload to application
        ctx->expected_seq_num++;     // Advance expected sequence number (automatic 16-bit rollover)
        return 1;                    // Success: New packet delivered
    }

    return 0; // Duplicate or out-of-order packet ignored
}

/**
 * @brief Receives and processes a cumulative ACK over RUDP (N+1 convention).
 *        Implements Fast Retransmit (Tri-ACK) to detect packet loss before timeout.
 *
 * @param ctx The RUDP context.
 * @param ack_num The sequence number of the next expected packet (N+1).
 * @return RUDP_OK on success, or negative error code on failure.
 */
int rudp_recv_ack(rudp_context_s *ctx, uint16_t ack_num) {
    if (!ctx) {
        return RUDP_ERR_INVALID_ARG;
    }

    /* Out-of-window check: Reject ACKs referencing sequences strictly ahead of what was sent */
    if ((int16_t)(ack_num - ctx->current_seq_num) > 0) {
        return RUDP_ERR_OUT_OF_WINDOW;
    }

    if (ctx->tail != ctx->head) {
        uint16_t tail_seq = ctx->tx_buffer[ctx->tail].frame.header.seq_num;
        if ((int16_t)(ack_num - tail_seq) < 0) {
            // ACK is for a sequence number older than the oldest in-flight packet
            return RUDP_ERR_OUT_OF_WINDOW;
        }
    }

    uint16_t old_tail = ctx->tail;

    /**
     * Iterate through the sliding window and free slots acknowledged by ack_num.
     * Modular signed arithmetic (int16_t) handles sequence number wraparound (65535 -> 0).
     */
    while (ctx->tail != ctx->head && 
        (int16_t)(ack_num - ctx->tx_buffer[ctx->tail].frame.header.seq_num) > 0) 
    {
        ctx->tx_buffer[ctx->tail].state = RUDP_SLOT_FREE;
        ctx->tail = (ctx->tail + 1) & (RUDP_WINDOW_SIZE - 1);
    }

    /* Fast Retransmit (Tri-ACK) State Machine */
    if (ctx->tail != old_tail) {
        // Window moved: this is a new ACK acknowledging fresh packets
        ctx->last_ack_received = ack_num;
        ctx->duplicate_ack_count = 0;
    } else if (ctx->tail != ctx->head) {
        // Window is stalled and packets are still in-flight
        if (ack_num == ctx->last_ack_received) {
            ctx->duplicate_ack_count++;

            // Fast Retransmit Trigger: Exactly 3 duplicate ACKs signify packet loss
            if (ctx->duplicate_ack_count == 3) {
                // Set explicit fast retransmit flag for immediate resend
                ctx->tx_buffer[ctx->tail].fast_retransmit = RUDP_FAST_RETRANSMIT_PENDING;
            }
        } else {
            // First observation of this stalled ACK reference
            ctx->last_ack_received = ack_num;
            ctx->duplicate_ack_count = 0;
        } 
    }

    return RUDP_OK;
}


/**
 * @brief Scans the transmission window for timed-out packets and fills an array with their indices.
 *
 * @param ctx The RUDP context.
 * @param now Current time in milliseconds.
 * @param timeout Retransmission timeout in milliseconds.
 * @param out_indices Array provided by the caller, to be filled with the indices of expired slots.
 * @param max_indices Maximum capacity of the out_indices array.
 * @return Number of packets marked for retransmission (>= 0), or negative error code.
 */
int rudp_tick(rudp_context_s *ctx, uint32_t now, uint32_t timeout, uint16_t *out_indices, int max_indices) {
    if (!ctx || !out_indices || max_indices <= 0) return RUDP_ERR_INVALID_ARG;

    if (ctx->state != RUDP_STATE_CONNECTED) return RUDP_ERR_DISCONNECTED; // Only process if connection is active
    
    if (ctx->head == ctx->tail) return 0; // Buffer is empty, nothing to do

    int count = 0;
    uint16_t current = ctx->tail;

    // Scan from tail to head, OR until the provided output array is full
    while (current != ctx->head && count < max_indices) {
        rudp_slot_s *slot = &ctx->tx_buffer[current];

        // Check if slot has expired or has fast_retransmit flagged
        if (slot->state == RUDP_SLOT_IN_FLIGHT && 
            (slot->fast_retransmit == RUDP_FAST_RETRANSMIT_PENDING || (now - slot->timestamp > timeout))) 
        {
            slot->fast_retransmit = RUDP_FAST_RETRANSMIT_OFF; // Clear fast retransmit flag

            // Dynamically refresh piggybacked cumulative ACK with current expected_seq_num
            slot->frame.header.ack = ctx->expected_seq_num;

            slot->retries++; // Increment the retry counter for this slot

            if (slot->retries > RUDP_MAX_RETRIES) {
                // Mark the connection as disconnected if retries exceed the limit
                ctx->state = RUDP_STATE_DISCONNECTED;
                return RUDP_ERR_DISCONNECTED; // Indicate a fatal error due to dead peer
            }
            
            // Reset the timer for this packet to prevent spamming
            slot->timestamp = now;
            
            // Add the slot's index to the output array
            out_indices[count] = current;
            count++;
        }

        // Move to the next slot in the circular buffer
        current = (current + 1) & (RUDP_WINDOW_SIZE - 1);
    }

    return count; // Return how many packets need to be resent
}


int rudp_pack_header(const rudp_header_s *header, uint8_t *out_buf, size_t max_len) {
    if (!header || !out_buf || max_len < RUDP_WIRE_HEADER_SIZE) {
        return RUDP_ERR_INVALID_ARG;
    }

    out_buf[0] = (uint8_t)(header->seq_num >> 8);
    out_buf[1] = (uint8_t)(header->seq_num & 0xFF);
    out_buf[2] = (uint8_t)(header->ack >> 8);
    out_buf[3] = (uint8_t)(header->ack & 0xFF);

    return RUDP_WIRE_HEADER_SIZE;
}

int rudp_pack_payload(const tfv_packet_u *packet, uint8_t *out_buf, size_t max_len) {
    if (!packet || !out_buf || max_len < sizeof(tfv_packet_u)) {
        return RUDP_ERR_INVALID_ARG;
    }

    out_buf[0] = packet->type;
    out_buf[1] = packet->flags;
    out_buf[2] = (uint8_t)(packet->value >> 8);
    out_buf[3] = (uint8_t)(packet->value & 0xFF);

    return (int)sizeof(tfv_packet_u);
}

int rudp_pack_frame(const rudp_frame_s *frame, uint8_t *out_buf, size_t max_len) {
    if (!frame || !out_buf || max_len < RUDP_WIRE_FRAME_SIZE) {
        return RUDP_ERR_INVALID_ARG;
    }

    // 1. Pack header (bytes 0..3)
    rudp_pack_header(&frame->header, out_buf, max_len);

    // 2. Pack payload (bytes 4..7)
    rudp_pack_payload(&frame->packet, out_buf + RUDP_WIRE_HEADER_SIZE, max_len - RUDP_WIRE_HEADER_SIZE);

    return RUDP_WIRE_FRAME_SIZE; // Success: 8 bytes written
}

int rudp_pack_ack(uint16_t ack_num, uint8_t *out_buf, size_t max_len) {
    if (!out_buf || max_len < RUDP_WIRE_HEADER_SIZE) {
        return RUDP_ERR_INVALID_ARG;
    }

    rudp_header_s header;
    header.seq_num = 0; // seq_num is unused for standalone cumulative ACK
    header.ack = ack_num;

    return rudp_pack_header(&header, out_buf, max_len);
}

int rudp_unpack_header(const uint8_t *in_buf, size_t in_len, rudp_header_s *out_header) {
    if (!in_buf || !out_header || in_len < RUDP_WIRE_HEADER_SIZE) {
        return RUDP_ERR_INVALID_ARG;
    }

    out_header->seq_num = ((uint16_t)in_buf[0] << 8) | in_buf[1];
    out_header->ack     = ((uint16_t)in_buf[2] << 8) | in_buf[3];

    return RUDP_OK;
}

int rudp_unpack_payload(const uint8_t *in_buf, size_t in_len, tfv_packet_u *out_packet) {
    if (!in_buf || !out_packet || in_len < sizeof(tfv_packet_u)) {
        return RUDP_ERR_INVALID_ARG;
    }

    out_packet->type  = in_buf[0];
    out_packet->flags = in_buf[1];
    out_packet->value = ((uint16_t)in_buf[2] << 8) | in_buf[3];

    return RUDP_OK;
}

int rudp_unpack_ack(const uint8_t *in_buf, size_t in_len, uint16_t *out_ack) {
    if (!in_buf || !out_ack) {
        return RUDP_ERR_INVALID_ARG;
    }

    rudp_header_s header;
    int res = rudp_unpack_header(in_buf, in_len, &header);
    if (res != RUDP_OK) {
        return res;
    }

    *out_ack = header.ack;
    return RUDP_OK;
}

int rudp_unpack_frame(const uint8_t *in_buf, size_t in_len, rudp_frame_s *out_frame) {
    if (!in_buf || !out_frame || in_len < RUDP_WIRE_FRAME_SIZE) {
        return RUDP_ERR_INVALID_ARG;
    }

    // 1. Unpack header via modular header decoder
    rudp_unpack_header(in_buf, in_len, &out_frame->header);

    // 2. Unpack payload via modular payload decoder
    rudp_unpack_payload(in_buf + RUDP_WIRE_HEADER_SIZE, in_len - RUDP_WIRE_HEADER_SIZE, &out_frame->packet);

    return RUDP_OK;
}

const rudp_frame_s *rudp_get_slot_frame(const rudp_context_s *ctx, uint16_t slot_idx) {
    if (!ctx || slot_idx >= RUDP_WINDOW_SIZE) {
        return NULL;
    }

    return &ctx->tx_buffer[slot_idx].frame;
}