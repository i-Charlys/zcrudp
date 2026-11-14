#include "rudp_session.h"
#include "rx_internal.h"
#include "rudp_reliability.h"
#include "rudp_codec.h"
#include <string.h>

static uint8_t read_session_mtu_recs(const rudp_session_s *session) {
  return (uint8_t)(((session->channels[0].unrel_ahead_streak & RUDP_CHAN_MTU_LO_MASK) >>
                    RUDP_CHAN_MTU_LO_SHIFT) |
                   (session->channels[0].chan_backoff & RUDP_CHAN_MTU_HI_MASK));
}

static size_t effective_mtu_cap(const rudp_session_s *session, size_t max_len) {
  uint8_t recs = read_session_mtu_recs(session);
  size_t mtu = (recs == 0U)
                   ? (size_t)RUDP_DEFAULT_MTU
                   : (size_t)(RUDP_WIRE_DATAGRAM_HEADER_SIZE +
                              ((size_t)recs * RUDP_WIRE_RECORD_SIZE));
  return (max_len > mtu) ? mtu : max_len;
}

static uint32_t channel_slot_timeout_ms(const rudp_channel_s *chan, uint16_t slot_idx,
                                        uint32_t fallback_timeout) {
  if (chan->rto_min_ms > 0U) {
    uint32_t d = chan->rto_ms;
    unsigned shift = chan->timeout_backoffs[slot_idx];
    unsigned chan_b = (unsigned)(chan->chan_backoff & RUDP_CHAN_BACKOFF_MASK);
    if (chan_b > shift) {
      shift = chan_b;
    }
    if (shift > 15U) {
      shift = 15U;
    }
    for (unsigned s = 0; s < shift && d < (uint32_t)chan->rto_max_ms; ++s) {
      d = (d > (uint32_t)chan->rto_max_ms / 2U) ? (uint32_t)chan->rto_max_ms : (d << 1);
    }
    if (d > (uint32_t)chan->rto_max_ms) {
      d = chan->rto_max_ms;
    }
    return d;
  }

  if (fallback_timeout == 0U) {
    return 0U;
  }
  unsigned shift = chan->ctx.tx_buffer[slot_idx].retries;
  if (shift > 6U) {
    shift = 6U;
  }
  uint32_t d = fallback_timeout;
  for (unsigned s = 0; s < shift; ++s) {
    d = (d > UINT32_MAX / 2U) ? UINT32_MAX : (d << 1);
  }
  return d;
}

static bool is_slot_eligible(const rudp_channel_s *chan, uint16_t slot_idx,
                             uint32_t now, uint32_t fallback_timeout) {
  const rudp_slot_s *slot = &chan->ctx.tx_buffer[slot_idx];
  if (slot->state != RUDP_SLOT_IN_FLIGHT) {
    return false;
  }
  if (slot->tx_count == 0 || slot->fast_retransmit == RUDP_FAST_RETRANSMIT_PENDING) {
    return true;
  }
  if (fallback_timeout == 0U && !chan->rto_min_ms) {
    return true;
  }
  uint32_t current_timeout = channel_slot_timeout_ms(chan, slot_idx, fallback_timeout);
  return ((uint32_t)(now - slot->timestamp) > current_timeout);
}

int rudp_session_send_reliable(rudp_session_s *session, uint8_t channel_id,
                               tfv_packet_u payload, uint32_t now) {
  if (!session || channel_id >= RUDP_MAX_CHANNELS) {
    return RUDP_ERR_INVALID_ARG;
  }
  rudp_channel_s *chan = &session->channels[channel_id];
  if (!(chan->flags & RUDP_CHANNEL_FLAG_RELIABLE)) {
    return RUDP_ERR_INVALID_ARG;
  }

  uint16_t slot_idx = chan->ctx.head;
  int ret = rudp_send(&chan->ctx, payload, now);
  if (ret != RUDP_OK) {
    return ret;
  }

  /* Queued for bundler emission; not transmitted on the wire until build_datagram(). */
  chan->ctx.tx_buffer[slot_idx].tx_count = 0;
  chan->timeout_backoffs[slot_idx] =
      chan->rto_min_ms ? (uint8_t)(chan->chan_backoff & RUDP_CHAN_BACKOFF_MASK) : 0U;
  return RUDP_OK;
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
  if (chan->ctx.state != RUDP_STATE_CONNECTED) {
    return RUDP_ERR_DISCONNECTED;
  }

  rudp_datagram_header_s d_header = {
      .ack = ack_chan->ctx.expected_seq_num, .ack_channel = ack_channel, .count = 1};

  int ret = rudp_pack_datagram_header(&d_header, out_buf, max_len);
  if (ret != RUDP_WIRE_DATAGRAM_HEADER_SIZE) {
    return ret;
  }

  ack_chan->last_ack_sent = d_header.ack;

  rudp_record_s record = {.channel_id = channel_id,
                          .flags = RUDP_RECORD_FLAG_UNRELIABLE,
                          .seq_num = chan->next_unreliable_seq++,
                          .payload = payload};

  ret = rudp_pack_record(&record, out_buf + RUDP_WIRE_DATAGRAM_HEADER_SIZE,
                         max_len - RUDP_WIRE_DATAGRAM_HEADER_SIZE);
  if (ret != RUDP_WIRE_RECORD_SIZE) {
    return ret;
  }

  return (RUDP_WIRE_DATAGRAM_HEADER_SIZE + RUDP_WIRE_RECORD_SIZE);
}

static void capture_channel_snapshot(const rudp_session_s *session,
                                     rudp_tx_plan_s *plan) {
  for (uint8_t c = 0; c < RUDP_MAX_CHANNELS; ++c) {
    plan->prev_chan_backoff[c] = session->channels[c].chan_backoff;
    plan->prev_last_ack_sent[c] = session->channels[c].last_ack_sent;
    plan->prev_ack_pending[c] = session->channels[c].ack_pending;
    plan->prev_state[c] = session->channels[c].ctx.state;
  }
  plan->prev_rr_cursor = session->rr_cursor;
  plan->record_count = 0;
  plan->valid = 0;
}

static int build_datagram_internal(rudp_session_s *session, uint8_t primary_ack_channel,
                                   uint8_t *out_buf, size_t max_len, uint32_t now,
                                   uint32_t timeout, rudp_tx_plan_s *plan) {
  if (!session || !out_buf || primary_ack_channel >= RUDP_MAX_CHANNELS) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (max_len < RUDP_WIRE_DATAGRAM_HEADER_SIZE) {
    return RUDP_ERR_BUFFER_FULL;
  }

  size_t effective_max = effective_mtu_cap(session, max_len);
  if (effective_max < RUDP_WIRE_DATAGRAM_HEADER_SIZE) {
    return RUDP_ERR_BUFFER_FULL;
  }

  rudp_channel_s *prim_chan = &session->channels[primary_ack_channel];

  /* Determine how many 8-byte records can fit into the effective MTU budget */
  size_t cap_records =
      (effective_max - RUDP_WIRE_DATAGRAM_HEADER_SIZE) / RUDP_WIRE_RECORD_SIZE;
  if (cap_records > RUDP_MAX_RECORDS_PER_DATAGRAM) {
    cap_records = RUDP_MAX_RECORDS_PER_DATAGRAM;
  }

  if (plan) {
    capture_channel_snapshot(session, plan);
  }

  bool has_eligible_tx = false;
  for (uint8_t c = 0; c < RUDP_MAX_CHANNELS && !has_eligible_tx; ++c) {
    const rudp_channel_s *chan = &session->channels[c];
    if (chan->ctx.state != RUDP_STATE_CONNECTED) {
      continue;
    }
    uint16_t curr = chan->ctx.tail;
    while (curr != chan->ctx.head) {
      if (is_slot_eligible(chan, curr, now, timeout)) {
        has_eligible_tx = true;
        break;
      }
      curr = (uint16_t)((curr + 1U) & (RUDP_WINDOW_SIZE - 1U));
    }
  }

  uint8_t count = 0;
  size_t ack_budget =
      (has_eligible_tx && cap_records >= 1U) ? (cap_records - 1U) : cap_records;

  /* 1. Multi-channel explicit ACK records, never starving urgent TX when cap == 1 */
  for (uint8_t i = 0; i < RUDP_MAX_CHANNELS && count < ack_budget; i++) {
    uint8_t c = (uint8_t)((session->rr_cursor + i) % (uint8_t)RUDP_MAX_CHANNELS);
    rudp_channel_s *chan = &session->channels[c];
    if (c != primary_ack_channel && chan->ctx.state == RUDP_STATE_CONNECTED &&
        chan->ack_pending) {
      rudp_record_s rec = {.channel_id = c,
                           .flags = RUDP_RECORD_FLAG_ACK,
                           .seq_num = chan->ctx.expected_seq_num,
                           .payload = {.raw = 0}};

      size_t offset =
          RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)count * RUDP_WIRE_RECORD_SIZE);
      int ret = rudp_pack_record(&rec, out_buf + offset, effective_max - offset);
      if (ret != RUDP_WIRE_RECORD_SIZE) {
        if (plan) {
          rudp_session_cancel_datagram(session, plan);
        }
        return ret;
      }

      chan->last_ack_sent = rec.seq_num;
      chan->ack_pending = 0;
      chan->chan_backoff &= (uint8_t)~RUDP_CHAN_FLAG_GAP_ACK_PENDING;
      count++;
    }
  }

  /* Stable priority order, rotating ties. ACKs remain ahead of regular data. */
  uint8_t order[RUDP_MAX_CHANNELS];
  for (size_t i = 0; i < RUDP_MAX_CHANNELS; ++i) {
    uint8_t c = (uint8_t)((session->rr_cursor + i) % (uint8_t)RUDP_MAX_CHANNELS);
    size_t j = i;
    while (j &&
           session->channels[order[j - 1]].priority > session->channels[c].priority) {
      order[j] = order[j - 1];
      --j;
    }
    order[j] = c;
  }

  /* 2. In-flight reliable slots in priority order. */
  for (uint8_t i = 0; i < RUDP_MAX_CHANNELS && count < cap_records; i++) {
    uint8_t c = order[i];
    rudp_channel_s *chan = &session->channels[c];
    if (chan->ctx.state != RUDP_STATE_CONNECTED) {
      continue;
    }

    uint16_t curr = chan->ctx.tail;
    while (curr != chan->ctx.head && count < cap_records) {
      rudp_slot_s *slot = &chan->ctx.tx_buffer[curr];
      if (is_slot_eligible(chan, curr, now, timeout)) {
        bool is_first_send = (slot->tx_count == 0);
        bool is_fast =
            (!is_first_send && slot->fast_retransmit == RUDP_FAST_RETRANSMIT_PENDING);

        if (!is_first_send && slot->retries >= RUDP_MAX_RETRIES) {
          /* Parity with rudp_tick: disconnect before emitting an extra wire copy */
          chan->ctx.state = RUDP_STATE_DISCONNECTED;
          if (plan) {
            plan->valid = 0;
          }
          return RUDP_ERR_DISCONNECTED;
        }

        rudp_record_s rec = {.channel_id = c,
                             .flags = RUDP_RECORD_FLAG_RELIABLE,
                             .seq_num = slot->frame.header.seq_num,
                             .payload = slot->frame.packet};

        size_t offset =
            RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)count * RUDP_WIRE_RECORD_SIZE);
        int ret = rudp_pack_record(&rec, out_buf + offset, effective_max - offset);
        if (ret != RUDP_WIRE_RECORD_SIZE) {
          if (plan) {
            plan->valid = 0;
          }
          return ret;
        }

        if (plan) {
          uint8_t idx = plan->record_count++;
          plan->slot_idx[idx] = curr;
          plan->channel_id[idx] = c;
          plan->prev_tx_count[idx] = slot->tx_count;
          plan->prev_retries[idx] = slot->retries;
          plan->prev_fast_retransmit[idx] = slot->fast_retransmit;
          plan->prev_slot_backoff[idx] = chan->timeout_backoffs[curr];
          plan->prev_timestamp[idx] = slot->timestamp;
        }

        if (is_first_send) {
          if (chan->rto_min_ms > 0U) {
            uint8_t cb = (uint8_t)(chan->chan_backoff & RUDP_CHAN_BACKOFF_MASK);
            if (cb > chan->timeout_backoffs[curr]) {
              chan->timeout_backoffs[curr] = cb;
            }
          } else {
            chan->timeout_backoffs[curr] = 0;
          }
          slot->tx_count = 1;
          slot->timestamp = now;
          slot->fast_retransmit = RUDP_FAST_RETRANSMIT_OFF;
        } else {
          if (!is_fast) {
            uint16_t clear = chan->ctx.tail;
            while (clear != chan->ctx.head) {
              if (chan->ctx.tx_buffer[clear].fast_retransmit ==
                  RUDP_FAST_RETRANSMIT_IN_RECOVERY) {
                chan->ctx.tx_buffer[clear].fast_retransmit = RUDP_FAST_RETRANSMIT_OFF;
              }
              clear = (uint16_t)((clear + 1U) & (RUDP_WINDOW_SIZE - 1U));
            }
            if (chan->timeout_backoffs[curr] < 15U) {
              chan->timeout_backoffs[curr]++;
            }
            if (chan->rto_min_ms > 0U) {
              uint8_t b = chan->timeout_backoffs[curr];
              if (b > (uint8_t)(chan->chan_backoff & RUDP_CHAN_BACKOFF_MASK)) {
                chan->chan_backoff = (uint8_t)(
                    (chan->chan_backoff & (uint8_t)~RUDP_CHAN_BACKOFF_MASK) |
                    (b & RUDP_CHAN_BACKOFF_MASK));
              }
            }
          }
          slot->retries++;
          if (slot->tx_count < UINT8_MAX) {
            slot->tx_count++;
          }
          slot->timestamp = now;
          slot->fast_retransmit = RUDP_FAST_RETRANSMIT_OFF;
        }
        count++;
      }
      curr = (uint16_t)((curr + 1U) & (RUDP_WINDOW_SIZE - 1U));
    }
  }

  /* If data was bundled alongside a gap/duplicate ACK on the primary channel, emit
   * one explicit ACK record so bidirectional traffic does not mask Tri-ACK recovery. */
  if (count > 0 && count < cap_records &&
      prim_chan->ctx.state == RUDP_STATE_CONNECTED && prim_chan->ack_pending &&
      (prim_chan->chan_backoff & RUDP_CHAN_FLAG_GAP_ACK_PENDING)) {
    rudp_record_s ack_rec = {.channel_id = primary_ack_channel,
                             .flags = RUDP_RECORD_FLAG_ACK,
                             .seq_num = prim_chan->ctx.expected_seq_num,
                             .payload = {.raw = 0}};
    size_t offset =
        RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)count * RUDP_WIRE_RECORD_SIZE);
    int ret = rudp_pack_record(&ack_rec, out_buf + offset, effective_max - offset);
    if (ret != RUDP_WIRE_RECORD_SIZE) {
      if (plan) {
        plan->valid = 0;
      }
      return ret;
    }
    prim_chan->chan_backoff &= (uint8_t)~RUDP_CHAN_FLAG_GAP_ACK_PENDING;
    count++;
  }

  if (count == 0 && prim_chan->ctx.state != RUDP_STATE_CONNECTED) {
    if (plan) {
      plan->valid = 0;
    }
    return RUDP_ERR_DISCONNECTED;
  }

  /* Advance round-robin cursor only when records were actually packed */
  if (count > 0) {
    session->rr_cursor =
        (uint8_t)((session->rr_cursor + 1U) % (uint8_t)RUDP_MAX_CHANNELS);
  }

  /* 3. Pack the primary 4-byte datagram header at the beginning of out_buf */
  rudp_datagram_header_s d_header = {.ack = prim_chan->ctx.expected_seq_num,
                                     .ack_channel = primary_ack_channel,
                                     .count = count};

  int ret = rudp_pack_datagram_header(&d_header, out_buf, effective_max);
  if (ret != RUDP_WIRE_DATAGRAM_HEADER_SIZE) {
    if (plan) {
      plan->valid = 0;
    }
    return ret;
  }

  prim_chan->last_ack_sent = d_header.ack;
  if (count == 0) {
    prim_chan->ack_pending = 0;
    prim_chan->chan_backoff &= (uint8_t)~RUDP_CHAN_FLAG_GAP_ACK_PENDING;
  } else if (!(prim_chan->chan_backoff & RUDP_CHAN_FLAG_GAP_ACK_PENDING)) {
    prim_chan->ack_pending = 0;
  }

  if (plan) {
    plan->valid = 1;
  }

  return (int)(RUDP_WIRE_DATAGRAM_HEADER_SIZE +
               ((size_t)count * RUDP_WIRE_RECORD_SIZE));
}

int rudp_session_build_datagram(rudp_session_s *session, uint8_t primary_ack_channel,
                                uint8_t *out_buf, size_t max_len, uint32_t now,
                                uint32_t timeout) {
  return build_datagram_internal(session, primary_ack_channel, out_buf, max_len, now,
                                 timeout, NULL);
}

int rudp_session_prepare_datagram(rudp_session_s *session, uint32_t now,
                                  uint32_t timeout, uint8_t *out_buf, size_t cap,
                                  rudp_tx_plan_s *out_plan) {
  if (!session || !out_buf || !out_plan) {
    return RUDP_ERR_INVALID_ARG;
  }
  return build_datagram_internal(session, 0, out_buf, cap, now, timeout, out_plan);
}

int rudp_session_commit_datagram(rudp_session_s *session, const rudp_tx_plan_s *plan) {
  if (!session || !plan || !plan->valid) {
    return RUDP_ERR_INVALID_ARG;
  }
  return RUDP_OK;
}

int rudp_session_cancel_datagram(rudp_session_s *session, const rudp_tx_plan_s *plan) {
  if (!session || !plan || !plan->valid) {
    return RUDP_ERR_INVALID_ARG;
  }
  for (uint8_t i = plan->record_count; i > 0; --i) {
    uint8_t idx = (uint8_t)(i - 1U);
    uint8_t c = plan->channel_id[idx];
    uint16_t s = plan->slot_idx[idx];
    rudp_channel_s *chan = &session->channels[c];
    rudp_slot_s *slot = &chan->ctx.tx_buffer[s];
    slot->tx_count = plan->prev_tx_count[idx];
    slot->retries = plan->prev_retries[idx];
    slot->fast_retransmit = plan->prev_fast_retransmit[idx];
    slot->timestamp = plan->prev_timestamp[idx];
    chan->timeout_backoffs[s] = plan->prev_slot_backoff[idx];
  }
  for (uint8_t c = 0; c < RUDP_MAX_CHANNELS; ++c) {
    session->channels[c].chan_backoff = plan->prev_chan_backoff[c];
    session->channels[c].last_ack_sent = plan->prev_last_ack_sent[c];
    session->channels[c].ack_pending = plan->prev_ack_pending[c];
    session->channels[c].ctx.state = plan->prev_state[c];
  }
  session->rr_cursor = plan->prev_rr_cursor;
  return RUDP_OK;
}

uint32_t rudp_session_next_wakeup_ms(const rudp_session_s *session, uint32_t now,
                                     uint32_t fallback_timeout_ms) {
  if (!session) {
    return UINT32_MAX;
  }
  uint32_t min_wait = UINT32_MAX;
  for (uint8_t c = 0; c < RUDP_MAX_CHANNELS; ++c) {
    const rudp_channel_s *chan = &session->channels[c];
    if (chan->ctx.state != RUDP_STATE_CONNECTED) {
      continue;
    }
    if (chan->ack_pending) {
      return 0U;
    }
    uint16_t curr = chan->ctx.tail;
    while (curr != chan->ctx.head) {
      const rudp_slot_s *slot = &chan->ctx.tx_buffer[curr];
      if (slot->state == RUDP_SLOT_IN_FLIGHT) {
        if (slot->tx_count == 0 ||
            slot->fast_retransmit == RUDP_FAST_RETRANSMIT_PENDING) {
          return 0U;
        }
        if (fallback_timeout_ms == 0U && !chan->rto_min_ms) {
          return 0U;
        }
        uint32_t limit = channel_slot_timeout_ms(chan, curr, fallback_timeout_ms);
        uint32_t elapsed = (uint32_t)(now - slot->timestamp);
        if (elapsed > limit) {
          return 0U;
        }
        uint32_t rem = limit - elapsed;
        uint32_t wait_ms = (rem == UINT32_MAX) ? UINT32_MAX : (rem + 1U);
        if (wait_ms < min_wait) {
          min_wait = wait_ms;
        }
      }
      curr = (uint16_t)((curr + 1U) & (RUDP_WINDOW_SIZE - 1U));
    }
  }
  return min_wait;
}
