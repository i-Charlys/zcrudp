#include "rudp_reliability.h"
#include <string.h>

int rudp_init(rudp_context_s *ctx) {
  if (!ctx) {
    return RUDP_ERR_INVALID_ARG;
  }

  *ctx = (rudp_context_s){
      .last_ack_received = UINT16_MAX, /* No ACK received yet. */
      .state = RUDP_STATE_CONNECTED,
  };

  return RUDP_OK;
}

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

  *slot = (rudp_slot_s){
      .frame =
          {
              .header =
                  {
                      .seq_num = ctx->current_seq_num,
                      .ack = ctx->expected_seq_num,
                  },
              .packet = packet,
          },
      .timestamp = now,
      .state = RUDP_SLOT_IN_FLIGHT,
      .retries = 0,
      .fast_retransmit = RUDP_FAST_RETRANSMIT_OFF,
      .tx_count = 0,
  };

  ctx->current_seq_num = (ctx->current_seq_num + 1);
  ctx->head = (ctx->head + 1) & (RUDP_WINDOW_SIZE - 1);

  return RUDP_OK;
}

int rudp_recv(rudp_context_s *ctx, const rudp_frame_s *frame,
              tfv_packet_u *out_packet) {
  if (!ctx || !frame || !out_packet) {
    return RUDP_ERR_INVALID_ARG;
  }

  /* Passive ACK processing: incoming data frames do not count toward Fast Retransmit
   * (RFC 5681) */
  rudp_recv_ack_ex(ctx, frame->header.ack, false);

  /* Check if the incoming sequence number matches the expected sequence number */
  if (frame->header.seq_num == ctx->expected_seq_num) {
    *out_packet = frame->packet; // Deliver packet payload to application
    ctx->expected_seq_num++;     // Advance expected sequence number (automatic 16-bit
                                 // rollover)
    return 1;                    // Success: New packet delivered
  }

  return 0; // Duplicate or out-of-order packet ignored
}

int rudp_recv_ack_ex(rudp_context_s *ctx, uint16_t ack_num, bool count_duplicate_ack) {
  if (!ctx) {
    return RUDP_ERR_INVALID_ARG;
  }

  /* Out-of-window check: Reject ACKs referencing sequences strictly ahead of what was
   * sent */
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
   * Modular signed arithmetic (int16_t) handles sequence number wraparound (65535 ->
   * 0).
   */
  while (ctx->tail != ctx->head &&
         (int16_t)(ack_num - ctx->tx_buffer[ctx->tail].frame.header.seq_num) > 0) {
    ctx->tx_buffer[ctx->tail].state = RUDP_SLOT_FREE;
    ctx->tail = (ctx->tail + 1) & (RUDP_WINDOW_SIZE - 1);
  }

  /* Fast Retransmit (Tri-ACK) State Machine */
  if (ctx->tail != old_tail) {
    // Window moved: this is a new ACK acknowledging fresh packets
    ctx->last_ack_received = ack_num;
    ctx->duplicate_ack_count = 0;
  } else if (count_duplicate_ack && ctx->tail != ctx->head) {
    // Only deliberate ACKs (Standalone ACK or explicit ACK record) count toward Fast
    // Retransmit!
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

rudp_tick_result_s rudp_tick(rudp_context_s *ctx, uint32_t now, uint32_t timeout,
                             uint16_t *out_indices, int max_indices) {
  if (!ctx || !out_indices || max_indices <= 0) {
    rudp_tick_result_s result = {.count = 0, .status = RUDP_ERR_INVALID_ARG};
    return result;
  }

  if (ctx->state != RUDP_STATE_CONNECTED) {
    rudp_tick_result_s result = {.count = 0, .status = RUDP_ERR_DISCONNECTED};
    return result;
  } // Only process if connection is active

  if (ctx->head == ctx->tail) {
    rudp_tick_result_s result = {.count = 0, .status = RUDP_OK};
    return result;
  } // Buffer is empty, nothing to do

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
        (slot->fast_retransmit == RUDP_FAST_RETRANSMIT_PENDING ||
         ((uint32_t)(now - slot->timestamp) > current_timeout))) {
      slot->fast_retransmit = RUDP_FAST_RETRANSMIT_OFF; // Clear fast retransmit flag

      // Dynamically refresh piggybacked cumulative ACK with current expected_seq_num
      slot->frame.header.ack = ctx->expected_seq_num;

      slot->retries++; // Increment the retry counter for this slot
      slot->tx_count++;

      if (slot->retries > RUDP_MAX_RETRIES) {
        // Mark the connection as disconnected if retries exceed the limit
        ctx->state = RUDP_STATE_DISCONNECTED;
        rudp_tick_result_s result = {.count = count, .status = RUDP_ERR_DISCONNECTED};
        return result; // Indicate a fatal error due to dead peer
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

  rudp_tick_result_s result = {.count = count, .status = RUDP_OK};
  return result; // Return how many packets need to be resent
}

int rudp_get_unacked_slots(const rudp_context_s *ctx, uint16_t *out_indices,
                           int max_indices) {
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

const rudp_frame_s *rudp_get_slot_frame(const rudp_context_s *ctx, uint16_t slot_idx) {
  if (!ctx || slot_idx >= RUDP_WINDOW_SIZE) {
    return NULL;
  }

  return &ctx->tx_buffer[slot_idx].frame;
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
  if (idle_timeout == 0) {
    return true;
  }
  return ((uint32_t)(now - ctx->last_rx_time) <= idle_timeout);
}

int rudp_reset(rudp_context_s *ctx) {
  if (!ctx) {
    return RUDP_ERR_INVALID_ARG;
  }
  return rudp_init(ctx);
}
