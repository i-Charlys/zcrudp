#include "rudp_reliability.h"
#include <string.h>

int rudp_init(rudp_context_s *ctx) {
  if (!ctx) {
    return RUDP_ERR_INVALID_ARG;
  }

  *ctx = (rudp_context_s){
      .last_ack_received = UINT16_MAX, /* No ACK received yet. */
      .state = RUDP_STATE_CONNECTED,
      .last_rx_time = UINT32_MAX, /* Sentinel: no timestamp recorded yet. */
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
      .tx_count = 1, /* Standalone rudp_send produces an emitted wire frame. */
  };

  if (ctx->last_rx_time == UINT32_MAX) {
    ctx->last_rx_time = (now == UINT32_MAX) ? (UINT32_MAX - 1U) : now;
  }

  ctx->current_seq_num = (ctx->current_seq_num + 1);
  ctx->head = (ctx->head + 1) & (RUDP_WINDOW_SIZE - 1);

  return RUDP_OK;
}

int rudp_recv(rudp_context_s *ctx, const rudp_frame_s *frame,
              tfv_packet_u *out_packet) {
  if (!ctx || !frame || !out_packet) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (ctx->state != RUDP_STATE_CONNECTED) {
    return RUDP_ERR_DISCONNECTED;
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
  if (ctx->state != RUDP_STATE_CONNECTED) {
    return RUDP_ERR_DISCONNECTED;
  }

  /* Out-of-window check: Reject ACKs referencing sequences strictly ahead of what was
   * sent */
  if ((int16_t)(ack_num - ctx->current_seq_num) > 0) {
    return RUDP_ERR_OUT_OF_WINDOW;
  }

  if (ctx->tail == ctx->head) {
    return (ack_num == ctx->current_seq_num) ? RUDP_OK : RUDP_ERR_OUT_OF_WINDOW;
  }

  uint16_t tail_seq = ctx->tx_buffer[ctx->tail].frame.header.seq_num;
  if ((int16_t)(ack_num - tail_seq) < 0) {
    /* ACK is for a sequence number older than the oldest in-flight packet */
    return RUDP_ERR_OUT_OF_WINDOW;
  }

  /* Pre-validate any newly acknowledged range BEFORE mutating state:
   * Reject ACKs targeting slots that were queued but never sent on the wire
   * (tx_count == 0). */
  if ((int16_t)(ack_num - tail_seq) > 0) {
    uint16_t check = ctx->tail;
    while (check != ctx->head &&
           (int16_t)(ack_num - ctx->tx_buffer[check].frame.header.seq_num) > 0) {
      if (ctx->tx_buffer[check].state == RUDP_SLOT_FREE ||
          ctx->tx_buffer[check].tx_count == 0) {
        return RUDP_ERR_OUT_OF_WINDOW;
      }
      check = (uint16_t)((check + 1U) & (RUDP_WINDOW_SIZE - 1U));
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
    ctx->tx_buffer[ctx->tail].fast_retransmit = RUDP_FAST_RETRANSMIT_OFF;
    ctx->tail = (ctx->tail + 1) & (RUDP_WINDOW_SIZE - 1);
  }

  /* Fast Retransmit & NewReno Partial ACK State Machine (RFC 5681 / RFC 6582) */
  if (ctx->tail != old_tail) {
    /* Window moved: this is a new ACK acknowledging fresh packets */
    ctx->last_ack_received = ack_num;
    ctx->duplicate_ack_count = 0;

    /* Partial ACK during loss recovery immediately triggers repair of the next
     * un-SACKed lost packet that was already in flight when recovery began. */
    uint16_t scan = ctx->tail;
    while (scan != ctx->head && ctx->tx_buffer[scan].state == RUDP_SLOT_SACKED) {
      scan = (uint16_t)((scan + 1U) & (RUDP_WINDOW_SIZE - 1U));
    }
    if (scan != ctx->head) {
      rudp_slot_s *next_hole = &ctx->tx_buffer[scan];
      if (next_hole->state == RUDP_SLOT_IN_FLIGHT && next_hole->tx_count > 0 &&
          rudp_slot_fast_state(next_hole) == RUDP_FAST_RETRANSMIT_IN_RECOVERY) {
        rudp_slot_set_fast_state(next_hole, RUDP_FAST_RETRANSMIT_PENDING);
      }
    }
  } else if (count_duplicate_ack &&
             ctx->tx_buffer[ctx->tail].state == RUDP_SLOT_IN_FLIGHT &&
             ctx->tx_buffer[ctx->tail].tx_count > 0) {
    /* Only deliberate ACKs for an actually transmitted slot count toward Tri-ACK. */
    if (ack_num == ctx->last_ack_received) {
      if (ctx->duplicate_ack_count < 255) {
        ctx->duplicate_ack_count++;
      }

      /* Fast Retransmit Trigger: Exactly 3 duplicate ACKs signify packet loss */
      if (ctx->duplicate_ack_count == 3) {
        rudp_slot_set_fast_state(&ctx->tx_buffer[ctx->tail],
                                 RUDP_FAST_RETRANSMIT_PENDING);
        uint16_t curr = (uint16_t)((ctx->tail + 1U) & (RUDP_WINDOW_SIZE - 1U));
        while (curr != ctx->head) {
          rudp_slot_s *s = &ctx->tx_buffer[curr];
          if (s->state == RUDP_SLOT_IN_FLIGHT && s->tx_count > 0 &&
              rudp_slot_fast_state(s) == RUDP_FAST_RETRANSMIT_OFF) {
            rudp_slot_set_fast_state(s, RUDP_FAST_RETRANSMIT_IN_RECOVERY);
          }
          curr = (uint16_t)((curr + 1U) & (RUDP_WINDOW_SIZE - 1U));
        }
      }
    } else {
      /* First observation of this stalled ACK reference */
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

    // Calculate timeout with binary exponential backoff (capped at 64x, saturating)
    uint32_t shift = (slot->retries > 6) ? 6 : slot->retries;
    uint32_t current_timeout = timeout;
    for (uint32_t s = 0; s < shift; s++) {
      current_timeout =
          (current_timeout > UINT32_MAX / 2U) ? UINT32_MAX : (current_timeout << 1);
    }

    // Check if slot has expired or has fast_retransmit flagged
    bool is_fast = (rudp_slot_fast_state(slot) == RUDP_FAST_RETRANSMIT_PENDING);
    bool timed_out = ((uint32_t)(now - slot->timestamp) > current_timeout);
    if (slot->state == RUDP_SLOT_IN_FLIGHT && (is_fast || timed_out)) {
      if (slot->retries >= RUDP_MAX_RETRIES) {
        // Mark the connection as disconnected without emitting an extra retry
        ctx->state = RUDP_STATE_DISCONNECTED;
        rudp_tick_result_s result = {.count = count, .status = RUDP_ERR_DISCONNECTED};
        return result;
      }

      rudp_slot_set_fast_state(slot, RUDP_FAST_RETRANSMIT_OFF);
      if (!is_fast) {
        /* Timer expiration exits NewReno fast recovery across the window. */
        uint16_t clear = ctx->tail;
        while (clear != ctx->head) {
          if (rudp_slot_fast_state(&ctx->tx_buffer[clear]) ==
              RUDP_FAST_RETRANSMIT_IN_RECOVERY) {
            rudp_slot_set_fast_state(&ctx->tx_buffer[clear], RUDP_FAST_RETRANSMIT_OFF);
          }
          clear = (uint16_t)((clear + 1U) & (RUDP_WINDOW_SIZE - 1U));
        }
      }

      // Dynamically refresh piggybacked cumulative ACK with current expected_seq_num
      slot->frame.header.ack = ctx->expected_seq_num;

      slot->retries++; // Increment the retry counter for this slot
      if (slot->tx_count < UINT8_MAX) {
        slot->tx_count++;
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
    ctx->last_rx_time = (now == UINT32_MAX) ? (UINT32_MAX - 1U) : now;
  }
}

bool rudp_is_alive(const rudp_context_s *ctx, uint32_t now, uint32_t idle_timeout) {
  if (!ctx || ctx->state != RUDP_STATE_CONNECTED) {
    return false;
  }
  if (idle_timeout == 0 || ctx->last_rx_time == UINT32_MAX) {
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
