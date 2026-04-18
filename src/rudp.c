#include "../include/protocol_rudp.h"
#include <string.h>

/*
 * RUDP_CONTEXT (The "Engine")
 * +-------------------------------------------+
 * | current_seq_num | head | tail | tx_buffer |
 * +-------------------------------------------+
 *                                 |
 *                                 v
 * TX_BUFFER (Circular Array - 64 Slots)
 * +---------+---------+---------+---------+
 * | Slot 0  | Slot 1  | Slot 2  |  ...    |
 * +---------+---------+---------+---------+
 * |
 * v
 * RUDP_SLOT_S (The "Storage Container")
 * +-------------------------------------------------------+
 * | state (1B) | timestamp (4B) |       FRAME (8B)        |
 * +-------------------------------------------------------+
 * (Local management)           (Network payload)
 *                               |
 *                               v
 * RUDP_FRAME_S (The "Wire Packet")
 * +---------------------------------------+
 * |      HEADER (4B)      |  PACKET (4B)  |
 * +-----------------------+---------------+
 * | type | len | seq_num  |   TLV UNION   |
 * +---------------------------------------+
 * (Protocol Control)     (Actual Data)
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

  ctx->current_seq_num = 0;
  ctx->head = 0;
  ctx->tail = 0;

  memset(ctx, 0, sizeof(rudp_context_s)); // Initialize the tx_buffer to zero

  return 0;
}

/**
 * @brief Sends a packet over RUDP.
 *
 * @param ctx The RUDP context.
 * @param packet The packet to send.
 * @return 0 on success, -1 on failure.
 */
int rudp_send(rudp_context_s *ctx, tlv_packet_u packet, uint32_t now) {

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