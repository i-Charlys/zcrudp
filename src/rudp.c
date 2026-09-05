#include "protocol_rudp.h"
#include <string.h>

/*
 * ============================================================================
 * RUDP MULTI-CHANNEL & UNIFIED DATAGRAM ARCHITECTURE
 * ============================================================================
 *
 * 1. LOCAL MEMORY HIERARCHY (ZERO-MALLOC, CACHE-FRIENDLY STATIC LAYOUT)
 *
 * +-------------------------------------------------------------------------+
 * | RUDP_SESSION_S (4212 Bytes Total)                                       |
 * |                                                                         |
 * | channels[0..3] (4 Channels x 1052 Bytes = 4208 Bytes)                   |
 * | active_channels (1B) + rr_cursor (1B) + reserved[2] (2B) = 4 Bytes      |
 * +-------------------------------------------------------------------------+
 *   |
 *   v
 * +-------------------------------------------------------------------------+
 * | RUDP_CHANNEL_S (1052 Bytes per Channel)                                 |
 * |                                                                         |
 * | Byte 0: flags (1B)                 | Byte 1: channel_id (1B)            |
 * | Bytes 2..3: last_unreliable_seq(2B)| Byte 4: has_unreliable_seq (1B)    |
 * | Byte 5: ack_pending (1B)           | Bytes 6..7: last_ack_sent (2B)     |
 * | Bytes 8..9: next_unreliable_seq(2B)| Bytes 10..11: reserved (2B)        |
 * |                                                                         |
 * | Bytes 12..1051: ctx (RUDP_CONTEXT_S - 1040 Bytes)                       |
 * |   - tx_buffer[64] : 64 slots x 16 Bytes = 1024 Bytes                    |
 * |   - State Machine : 16 Bytes (head, tail, seq, exp, ack, dup, st, rx)   |
 * +-------------------------------------------------------------------------+
 *
 * ============================================================================
 * 2. UNIFIED WIRE FORMAT (NETWORK DATAGRAM & MULTI-CHANNEL BUNDLING)
 * ============================================================================
 *
 * A single UDP Datagram can bundle multiple messages from DIFFERENT channels:
 *
 * +----------------------------------+---------------------------------------+
 * | DATAGRAM HEADER (4 Bytes)        | BUNDLED RECORDS (count x 8 Bytes)     |
 * +-----------------+--------+-------+---------------------------------------+
 * | ack (2B)        | ack_ch | count | Record 0 (8B) | Record 1 (8B) | ...   |
 * | (Cumulative ACK)| (1B)   | (1B)  | (Channel A)   | (Channel B)   |       |
 * +-----------------+--------+-------+---------------+---------------+-------+
 *
 * A. STANDALONE ACK (count == 0, exactly 4 Bytes):
 * +------------------------------------+
 * | ack (2B)      | ack_chan (1B) | 0  |
 * +------------------------------------+
 *
 * B. BUNDLED MULTI-CHANNEL DATAGRAM (e.g., count == 2, 20 Bytes on wire):
 * +-------------------------------------------------------------------------+
 * | Header (4B) : ack=42, ack_channel=0, count=2                            |
 * +-------------------------------------------------------------------------+
 * | Record 0 (8B) -> Channel 0 [RELIABLE]   | seq=5 | tfv=ChatMsg           |
 * +-------------------------------------------------------------------------+
 * | Record 1 (8B) -> Channel 1 [UNRELIABLE] | seq=9 | tfv=PlayerPos         |
 * +-------------------------------------------------------------------------+
 *   -> Total Wire Length: 4 + (2 x 8) = 20 Bytes (fits in 84B Ethernet floor)
 *
 * ============================================================================
 * 3. RECORD WIRE LAYOUT (8 Bytes - Zero Padding)
 * ============================================================================
 * +--------------+------------+------------------+--------------------------+
 * | channel_id   | flags      | seq_num          | tfv_packet_u             |
 * | (1 Byte)     | (1 Byte)   | (2 Bytes)        | (4 Bytes)                |
 * +--------------+------------+------------------+--------------------------+
 *
 * ============================================================================
 * 4. MULTI-CHANNEL SESSION DISPATCH FLOW
 * ============================================================================
 *
 *  SOCKET RX (in_buf, in_len)
 *          │
 *          ▼
 *  rudp_session_process_datagram()
 *          │
 *          ├─► Pass 1: Atomic Integrity & Wire Bounds Check
 *          │
 *          └─► Pass 2: Dispatch & State Mutation:
 *                 ├─► 1. Piggybacked ACK:
 *                 │      rudp_recv_ack_ex(primary_chan, header.ack, count == 0);
 *                 │
 *                 └─► 2. For each Record (0 .. count - 1):
 *                        chan = &session->channels[rec.channel_id];
 *                        │
 *                        ├── ACK RECORD ──► rudp_recv_ack_ex(&chan->ctx, rec.seq_num, true)
 *                        ├── RELIABLE   ──► In-order delivery (if buffer capacity) / Re-arm ACK if dup
 *                        └── UNRELIABLE ──► RFC 1982 Anti-Rollback filter (last_unreliable_seq)
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
    ctx->last_rx_time = 0;

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
    slot->tx_count = 0;
    slot->timestamp = now;

    ctx->current_seq_num = (ctx->current_seq_num + 1);
    ctx->head = (ctx->head + 1) & (RUDP_WINDOW_SIZE - 1);

    return RUDP_OK;
}

int rudp_recv(rudp_context_s *ctx, const rudp_frame_s *frame, tfv_packet_u *out_packet) {
    if (!ctx || !frame || !out_packet) {
        return RUDP_ERR_INVALID_ARG;
    }

    /* Passive ACK processing: incoming data frames do not count toward Fast Retransmit (RFC 5681) */
    rudp_recv_ack_ex(ctx, frame->header.ack, false);

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
int rudp_recv_ack_ex(rudp_context_s *ctx, uint16_t ack_num, bool count_duplicate_ack) {
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
    } else if (count_duplicate_ack && ctx->tail != ctx->head) {
        // Only deliberate ACKs (Standalone ACK or explicit ACK record) count toward Fast Retransmit!
        if (ack_num == ctx->last_ack_received) {
            if (ctx->duplicate_ack_count < 255) {
                ctx->duplicate_ack_count++;
            }

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

int rudp_recv_ack(rudp_context_s *ctx, uint16_t ack_num) {
    return rudp_recv_ack_ex(ctx, ack_num, true);
}

void rudp_touch(rudp_context_s *ctx, uint32_t now) {
    if (ctx) {
        ctx->last_rx_time = now;
    }
}

bool rudp_is_alive(const rudp_context_s *ctx, uint32_t now, uint32_t idle_timeout) {
    if (!ctx || ctx->state != RUDP_STATE_CONNECTED) {
        return false;
    }
    return ((uint32_t)(now - ctx->last_rx_time) <= idle_timeout);
}

int rudp_session_init(rudp_session_s *session) {
    if (!session) {
        return RUDP_ERR_INVALID_ARG;
    }

    session->active_channels = 0;
    session->rr_cursor = 0;
    session->reserved[0] = 0;
    session->reserved[1] = 0;
    for (uint8_t i = 0; i < RUDP_MAX_CHANNELS; i++) {
        session->channels[i].channel_id = i;
        session->channels[i].flags = RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED;
        session->channels[i].last_unreliable_seq = 0;
        session->channels[i].has_unreliable_seq = 0;
        session->channels[i].ack_pending = 0;
        session->channels[i].last_ack_sent = 0;
        session->channels[i].next_unreliable_seq = 0;
        session->channels[i].reserved = 0;
        rudp_init(&session->channels[i].ctx);
    }

    return RUDP_OK;
}

int rudp_session_config_channel(rudp_session_s *session, uint8_t channel_id, uint8_t flags) {
    if (!session || channel_id >= RUDP_MAX_CHANNELS) {
        return RUDP_ERR_INVALID_ARG;
    }

    session->channels[channel_id].flags = flags;
    if (channel_id >= session->active_channels) {
        session->active_channels = (uint8_t)(channel_id + 1);
    }

    return RUDP_OK;
}

int rudp_session_reset_channel(rudp_session_s *session, uint8_t channel_id) {
    if (!session || channel_id >= RUDP_MAX_CHANNELS) {
        return RUDP_ERR_INVALID_ARG;
    }

    rudp_channel_s *chan = &session->channels[channel_id];
    chan->last_unreliable_seq = 0;
    chan->has_unreliable_seq = 0;
    chan->ack_pending = 0;
    chan->last_ack_sent = 0;
    chan->next_unreliable_seq = 0;
    chan->reserved = 0;

    return rudp_reset(&chan->ctx);
}

int rudp_session_reset(rudp_session_s *session) {
    if (!session) {
        return RUDP_ERR_INVALID_ARG;
    }

    for (uint8_t i = 0; i < RUDP_MAX_CHANNELS; i++) {
        rudp_session_reset_channel(session, i);
    }
    session->active_channels = 0;
    session->rr_cursor = 0;
    session->reserved[0] = 0;
    session->reserved[1] = 0;

    return RUDP_OK;
}

int rudp_session_send_reliable(rudp_session_s *session, uint8_t channel_id, tfv_packet_u payload, uint32_t now) {
    if (!session || channel_id >= RUDP_MAX_CHANNELS) {
        return RUDP_ERR_INVALID_ARG;
    }
    if (!(session->channels[channel_id].flags & RUDP_CHANNEL_FLAG_RELIABLE)) {
        return RUDP_ERR_INVALID_ARG;
    }

    return rudp_send(&session->channels[channel_id].ctx, payload, now);
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
rudp_tick_result_s rudp_tick(rudp_context_s *ctx, uint32_t now, uint32_t timeout, uint16_t *out_indices, int max_indices) {
    if (!ctx || !out_indices || max_indices <= 0) {
        rudp_tick_result_s result = {0, RUDP_ERR_INVALID_ARG};
        return result;
    }

    if (ctx->state != RUDP_STATE_CONNECTED) {
        rudp_tick_result_s result = {0, RUDP_ERR_DISCONNECTED};
        return result;
    } // Only process if connection is active
    
    if (ctx->head == ctx->tail) {
        rudp_tick_result_s result = {0, RUDP_OK};
        return result;
    }  // Buffer is empty, nothing to do

    int count = 0;
    uint16_t current = ctx->tail;

    // Scan from tail to head, OR until the provided output array is full
    while (current != ctx->head && count < max_indices) {
        rudp_slot_s *slot = &ctx->tx_buffer[current];

        // Calculate timeout with binary exponential backoff (capped at 64x)
        uint32_t shift = (slot->retries > 6) ? 6 : slot->retries;
        uint32_t current_timeout = timeout << shift;

        // Check if slot has expired or has fast_retransmit flagged
        if (slot->state == RUDP_SLOT_IN_FLIGHT && 
            (slot->fast_retransmit == RUDP_FAST_RETRANSMIT_PENDING || ((uint32_t)(now - slot->timestamp) > current_timeout))) 
        {
            slot->fast_retransmit = RUDP_FAST_RETRANSMIT_OFF; // Clear fast retransmit flag

            // Dynamically refresh piggybacked cumulative ACK with current expected_seq_num
            slot->frame.header.ack = ctx->expected_seq_num;

            slot->retries++; // Increment the retry counter for this slot
            slot->tx_count++;

            if (slot->retries > RUDP_MAX_RETRIES) {
                // Mark the connection as disconnected if retries exceed the limit
                ctx->state = RUDP_STATE_DISCONNECTED;
                    rudp_tick_result_s result = {count, RUDP_ERR_DISCONNECTED};
                return  result ; // Indicate a fatal error due to dead peer
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

    rudp_tick_result_s result = {count, RUDP_OK};
    return result; // Return how many packets need to be resent
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

int rudp_get_unacked_slots(const rudp_context_s *ctx, uint16_t *out_indices, int max_indices) {
    if (!ctx || !out_indices || max_indices <= 0) {
        return RUDP_ERR_INVALID_ARG;
    }

    int count = 0;
    uint16_t current = ctx->tail;

    while (current != ctx->head && count < max_indices) {
        if (ctx->tx_buffer[current].state == RUDP_SLOT_IN_FLIGHT) {
            out_indices[count++] = current;
        }
        current = (current + 1) & (RUDP_WINDOW_SIZE - 1);
    }

    return count;
}

int rudp_pack_datagram_header(const rudp_datagram_header_s *header, uint8_t *out_buf, size_t max_len) {
    if (!header || !out_buf || max_len < RUDP_WIRE_DATAGRAM_HEADER_SIZE) {
        return RUDP_ERR_INVALID_ARG;
    }
    if (header->ack_channel >= RUDP_MAX_CHANNELS) {
        return RUDP_ERR_INVALID_ARG;
    }

    out_buf[0] = (uint8_t)(header->ack >> 8);
    out_buf[1] = (uint8_t)(header->ack & 0xFF);
    out_buf[2] = header->ack_channel;
    out_buf[3] = header->count;

    return RUDP_WIRE_DATAGRAM_HEADER_SIZE;
}

int rudp_unpack_datagram_header(const uint8_t *in_buf, size_t in_len, rudp_datagram_header_s *out_header) {
    if (!in_buf || !out_header || in_len < RUDP_WIRE_DATAGRAM_HEADER_SIZE) {
        return RUDP_ERR_INVALID_ARG;
    }

    out_header->ack         = ((uint16_t)in_buf[0] << 8) | in_buf[1];
    out_header->ack_channel = in_buf[2];
    out_header->count       = in_buf[3];

    return RUDP_OK;
}

int rudp_pack_record(const rudp_record_s *record, uint8_t *out_buf, size_t max_len) {
    if (!record || !out_buf || max_len < RUDP_WIRE_RECORD_SIZE) {
        return RUDP_ERR_INVALID_ARG;
    }
    if (record->channel_id >= RUDP_MAX_CHANNELS) {
        return RUDP_ERR_INVALID_ARG;
    }

    out_buf[0] = record->channel_id;
    out_buf[1] = record->flags;
    out_buf[2] = (uint8_t)(record->seq_num >> 8);
    out_buf[3] = (uint8_t)(record->seq_num & 0xFF);

    out_buf[4] = record->payload.type;
    out_buf[5] = record->payload.flags;
    out_buf[6] = (uint8_t)(record->payload.value >> 8);
    out_buf[7] = (uint8_t)(record->payload.value & 0xFF);

    return RUDP_WIRE_RECORD_SIZE;
}

int rudp_unpack_record(const uint8_t *in_buf, size_t in_len, rudp_record_s *out_record) {
    if (!in_buf || !out_record || in_len < RUDP_WIRE_RECORD_SIZE) {
        return RUDP_ERR_INVALID_ARG;
    }

    out_record->channel_id = in_buf[0];
    out_record->flags      = in_buf[1];
    out_record->seq_num    = ((uint16_t)in_buf[2] << 8) | in_buf[3];

    out_record->payload.type  = in_buf[4];
    out_record->payload.flags = in_buf[5];
    out_record->payload.value = ((uint16_t)in_buf[6] << 8) | in_buf[7];

    return RUDP_OK;
}

int rudp_unpack_datagram(const uint8_t *in_buf, size_t in_len,
                         rudp_datagram_header_s *out_header,
                         rudp_record_s *out_records, size_t max_records) {
    if (!in_buf || !out_header || in_len < RUDP_WIRE_DATAGRAM_HEADER_SIZE) {
        return RUDP_ERR_INVALID_ARG;
    }

    int ret = rudp_unpack_datagram_header(in_buf, in_len, out_header);
    if (ret != RUDP_OK) {
        return ret;
    }

    size_t expected_len = RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)out_header->count * RUDP_WIRE_RECORD_SIZE);
    if (in_len != expected_len) {
        return RUDP_ERR_INVALID_ARG;
    }

    if (out_header->count > 0) {
        if (!out_records || max_records < (size_t)out_header->count) {
            return RUDP_ERR_INVALID_ARG;
        }

        for (uint8_t i = 0; i < out_header->count; i++) {
            size_t offset = RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)i * RUDP_WIRE_RECORD_SIZE);
            ret = rudp_unpack_record(in_buf + offset, in_len - offset, &out_records[i]);
            if (ret != RUDP_OK) {
                return ret;
            }
        }
    }

    return RUDP_OK;
}

int rudp_session_build_datagram(rudp_session_s *session, uint8_t primary_ack_channel,
                                uint8_t *out_buf, size_t max_len,
                                uint32_t now, uint32_t timeout) {
    if (!session || !out_buf || primary_ack_channel >= RUDP_MAX_CHANNELS) {
        return RUDP_ERR_INVALID_ARG;
    }
    if (max_len < RUDP_WIRE_DATAGRAM_HEADER_SIZE) {
        return RUDP_ERR_BUFFER_FULL;
    }

    /* Clamp capacity to RUDP_DEFAULT_MTU to prevent IP fragmentation */
    size_t effective_max = (max_len > RUDP_DEFAULT_MTU) ? RUDP_DEFAULT_MTU : max_len;

    rudp_channel_s *prim_chan = &session->channels[primary_ack_channel];

    /* Determine how many 8-byte records can fit into the provided buffer */
    size_t cap_records = (effective_max - RUDP_WIRE_DATAGRAM_HEADER_SIZE) / RUDP_WIRE_RECORD_SIZE;
    if (cap_records > 255) {
        cap_records = 255;
    }

    uint8_t count = 0;

    /* 1. Bundling priority: Multi-channel explicit ACK records for any other channel with ack_pending */
    for (uint8_t i = 0; i < RUDP_MAX_CHANNELS && count < cap_records; i++) {
        uint8_t c = (uint8_t)((session->rr_cursor + i) % RUDP_MAX_CHANNELS);
        if (c != primary_ack_channel && session->channels[c].ack_pending) {
            rudp_channel_s *chan = &session->channels[c];
            rudp_record_s rec;
            rec.channel_id = c;
            rec.flags = RUDP_RECORD_FLAG_ACK;
            rec.seq_num = chan->ctx.expected_seq_num;
            rec.payload.raw = 0;

            size_t offset = RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)count * RUDP_WIRE_RECORD_SIZE);
            int ret = rudp_pack_record(&rec, out_buf + offset, effective_max - offset);
            if (ret != RUDP_WIRE_RECORD_SIZE) {
                return ret;
            }

            chan->last_ack_sent = rec.seq_num;
            chan->ack_pending = 0;
            count++;
        }
    }

    /* 2. Bundling priority: In-flight reliable slots waiting in channel tx_buffers (Fair Round-Robin) */
    for (uint8_t i = 0; i < RUDP_MAX_CHANNELS && count < cap_records; i++) {
        uint8_t c = (uint8_t)((session->rr_cursor + i) % RUDP_MAX_CHANNELS);
        rudp_channel_s *chan = &session->channels[c];
        if (chan->ctx.state != RUDP_STATE_CONNECTED) {
            continue;
        }

        uint16_t curr = chan->ctx.tail;
        while (curr != chan->ctx.head && count < cap_records) {
            rudp_slot_s *slot = &chan->ctx.tx_buffer[curr];
            if (slot->state == RUDP_SLOT_IN_FLIGHT) {
                bool send_now = false;
                if (slot->tx_count == 0) {
                    send_now = true;
                } else if (slot->fast_retransmit == RUDP_FAST_RETRANSMIT_PENDING) {
                    send_now = true;
                } else if (timeout == 0) {
                    send_now = true;
                } else {
                    uint32_t shift = (slot->retries > 6) ? 6 : slot->retries;
                    uint32_t current_timeout = timeout << shift;
                    if ((uint32_t)(now - slot->timestamp) > current_timeout) {
                        send_now = true;
                    }
                }

                if (send_now) {
                    rudp_record_s rec;
                    rec.channel_id = c;
                    rec.flags = RUDP_RECORD_FLAG_RELIABLE;
                    rec.seq_num = slot->frame.header.seq_num;
                    rec.payload = slot->frame.packet;

                    size_t offset = RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)count * RUDP_WIRE_RECORD_SIZE);
                    int ret = rudp_pack_record(&rec, out_buf + offset, effective_max - offset);
                    if (ret != RUDP_WIRE_RECORD_SIZE) {
                        return ret;
                    }

                    if (slot->tx_count == 0) {
                        slot->tx_count = 1;
                        slot->timestamp = now;
                    } else {
                        slot->retries++;
                        slot->tx_count++;
                        slot->timestamp = now;
                        slot->fast_retransmit = RUDP_FAST_RETRANSMIT_OFF;
                        if (slot->retries > RUDP_MAX_RETRIES) {
                            chan->ctx.state = RUDP_STATE_DISCONNECTED;
                        }
                    }
                    count++;
                }
            }
            curr = (curr + 1) & (RUDP_WINDOW_SIZE - 1);
        }
    }

    /* Advance round-robin cursor for next datagram egress */
    session->rr_cursor = (uint8_t)((session->rr_cursor + 1) % RUDP_MAX_CHANNELS);

    /* 3. Pack the primary 4-byte datagram header at the beginning of out_buf */
    rudp_datagram_header_s d_header;
    d_header.ack = prim_chan->ctx.expected_seq_num;
    d_header.ack_channel = primary_ack_channel;
    d_header.count = count;

    int ret = rudp_pack_datagram_header(&d_header, out_buf, effective_max);
    if (ret != RUDP_WIRE_DATAGRAM_HEADER_SIZE) {
        return ret;
    }

    prim_chan->last_ack_sent = d_header.ack;
    prim_chan->ack_pending = 0;

    return (int)(RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)count * RUDP_WIRE_RECORD_SIZE));
}

int rudp_session_send_unreliable(rudp_session_s *session, uint8_t channel_id,
                                 tfv_packet_u payload, uint8_t ack_channel,
                                 uint8_t *out_buf, size_t max_len) {
    if (!session || !out_buf) {
        return RUDP_ERR_INVALID_ARG;
    }
    if (channel_id >= RUDP_MAX_CHANNELS || ack_channel >= RUDP_MAX_CHANNELS) {
        return RUDP_ERR_INVALID_ARG;
    }
    if (max_len < (RUDP_WIRE_DATAGRAM_HEADER_SIZE + RUDP_WIRE_RECORD_SIZE)) {
        return RUDP_ERR_INVALID_ARG;
    }

    rudp_channel_s *chan = &session->channels[channel_id];
    rudp_channel_s *ack_chan = &session->channels[ack_channel];

    rudp_datagram_header_s d_header;
    d_header.ack = ack_chan->ctx.expected_seq_num;
    d_header.ack_channel = ack_channel;
    d_header.count = 1;

    int ret = rudp_pack_datagram_header(&d_header, out_buf, max_len);
    if (ret != RUDP_WIRE_DATAGRAM_HEADER_SIZE) {
        return ret;
    }

    ack_chan->last_ack_sent = d_header.ack;
    ack_chan->ack_pending = 0;

    rudp_record_s record;
    record.channel_id = channel_id;
    record.flags = RUDP_RECORD_FLAG_UNRELIABLE;
    record.seq_num = chan->next_unreliable_seq++;
    record.payload = payload;

    ret = rudp_pack_record(&record, out_buf + RUDP_WIRE_DATAGRAM_HEADER_SIZE,
                           max_len - RUDP_WIRE_DATAGRAM_HEADER_SIZE);
    if (ret != RUDP_WIRE_RECORD_SIZE) {
        return ret;
    }

    return (RUDP_WIRE_DATAGRAM_HEADER_SIZE + RUDP_WIRE_RECORD_SIZE);
}

int rudp_session_process_datagram(rudp_session_s *session, const uint8_t *in_buf, size_t in_len,
                                  rudp_record_s *out_delivered, size_t max_delivered) {
    if (!session || !in_buf || in_len < RUDP_WIRE_DATAGRAM_HEADER_SIZE) {
        return RUDP_ERR_INVALID_ARG;
    }

    rudp_datagram_header_s header;
    int ret = rudp_unpack_datagram_header(in_buf, in_len, &header);
    if (ret != RUDP_OK) {
        return ret;
    }

    if (header.ack_channel >= RUDP_MAX_CHANNELS) {
        return RUDP_ERR_INVALID_ARG;
    }

    size_t expected_len = RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)header.count * RUDP_WIRE_RECORD_SIZE);
    if (in_len != expected_len) {
        return RUDP_ERR_INVALID_ARG;
    }

    /* PASS 1: Atomicity & Integrity Validation
     * Verify all records unpack successfully and have valid channel IDs BEFORE mutating any session state. */
    if (header.count > 0 && (!out_delivered || max_delivered == 0)) {
        return RUDP_ERR_INVALID_ARG;
    }

    for (uint8_t i = 0; i < header.count; i++) {
        size_t offset = RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)i * RUDP_WIRE_RECORD_SIZE);
        rudp_record_s rec;
        ret = rudp_unpack_record(in_buf + offset, in_len - offset, &rec);
        if (ret != RUDP_OK) {
            return ret;
        }
        if (rec.channel_id >= RUDP_MAX_CHANNELS) {
            return RUDP_ERR_INVALID_ARG;
        }
    }

    /* PASS 2: State Mutation and Delivery */
    /* 1. Dispatch piggybacked ACK to target channel sliding window.
     * Note: Passive piggyback on datagrams with data (count > 0) does NOT count duplicate ACKs (RFC 5681). */
    rudp_recv_ack_ex(&session->channels[header.ack_channel].ctx, header.ack, (header.count == 0));

    /* If standalone ACK (count == 0), no records to deliver */
    if (header.count == 0) {
        return 0;
    }

    /* 2. Process bundled message records */
    int delivered_count = 0;
    for (uint8_t i = 0; i < header.count; i++) {
        size_t offset = RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)i * RUDP_WIRE_RECORD_SIZE);
        rudp_record_s rec;
        (void)rudp_unpack_record(in_buf + offset, in_len - offset, &rec);

        rudp_channel_s *chan = &session->channels[rec.channel_id];

        if (rec.flags & RUDP_RECORD_FLAG_ACK) {
            /* Explicit multi-channel ACK record: routes cumulative ACK and counts duplicate ACKs */
            rudp_recv_ack_ex(&chan->ctx, rec.seq_num, true);
        } else if (rec.flags & RUDP_RECORD_FLAG_RELIABLE) {
            /* Reliable delivery via channel sliding window */
            if (rec.seq_num == chan->ctx.expected_seq_num) {
                if ((size_t)delivered_count < max_delivered) {
                    out_delivered[delivered_count++] = rec;
                    chan->ctx.expected_seq_num++;
                    chan->ack_pending = 1;
                }
                /* If out_delivered is saturated, do not increment expected_seq_num and do not set ack_pending.
                 * The un-delivered record will be dropped, forcing the sender to retransmit it later. */
            } else {
                /* Duplicate, retransmitted, or out-of-order gap packet check (RFC 1982 / RFC 5681) */
                int32_t diff = (int16_t)(rec.seq_num - chan->ctx.expected_seq_num);
                if (diff < 0 || (diff > 0 && diff < (int32_t)RUDP_WINDOW_SIZE)) {
                    /* Peer retransmitted an earlier packet (its ACK may have been lost),
                     * or a gap arrived (out-of-order packet).
                     * Under RFC 5681, re-arm ACK immediately to trigger duplicate ACKs / break deadlocks! */
                    chan->ack_pending = 1;
                }
            }
        } else {
            /* Unreliable delivery via 16-bit anti-rollback sequence filter (RFC 1982) */
            if (!chan->has_unreliable_seq) {
                if ((size_t)delivered_count < max_delivered) {
                    chan->last_unreliable_seq = rec.seq_num;
                    chan->has_unreliable_seq = 1;
                    out_delivered[delivered_count++] = rec;
                }
            } else {
                uint16_t distance = (uint16_t)(rec.seq_num - chan->last_unreliable_seq);
                if (distance != 0 && distance <= RUDP_UNRELIABLE_MAX_AHEAD) {
                    if ((size_t)delivered_count < max_delivered) {
                        chan->last_unreliable_seq = rec.seq_num;
                        out_delivered[delivered_count++] = rec;
                    }
                }
            }
        }
    }

    return delivered_count;
}