#include "../include/protocol_rudp.h"
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
 * ============================================================================
 */
/**
 * @brief Initializes a RUDP context.
 *
 * @param ctx The RUDP context to initialize.
 * @return 0 on success, -1 on failure.
 */

int rudp_init(rudp_context_s *ctx) {
  if (!ctx)
    return -1;

  memset(ctx->tx_buffer, 0, sizeof(ctx->tx_buffer)); // Initialize the tx_buffer to zero

  ctx->current_seq_num = 0;
  ctx->head = 0;
  ctx->tail = 0;


  return 0;
}

/**
 * @brief Sends a packet over RUDP.
 *
 * @param ctx The RUDP context.
 * @param packet The packet to send.
 * @return 0 on success, -1 on failure.
 */
int rudp_send(rudp_context_s *ctx, tfv_packet_u packet, uint32_t now) {

  /** Check if the context is valid */

  if (!ctx || ((ctx->head + 1) & (RUDP_WINDOW_SIZE - 1)) == (ctx->tail))
    return -1; // Check if the context is valid and the buffer is not full

  /** Get the next slot and fill it with the packet and update the context */
  rudp_slot_s *slot = &ctx->tx_buffer[ctx->head];

  /** Fill the slot with the packet and update the context */
  slot->frame.packet = packet;
  slot->frame.header.seq_num = ctx->current_seq_num;
  slot->state = RUDP_SLOT_IN_FLIGHT;
  slot->timestamp = now;

  /** Update the sequence number and head pointer */
  ctx->current_seq_num = (ctx->current_seq_num + 1);
  ctx->head = (ctx->head + 1) & (RUDP_WINDOW_SIZE - 1);

  return 0;
}


/**
 * @brief Receives an ACK over RUDP.
 *
 * @param ctx The RUDP context.
 * @param ack_num The sequence number of the ACK.
 * @return 0 on success, -1 on failure.
 */
int rudp_recv_ack(rudp_context_s *ctx, uint16_t ack_num) {
  if (!ctx)
    return -1;

  /**
   * Iterate through the buffer and mark slots as acknowledged.
   * We use modular arithmetic to handle sequence number wraparound (65535 -> 0).
   */
  while (ctx->tail != ctx->head) {
    rudp_slot_s *current_slot = &ctx->tx_buffer[ctx->tail];

    /* * Distance check: If (ack_num - seq_num) is positive in 16-bit signed logic,
     * it means the ACK is "ahead" of or equal to our current tail.
     */
    if ((int16_t)(ack_num - current_slot->frame.header.seq_num) >= 0) {

      // Mark the slot as free for future use
      current_slot->state = RUDP_SLOT_FREE;

      // Move the tail forward in the circular buffer (0 to 63)
      ctx->tail = (ctx->tail + 1) & (RUDP_WINDOW_SIZE - 1);

    } else {
      /** Stop at the first slot that is still ahead of the received ACK */
      break;
    }
  }

  return 0;
}


/**
 * @brief Scans the transmission window for timed-out packets and fills an array with their indices.
 *
 * @param ctx The RUDP context.
 * @param now Current time in milliseconds.
 * @param timeout Retransmission timeout in milliseconds.
 * @param out_indices Array provided by the caller, to be filled with the indices of expired slots.
 * @param max_indices Maximum capacity of the out_indices array.
 * @return The number of packets marked for retransmission, or -1 on error.
 */
int rudp_tick(rudp_context_s *ctx, uint32_t now, uint32_t timeout, uint16_t *out_indices, int max_indices) {
    if (!ctx || !out_indices || max_indices <= 0) return -1;
    
    if (ctx->head == ctx->tail) return 0; // Buffer is empty, nothing to do

    int count = 0;
    uint16_t current = ctx->tail;

    // Scan from tail to head, OR until the provided output array is full
    while (current != ctx->head && count < max_indices) {
        rudp_slot_s *slot = &ctx->tx_buffer[current];

        // If the packet is in-flight AND the timeout duration has elapsed
        if (slot->state == RUDP_SLOT_IN_FLIGHT && (now - slot->timestamp > timeout)) {
            
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


int rudp_pack_frame(const rudp_frame_s *frame, uint8_t *out_buf, size_t max_len) {
    if (!frame || !out_buf) {
        return -1; // Error: NULL pointer
    }

    if (max_len < RUDP_WIRE_FRAME_SIZE) {
        return -1; // Error: Buffer too small
    }

    out_buf[0] = (uint8_t)(frame->header.seq_num >> 8);
    out_buf[1] = (uint8_t)(frame->header.seq_num & 0xFF);
    out_buf[2] = (uint8_t)(frame->header.ack >> 8);
    out_buf[3] = (uint8_t)(frame->header.ack & 0xFF);

    out_buf[4] = frame->packet.type;
    out_buf[5] = frame->packet.flags;
    out_buf[6] = (uint8_t)(frame->packet.value >> 8);
    out_buf[7] = (uint8_t)(frame->packet.value & 0xFF);
    
    return RUDP_WIRE_FRAME_SIZE; // Success: 8 bytes written
}

int rudp_unpack_frame(const uint8_t *in_buf, size_t in_len, rudp_frame_s *out_frame) {
    if (!in_buf || !out_frame) {
        return -1; // Error: NULL pointer
    }

    if (in_len < RUDP_WIRE_FRAME_SIZE) {
        return -1; // Error: Buffer too small
    }

    out_frame->header.seq_num = ((uint16_t)in_buf[0] << 8) | in_buf[1];
    out_frame->header.ack     = ((uint16_t)in_buf[2] << 8) | in_buf[3];

    out_frame->packet.type    = in_buf[4];
    out_frame->packet.flags   = in_buf[5];
    out_frame->packet.value   = ((uint16_t)in_buf[6] << 8) | in_buf[7];

    return 0; // Success
}