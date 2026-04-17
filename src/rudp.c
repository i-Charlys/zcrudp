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
int rudp_send(rudp_context_s *ctx, tlv_packet_u packet) {

  /** Check if the context is valid */

  if (!ctx || ((ctx->head + 1) & (RUDP_WINDOW_SIZE - 1)) == (ctx->tail))
    return -1; // Check if the context is valid and the buffer is not full

  /** Get the next slot and fill it with the packet and update the context */
  rudp_slot_s *slot = &ctx->tx_buffer[ctx->head];

  /** Fill the slot with the packet and update the context */
  slot->frame.packet = packet;
  slot->frame.header.seq_num = ctx->current_seq_num;
  slot->state = 1;
  slot->timestamp = 0; /** Initialize the timestamp to 0 for the test */

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
   */
  while (ctx->tail != ctx->head) {
    rudp_slot_s *current_slot = &ctx->tx_buffer[ctx->tail];

    if (current_slot->frame.header.seq_num <= ack_num) {

      current_slot->state = 0;

      ctx->tail = (ctx->tail + 1) & (RUDP_WINDOW_SIZE - 1);

    } else {
        /** Stop at the first unacknowledged slot */
      break;
    }
  }

  return 0;
}


/**
 * @brief Handles retransmissions and timing.
 * * @param ctx The RUDP context.
 * @param now Current time in milliseconds.
 * @param timeout Retransmission timeout in milliseconds (e.g., 100).
 * @return 1 if a packet needs retransmission, 0 otherwise, -1 on error.
 */
int rudp_tick(rudp_context_s *ctx, uint32_t now, uint32_t timeout) {
    if (!ctx) return -1;
    
    /** If the buffer is empty, nothing to retransmit */
    if (ctx->head == ctx->tail) return 0;

    /** Get the oldest slot (the one at the tail) */
    rudp_slot_s *oldest = &ctx->tx_buffer[ctx->tail];

    /** If the oldest slot has timed out, mark it for retransmission */
    if (now - oldest->timestamp > timeout) {
        oldest->timestamp = now;
        
        return 1;
    }

    return 0;
}