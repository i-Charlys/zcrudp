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

static inline uint32_t channel_slot_timeout_ms(const rudp_channel_s *chan,
                                               uint16_t slot_idx,
                                               uint32_t fallback_timeout) {
  if (chan->rto_min_ms > 0U) {
    uint32_t d = chan->rto_ms;
    uint32_t max_ms = chan->rto_max_ms;
    if (d > max_ms) {
      return max_ms;
    }
    unsigned shift = rudp_slot_backoff(&chan->ctx.tx_buffer[slot_idx]);
    unsigned chan_b = (unsigned)(chan->chan_backoff & RUDP_CHAN_BACKOFF_MASK);
    if (chan_b > shift) {
      shift = chan_b;
    }
    if (shift == 0U) {
      return d;
    }
    if (shift >= 15U || (max_ms >> shift) <= d) {
      return max_ms;
    }
    return d << shift;
  }

  if (fallback_timeout == 0U) {
    return 0U;
  }
  unsigned shift = chan->ctx.tx_buffer[slot_idx].retries;
  if (shift == 0U) {
    return fallback_timeout;
  }
  if (shift > 6U) {
    shift = 6U;
  }
  return (fallback_timeout > (UINT32_MAX >> shift)) ? UINT32_MAX
                                                    : (fallback_timeout << shift);
}

typedef struct {
  uint16_t total_in_flight;
  uint16_t total_sacked;
  uint16_t first_unsent_idx;
  uint16_t inflight_before_first_unsent;
  uint16_t sacked_before_first_unsent;
  uint32_t max_sent_elapsed;
  bool has_unsent;
  bool any_fast;
} chan_tx_scan_s;

static void inspect_channel_tail_state(const rudp_channel_s *chan, uint32_t now,
                                       chan_tx_scan_s *out) {
  chan_tx_scan_s sc = {0};
  uint16_t curr = chan->ctx.tail;
  while (curr != chan->ctx.head) {
    const rudp_slot_s *s = &chan->ctx.tx_buffer[curr];
    if (s->state == RUDP_SLOT_IN_FLIGHT) {
      if (s->tx_count == 0U) {
        if (!sc.has_unsent) {
          sc.has_unsent = true;
          sc.first_unsent_idx = curr;
          sc.inflight_before_first_unsent = sc.total_in_flight;
          sc.sacked_before_first_unsent = sc.total_sacked;
        }
      } else {
        if (rudp_slot_fast_state(s) == RUDP_FAST_RETRANSMIT_PENDING) {
          sc.any_fast = true;
        }
        uint32_t el = (uint32_t)(now - s->timestamp);
        if (el > sc.max_sent_elapsed) {
          sc.max_sent_elapsed = el;
        }
      }
      sc.total_in_flight++;
    } else if (s->state == RUDP_SLOT_UNRELIABLE) {
      if (!sc.has_unsent) {
        sc.has_unsent = true;
        sc.first_unsent_idx = curr;
        sc.inflight_before_first_unsent = sc.total_in_flight;
        sc.sacked_before_first_unsent = sc.total_sacked;
      }
      sc.total_in_flight++;
    } else if (s->state == RUDP_SLOT_SACKED) {
      sc.total_sacked++;
    }
    curr = (uint16_t)((curr + 1U) & (RUDP_WINDOW_SIZE - 1U));
  }
  *out = sc;
}

static inline uint32_t channel_min_rto_floor_ms(const rudp_channel_s *chan,
                                                uint32_t fallback_timeout) {
  if (chan->rto_min_ms > 0U) {
    return (chan->rto_ms < chan->rto_max_ms) ? chan->rto_ms : chan->rto_max_ms;
  }
  return fallback_timeout;
}

/* RFC 8985 Tail-Loss Probe deadline for an un-retransmitted slot near the end of
 * flight (< 3 un-SACKed packets above it, so FACK/Tri-ACK cannot self-trigger). */
static inline uint32_t channel_slot_pto_ms(const rudp_channel_s *chan,
                                           const rudp_slot_s *slot, bool has_unsent,
                                           uint16_t unacked_above) {
  if (has_unsent || unacked_above >= 3U || chan->rto_min_ms == 0U ||
      chan->srtt_scaled == 0U) {
    return UINT32_MAX;
  }
  if (slot->state != RUDP_SLOT_IN_FLIGHT || slot->tx_count != 1U ||
      slot->retries != 0U ||
      rudp_slot_fast_state(slot) != RUDP_FAST_RETRANSMIT_OFF) {
    return UINT32_MAX;
  }
  uint32_t pto = (chan->srtt_scaled * 3U) / 16U + 2U;
  if (pto < (uint32_t)chan->rto_min_ms) {
    pto = chan->rto_min_ms;
  }
  return pto;
}

/* RFC 8985 §6.2 RACK reorder-window timer for a slot with >= 3 SACKed packets
 * above it whose initial fast-retransmission was itself lost in transit. */
static inline uint32_t channel_slot_rack_ms(const rudp_channel_s *chan,
                                            const rudp_slot_s *slot,
                                            uint16_t sacked_above) {
  if (sacked_above < 3U || chan->rto_min_ms == 0U || chan->srtt_scaled == 0U) {
    return UINT32_MAX;
  }
  if (slot->state != RUDP_SLOT_IN_FLIGHT || slot->tx_count < 2U ||
      slot->retries < 1U || slot->retries > 2U ||
      rudp_slot_fast_state(slot) != RUDP_FAST_RETRANSMIT_OFF) {
    return UINT32_MAX;
  }
  uint32_t rack = (chan->srtt_scaled >> 3U) + (chan->rttvar_scaled >> 2U) + 2U;
  if (rack < (uint32_t)chan->rto_min_ms) {
    rack = chan->rto_min_ms;
  }
  return rack;
}

static inline uint32_t effective_slot_deadline_ms(const rudp_channel_s *chan,
                                                  uint16_t slot_idx,
                                                  uint32_t fallback_timeout,
                                                  bool has_unsent,
                                                  uint16_t unacked_above,
                                                  uint16_t sacked_above,
                                                  bool *out_is_probe) {
  uint32_t best = channel_slot_timeout_ms(chan, slot_idx, fallback_timeout);
  bool probe = false;
  const rudp_slot_s *slot = &chan->ctx.tx_buffer[slot_idx];
  if (!has_unsent && unacked_above < 3U) {
    uint32_t pto_d = channel_slot_pto_ms(chan, slot, has_unsent, unacked_above);
    if (pto_d < best) {
      best = pto_d;
      probe = true;
    }
  }
  if (sacked_above >= 3U) {
    uint32_t rack_d = channel_slot_rack_ms(chan, slot, sacked_above);
    if (rack_d < best) {
      best = rack_d;
      probe = true;
    }
  }
  if (out_is_probe) {
    *out_is_probe = probe;
  }
  return best;
}

static bool channel_has_eligible_slot(const rudp_channel_s *chan, uint32_t now,
                                      uint32_t fallback_timeout) {
  if (chan->ctx.state != RUDP_STATE_CONNECTED || chan->ctx.tail == chan->ctx.head) {
    return false;
  }
  chan_tx_scan_s sc;
  inspect_channel_tail_state(chan, now, &sc);
  if (sc.has_unsent || sc.any_fast) {
    return true;
  }
  if (sc.total_in_flight == 0U) {
    return false;
  }
  if (fallback_timeout == 0U && !chan->rto_min_ms) {
    return true;
  }
  uint32_t min_limit = channel_min_rto_floor_ms(chan, fallback_timeout);
  if (chan->rto_min_ms > 0U && chan->srtt_scaled > 0U) {
    uint32_t pto = (chan->srtt_scaled * 3U) / 16U + 2U;
    if (pto < (uint32_t)chan->rto_min_ms) {
      pto = chan->rto_min_ms;
    }
    if (pto < min_limit) {
      min_limit = pto;
    }
    if (sc.total_sacked >= 3U) {
      uint32_t rack = (chan->srtt_scaled >> 3U) + (chan->rttvar_scaled >> 2U) + 2U;
      if (rack < (uint32_t)chan->rto_min_ms) {
        rack = chan->rto_min_ms;
      }
      if (rack < min_limit) {
        min_limit = rack;
      }
    }
  }
  if (sc.max_sent_elapsed <= min_limit) {
    return false;
  }
  uint16_t rem_inflight = sc.total_in_flight;
  uint16_t rem_sacked = sc.total_sacked;
  uint16_t curr = chan->ctx.tail;
  while (curr != chan->ctx.head) {
    const rudp_slot_s *slot = &chan->ctx.tx_buffer[curr];
    if (slot->state == RUDP_SLOT_IN_FLIGHT) {
      rem_inflight--;
      uint32_t limit = effective_slot_deadline_ms(
          chan, curr, fallback_timeout, sc.has_unsent, rem_inflight, rem_sacked, NULL);
      if ((uint32_t)(now - slot->timestamp) > limit) {
        return true;
      }
    } else if (slot->state == RUDP_SLOT_SACKED) {
      rem_sacked--;
    }
    curr = (uint16_t)((curr + 1U) & (RUDP_WINDOW_SIZE - 1U));
  }
  return false;
}

static bool channel_has_ready_fec(const rudp_channel_s *chan) {
  if (!(chan->flags & RUDP_CHANNEL_FLAG_FEC_K2) ||
      chan->ctx.state != RUDP_STATE_CONNECTED || chan->ctx.tail == chan->ctx.head) {
    return false;
  }
  uint16_t curr = chan->ctx.tail;
  while (curr != chan->ctx.head) {
    const rudp_slot_s *odd = &chan->ctx.tx_buffer[curr];
    uint16_t odd_seq = odd->frame.header.seq_num;
    if ((odd_seq & 1U) == 1U && odd->state == RUDP_SLOT_IN_FLIGHT &&
        odd->tx_count >= 1U && !(odd->fast_retransmit & RUDP_SLOT_FLAG_FEC_SENT)) {
      uint16_t even_idx = (uint16_t)((curr - 1U) & (RUDP_WINDOW_SIZE - 1U));
      const rudp_slot_s *even = &chan->ctx.tx_buffer[even_idx];
      if (even->frame.header.seq_num == (uint16_t)(odd_seq - 1U) &&
          (even->state == RUDP_SLOT_IN_FLIGHT || even->state == RUDP_SLOT_SACKED) &&
          even->tx_count >= 1U) {
        return true;
      }
    }
    curr = (uint16_t)((curr + 1U) & (RUDP_WINDOW_SIZE - 1U));
  }
  return false;
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
  chan->ctx.tx_buffer[slot_idx].tx_count = 0U;
  uint8_t init_backoff =
      chan->rto_min_ms ? (uint8_t)(chan->chan_backoff & RUDP_CHAN_BACKOFF_MASK) : 0U;
  rudp_slot_set_backoff(&chan->ctx.tx_buffer[slot_idx], init_backoff);
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

int rudp_session_queue_unreliable(rudp_session_s *session, uint8_t channel_id,
                                  tfv_packet_u payload) {
  if (!session || channel_id >= RUDP_MAX_CHANNELS) {
    return RUDP_ERR_INVALID_ARG;
  }
  rudp_channel_s *chan = &session->channels[channel_id];
  if (chan->ctx.state != RUDP_STATE_CONNECTED) {
    return RUDP_ERR_DISCONNECTED;
  }
  uint16_t next_head = (uint16_t)((chan->ctx.head + 1U) & (RUDP_WINDOW_SIZE - 1U));
  if (next_head == chan->ctx.tail) {
    return RUDP_ERR_BUFFER_FULL;
  }
  rudp_slot_s *slot = &chan->ctx.tx_buffer[chan->ctx.head];
  *slot = (rudp_slot_s){
      .frame = {.header = {.seq_num = chan->next_unreliable_seq++},
                .packet = payload},
      .timestamp = 0U,
      .state = RUDP_SLOT_UNRELIABLE,
      .retries = 0U,
      .fast_retransmit = 0U,
      .tx_count = 0U,
  };
  chan->ctx.head = next_head;
  return RUDP_OK;
}

static void capture_channel_snapshot(const rudp_session_s *session,
                                     rudp_tx_plan_s *plan) {
  for (uint8_t c = 0; c < RUDP_MAX_CHANNELS; ++c) {
    plan->prev_chan_backoff[c] = session->channels[c].chan_backoff;
    plan->prev_last_ack_sent[c] = session->channels[c].last_ack_sent;
    plan->prev_ack_pending[c] = session->channels[c].ack_pending;
    plan->prev_state[c] = session->channels[c].ctx.state;
    plan->prev_tail[c] = session->channels[c].ctx.tail;
  }
  plan->prev_rr_cursor = session->rr_cursor;
  plan->record_count = 0U;
  plan->valid = 0U;
}

static void snapshot_slot_in_plan(rudp_tx_plan_s *plan, uint8_t c, uint16_t curr,
                                  const rudp_slot_s *slot) {
  uint8_t idx = plan->record_count++;
  plan->slot_idx[idx] = curr;
  plan->channel_id[idx] = c;
  plan->prev_tx_count[idx] = slot->tx_count;
  plan->prev_retries[idx] = slot->retries;
  plan->prev_fast_retransmit[idx] = slot->fast_retransmit;
  plan->prev_slot_state[idx] = slot->state;
  plan->prev_timestamp[idx] = slot->timestamp;
}

static int build_datagram_internal(rudp_session_s *session, uint8_t primary_ack_channel,
                                   uint8_t *out_buf, size_t max_len, uint32_t now,
                                   uint32_t timeout, rudp_tx_plan_s *plan,
                                   uint8_t *out_lead_channel) {
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
  size_t cap_records =
      (effective_max - RUDP_WIRE_DATAGRAM_HEADER_SIZE) / RUDP_WIRE_RECORD_SIZE;
  if (cap_records > RUDP_MAX_RECORDS_PER_DATAGRAM) {
    cap_records = RUDP_MAX_RECORDS_PER_DATAGRAM;
  }

  /* O(channels) idle check: avoid priority sorting and multi-pass scans when all
   * TX windows are empty and no secondary ACK or primary SACK record is pending. */
  bool any_tx_window_nonempty = false;
  size_t sec_ack_pending_count = 0U;
  for (uint8_t c = 0; c < RUDP_MAX_CHANNELS; ++c) {
    const rudp_channel_s *ch = &session->channels[c];
    if (ch->ctx.state == RUDP_STATE_CONNECTED) {
      if (ch->ctx.tail != ch->ctx.head) {
        any_tx_window_nonempty = true;
      }
      if (c != primary_ack_channel && ch->ack_pending) {
        sec_ack_pending_count++;
      }
    }
  }

  uint32_t prim_sack_mask =
      (prim_chan->ack_pending && prim_chan->rto_min_ms > 0U && cap_records >= 1U &&
       prim_chan->ctx.state == RUDP_STATE_CONNECTED)
          ? rudp_rx_sack_mask(prim_chan)
          : 0U;

  if (!any_tx_window_nonempty && sec_ack_pending_count == 0U && prim_sack_mask == 0U) {
    if (prim_chan->ctx.state != RUDP_STATE_CONNECTED) {
      if (plan) {
        plan->valid = 0U;
      }
      return RUDP_ERR_DISCONNECTED;
    }
    if (plan) {
      capture_channel_snapshot(session, plan);
    }
    rudp_datagram_header_s d_header = {.ack = prim_chan->ctx.expected_seq_num,
                                       .ack_channel = primary_ack_channel,
                                       .count = 0U};
    int ret = rudp_pack_datagram_header(&d_header, out_buf, effective_max);
    if (ret != RUDP_WIRE_DATAGRAM_HEADER_SIZE) {
      return ret;
    }
    prim_chan->last_ack_sent = d_header.ack;
    prim_chan->ack_pending = 0U;
    prim_chan->chan_backoff &= (uint8_t)~RUDP_CHAN_FLAG_GAP_ACK_PENDING;
    if (out_lead_channel) {
      *out_lead_channel = primary_ack_channel;
    }
    if (plan) {
      plan->valid = 1U;
    }
    return RUDP_WIRE_DATAGRAM_HEADER_SIZE;
  }

  if (plan) {
    capture_channel_snapshot(session, plan);
  }

  /* Only scan upfront for TX eligibility in the rare case where secondary ACKs could
   * fill the entire datagram and starve urgent data (sec_ack_pending_count >= cap_records). */
  size_t ack_budget = cap_records;
  if (any_tx_window_nonempty && sec_ack_pending_count >= cap_records &&
      cap_records >= 1U) {
    for (uint8_t c = 0; c < RUDP_MAX_CHANNELS; ++c) {
      if (channel_has_eligible_slot(&session->channels[c], now, timeout) ||
          channel_has_ready_fec(&session->channels[c])) {
        ack_budget = cap_records - 1U;
        break;
      }
    }
  }

  uint8_t count = 0U;
  uint8_t lead_channel = primary_ack_channel;
  bool lead_set = false;

  /* 1. Multi-channel explicit ACK + 32-bit SACK records */
  if (sec_ack_pending_count > 0U) {
    for (uint8_t i = 0; i < RUDP_MAX_CHANNELS && count < ack_budget; i++) {
      uint8_t c = (uint8_t)((session->rr_cursor + i) % (uint8_t)RUDP_MAX_CHANNELS);
      rudp_channel_s *chan = &session->channels[c];
      if (c != primary_ack_channel && chan->ctx.state == RUDP_STATE_CONNECTED &&
          chan->ack_pending) {
        rudp_record_s rec = {.channel_id = c,
                             .flags = RUDP_RECORD_FLAG_ACK,
                             .seq_num = chan->ctx.expected_seq_num,
                             .payload = rudp_mask_to_payload(rudp_rx_sack_mask(chan))};

        size_t offset =
            RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)count * RUDP_WIRE_RECORD_SIZE);
        int ret = rudp_pack_record(&rec, out_buf + offset, effective_max - offset);
        if (ret != RUDP_WIRE_RECORD_SIZE) {
          if (plan) {
            rudp_session_cancel_datagram(session, plan);
            plan->valid = 0U;
          }
          return ret;
        }

        chan->last_ack_sent = rec.seq_num;
        chan->ack_pending = 0U;
        chan->chan_backoff &= (uint8_t)~RUDP_CHAN_FLAG_GAP_ACK_PENDING;
        if (!lead_set) {
          lead_channel = c;
          lead_set = true;
        }
        count++;
      }
    }
  }

  /* 2. In-flight reliable slots & K=2 XOR FEC parity in priority order */
  if (any_tx_window_nonempty && count < cap_records) {
    uint8_t order[RUDP_MAX_CHANNELS];
    uint8_t active_order_len = 0U;
    for (size_t i = 0; i < RUDP_MAX_CHANNELS; ++i) {
      uint8_t c = (uint8_t)((session->rr_cursor + i) % (uint8_t)RUDP_MAX_CHANNELS);
      const rudp_channel_s *ch = &session->channels[c];
      if (ch->ctx.state != RUDP_STATE_CONNECTED || ch->ctx.tail == ch->ctx.head) {
        continue;
      }
      size_t j = active_order_len++;
      while (j > 0U &&
             session->channels[order[j - 1U]].priority > ch->priority) {
        order[j] = order[j - 1U];
        --j;
      }
      order[j] = c;
    }

    for (uint8_t i = 0; i < active_order_len && count < cap_records; i++) {
      uint8_t c = order[i];
      rudp_channel_s *chan = &session->channels[c];
      bool is_fec_chan = (chan->flags & RUDP_CHANNEL_FLAG_FEC_K2) != 0U;
      bool fec_touched_pair = false;
      uint16_t fec_touched_base = 0U;

      chan_tx_scan_s sc;
      inspect_channel_tail_state(chan, now, &sc);
      bool has_unsent = sc.has_unsent;
      uint16_t rem_inflight = sc.total_in_flight;
      uint16_t rem_sacked = sc.total_sacked;
      uint8_t next_chan_backoff =
          (uint8_t)(chan->chan_backoff & RUDP_CHAN_BACKOFF_MASK);
      bool cleared_recovery = false;

      uint16_t curr = chan->ctx.tail;
      if (sc.total_in_flight == 0U) {
        curr = chan->ctx.head;
      } else if (!sc.any_fast && (chan->rto_min_ms > 0U || timeout > 0U)) {
        uint32_t min_limit = channel_min_rto_floor_ms(chan, timeout);
        if (chan->rto_min_ms > 0U && chan->srtt_scaled > 0U) {
          if (!has_unsent) {
            uint32_t pto = (chan->srtt_scaled * 3U) / 16U + 2U;
            if (pto < (uint32_t)chan->rto_min_ms) {
              pto = chan->rto_min_ms;
            }
            if (pto < min_limit) {
              min_limit = pto;
            }
          }
          if (sc.total_sacked >= 3U) {
            uint32_t rack =
                (chan->srtt_scaled >> 3U) + (chan->rttvar_scaled >> 2U) + 2U;
            if (rack < (uint32_t)chan->rto_min_ms) {
              rack = chan->rto_min_ms;
            }
            if (rack < min_limit) {
              min_limit = rack;
            }
          }
        }
        if (sc.max_sent_elapsed <= min_limit) {
          if (has_unsent) {
            curr = sc.first_unsent_idx;
            rem_inflight =
                (uint16_t)(sc.total_in_flight - sc.inflight_before_first_unsent);
            rem_sacked =
                (uint16_t)(sc.total_sacked - sc.sacked_before_first_unsent);
          } else {
            curr = chan->ctx.head;
          }
        }
      }

      while (curr != chan->ctx.head && count < cap_records) {
        rudp_slot_s *slot = &chan->ctx.tx_buffer[curr];
        if (slot->state == RUDP_SLOT_SACKED) {
          rem_sacked--;
        } else if (slot->state == RUDP_SLOT_IN_FLIGHT) {
          uint16_t unacked_above = --rem_inflight;
          uint16_t sacked_above = rem_sacked;
          bool is_first_send = (slot->tx_count == 0U);
          bool is_fast = (!is_first_send &&
                          rudp_slot_fast_state(slot) == RUDP_FAST_RETRANSMIT_PENDING);
          bool is_probe = false;
          bool eligible = is_first_send || is_fast;
          if (!eligible) {
            if (timeout == 0U && !chan->rto_min_ms) {
              eligible = true;
            } else {
              uint32_t limit =
                  effective_slot_deadline_ms(chan, curr, timeout, has_unsent,
                                             unacked_above, sacked_above, &is_probe);
              eligible = ((uint32_t)(now - slot->timestamp) > limit);
            }
          }

          if (eligible) {
            uint16_t pair_base = (uint16_t)(slot->frame.header.seq_num & ~(uint16_t)1U);
            /* On FEC K=2 channels, never pack both members of a pair into the same
             * UDP datagram so a single datagram drop never destroys both A and B. */
            if (is_fec_chan && fec_touched_pair && pair_base == fec_touched_base) {
              break;
            }

            if (!is_first_send && slot->retries >= RUDP_MAX_RETRIES) {
              chan->ctx.state = RUDP_STATE_DISCONNECTED;
              if (plan) {
                plan->valid = 0U;
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
                rudp_session_cancel_datagram(session, plan);
                plan->valid = 0U;
              }
              return ret;
            }

            if (plan) {
              snapshot_slot_in_plan(plan, c, curr, slot);
            }

            if (is_first_send) {
              if (chan->rto_min_ms > 0U) {
                uint8_t cb = next_chan_backoff;
                if (cb > rudp_slot_backoff(slot)) {
                  rudp_slot_set_backoff(slot, cb);
                }
              } else {
                rudp_slot_set_backoff(slot, 0U);
              }
              slot->tx_count = 1U;
              slot->timestamp = now;
              rudp_slot_set_fast_state(slot, RUDP_FAST_RETRANSMIT_OFF);
            } else {
              if (!is_fast && !is_probe) {
                if (!cleared_recovery) {
                  cleared_recovery = true;
                  uint16_t clear = chan->ctx.tail;
                  while (clear != chan->ctx.head) {
                    if (rudp_slot_fast_state(&chan->ctx.tx_buffer[clear]) ==
                        RUDP_FAST_RETRANSMIT_IN_RECOVERY) {
                      if (plan) {
                        snapshot_slot_in_plan(plan, c, clear,
                                              &chan->ctx.tx_buffer[clear]);
                      }
                      rudp_slot_set_fast_state(&chan->ctx.tx_buffer[clear],
                                               RUDP_FAST_RETRANSMIT_OFF);
                    }
                    clear = (uint16_t)((clear + 1U) & (RUDP_WINDOW_SIZE - 1U));
                  }
                }
                uint8_t b = rudp_slot_backoff(slot);
                if (b < 15U) {
                  b++;
                  rudp_slot_set_backoff(slot, b);
                }
                if (chan->rto_min_ms > 0U && b > next_chan_backoff) {
                  next_chan_backoff = b;
                }
              }
              slot->retries++;
              if (slot->tx_count < UINT8_MAX) {
                slot->tx_count++;
              }
              slot->timestamp = now;
              rudp_slot_set_fast_state(slot, RUDP_FAST_RETRANSMIT_OFF);
            }

            if (is_fec_chan) {
              fec_touched_pair = true;
              fec_touched_base = pair_base;
            }
            lead_channel = c;
            lead_set = true;
            count++;
          }
        } else if (slot->state == RUDP_SLOT_UNRELIABLE) {
          rem_inflight--;
          rudp_record_s rec = {.channel_id = c,
                               .flags = RUDP_RECORD_FLAG_UNRELIABLE,
                               .seq_num = slot->frame.header.seq_num,
                               .payload = slot->frame.packet};
          size_t offset =
              RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)count * RUDP_WIRE_RECORD_SIZE);
          int ret = rudp_pack_record(&rec, out_buf + offset, effective_max - offset);
          if (ret != RUDP_WIRE_RECORD_SIZE) {
            if (plan) {
              rudp_session_cancel_datagram(session, plan);
              plan->valid = 0U;
            }
            return ret;
          }
          if (plan) {
            snapshot_slot_in_plan(plan, c, curr, slot);
          }
          slot->state = RUDP_SLOT_FREE;
          slot->tx_count = 0U;
          while (chan->ctx.tail != chan->ctx.head &&
                 chan->ctx.tx_buffer[chan->ctx.tail].state == RUDP_SLOT_FREE) {
            chan->ctx.tail = (uint16_t)((chan->ctx.tail + 1U) & (RUDP_WINDOW_SIZE - 1U));
          }
          lead_channel = c;
          lead_set = true;
          count++;
        }
        curr = (uint16_t)((curr + 1U) & (RUDP_WINDOW_SIZE - 1U));
      }

      if (chan->rto_min_ms > 0U &&
          next_chan_backoff > (uint8_t)(chan->chan_backoff & RUDP_CHAN_BACKOFF_MASK)) {
        chan->chan_backoff =
            (uint8_t)((chan->chan_backoff & (uint8_t)~RUDP_CHAN_BACKOFF_MASK) |
                      (next_chan_backoff & RUDP_CHAN_BACKOFF_MASK));
      }

      /* Emit ready K=2 XOR parity records for pairs not transmitted in this datagram. */
      if (is_fec_chan && count < cap_records) {
        curr = chan->ctx.tail;
        while (curr != chan->ctx.head && count < cap_records) {
          rudp_slot_s *odd = &chan->ctx.tx_buffer[curr];
          uint16_t odd_seq = odd->frame.header.seq_num;
          uint16_t even_seq = (uint16_t)(odd_seq - 1U);
          if ((odd_seq & 1U) == 1U && odd->state == RUDP_SLOT_IN_FLIGHT &&
              odd->tx_count >= 1U &&
              !(odd->fast_retransmit & RUDP_SLOT_FLAG_FEC_SENT) &&
              !(fec_touched_pair && even_seq == fec_touched_base)) {
            uint16_t even_idx = (uint16_t)((curr - 1U) & (RUDP_WINDOW_SIZE - 1U));
            const rudp_slot_s *even = &chan->ctx.tx_buffer[even_idx];
            if (even->frame.header.seq_num == even_seq &&
                (even->state == RUDP_SLOT_IN_FLIGHT ||
                 even->state == RUDP_SLOT_SACKED) &&
                even->tx_count >= 1U) {
              rudp_record_s prec = {
                  .channel_id = c,
                  .flags = RUDP_RECORD_FLAG_FEC_PARITY,
                  .seq_num = even_seq,
                  .payload = rudp_payload_xor(even->frame.packet, odd->frame.packet),
              };
              size_t offset = RUDP_WIRE_DATAGRAM_HEADER_SIZE +
                              ((size_t)count * RUDP_WIRE_RECORD_SIZE);
              int ret = rudp_pack_record(&prec, out_buf + offset, effective_max - offset);
              if (ret != RUDP_WIRE_RECORD_SIZE) {
                if (plan) {
                  plan->valid = 0U;
                }
                return ret;
              }
              if (plan) {
                snapshot_slot_in_plan(plan, c, curr, odd);
              }
              odd->fast_retransmit |= RUDP_SLOT_FLAG_FEC_SENT;
              if (!lead_set) {
                lead_channel = c;
                lead_set = true;
              }
              count++;
            }
          }
          curr = (uint16_t)((curr + 1U) & (RUDP_WINDOW_SIZE - 1U));
        }
      }
    }
  }

  /* Emit an explicit ACK + 32-bit SACK record on the primary channel when:
   * - data was bundled alongside a gap/duplicate ACK (preserving bidirectional Tri-ACK), OR
   * - count == 0 in adaptive mode and out-of-order SACK blocks exist (`prim_sack_mask != 0`). */
  if (count < cap_records && prim_chan->ctx.state == RUDP_STATE_CONNECTED &&
      prim_chan->ack_pending &&
      ((count > 0U && (prim_chan->chan_backoff & RUDP_CHAN_FLAG_GAP_ACK_PENDING)) ||
       (count == 0U && prim_sack_mask != 0U))) {
    uint32_t mask = (count == 0U) ? prim_sack_mask : rudp_rx_sack_mask(prim_chan);
    rudp_record_s ack_rec = {.channel_id = primary_ack_channel,
                             .flags = RUDP_RECORD_FLAG_ACK,
                             .seq_num = prim_chan->ctx.expected_seq_num,
                             .payload = rudp_mask_to_payload(mask)};
    size_t offset =
        RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)count * RUDP_WIRE_RECORD_SIZE);
    int ret = rudp_pack_record(&ack_rec, out_buf + offset, effective_max - offset);
    if (ret != RUDP_WIRE_RECORD_SIZE) {
      if (plan) {
        plan->valid = 0U;
      }
      return ret;
    }
    prim_chan->chan_backoff &= (uint8_t)~RUDP_CHAN_FLAG_GAP_ACK_PENDING;
    if (!lead_set) {
      lead_channel = primary_ack_channel;
      lead_set = true;
    }
    count++;
  }

  if (count == 0U && prim_chan->ctx.state != RUDP_STATE_CONNECTED) {
    if (plan) {
      plan->valid = 0U;
    }
    return RUDP_ERR_DISCONNECTED;
  }

  if (count > 0U) {
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
      plan->valid = 0U;
    }
    return ret;
  }

  prim_chan->last_ack_sent = d_header.ack;
  if (count == 0U) {
    prim_chan->ack_pending = 0U;
    prim_chan->chan_backoff &= (uint8_t)~RUDP_CHAN_FLAG_GAP_ACK_PENDING;
  } else if (!(prim_chan->chan_backoff & RUDP_CHAN_FLAG_GAP_ACK_PENDING)) {
    prim_chan->ack_pending = 0U;
  }

  if (out_lead_channel) {
    *out_lead_channel = lead_channel;
  }
  if (plan) {
    plan->valid = 1U;
  }

  return (int)(RUDP_WIRE_DATAGRAM_HEADER_SIZE +
               ((size_t)count * RUDP_WIRE_RECORD_SIZE));
}

int rudp_session_build_datagram(rudp_session_s *session, uint8_t primary_ack_channel,
                                uint8_t *out_buf, size_t max_len, uint32_t now,
                                uint32_t timeout) {
  return build_datagram_internal(session, primary_ack_channel, out_buf, max_len, now,
                                 timeout, NULL, NULL);
}

int rudp_session_prepare_datagram(rudp_session_s *session, uint32_t now,
                                  uint32_t timeout, uint8_t *out_buf, size_t cap,
                                  rudp_tx_plan_s *out_plan) {
  if (!session || !out_buf || !out_plan) {
    return RUDP_ERR_INVALID_ARG;
  }
  return build_datagram_internal(session, 0U, out_buf, cap, now, timeout, out_plan,
                                 NULL);
}

static void populate_iovec_from_built(const rudp_session_s *session,
                                      uint8_t lead_channel, uint16_t flow_hint,
                                      const uint8_t *scratch_buf, int built_len,
                                      rudp_iovec_collection_s *out_col) {
  out_col->iovecs[0] = (rudp_iovec_s){
      .base = scratch_buf,
      .len = RUDP_WIRE_DATAGRAM_HEADER_SIZE,
  };
  if (built_len > RUDP_WIRE_DATAGRAM_HEADER_SIZE && RUDP_IOVEC_MAX >= 2) {
    out_col->iovecs[1] = (rudp_iovec_s){
        .base = scratch_buf + RUDP_WIRE_DATAGRAM_HEADER_SIZE,
        .len = (uint32_t)(built_len - RUDP_WIRE_DATAGRAM_HEADER_SIZE),
    };
    out_col->count = 2U;
  } else {
    out_col->iovecs[0].len = (uint32_t)built_len;
    out_col->count = 1U;
  }
  out_col->total_len = (uint16_t)built_len;
  out_col->dscp = session->channels[lead_channel].dscp;
  out_col->path_entropy =
      rudp_calc_entropy(session->session_seed, lead_channel, flow_hint);
}

int rudp_session_build_iovec(rudp_session_s *session, uint8_t primary_ack_channel,
                             uint16_t flow_hint, uint8_t *scratch_buf, size_t max_len,
                             uint32_t now, uint32_t timeout,
                             rudp_iovec_collection_s *out_col) {
  if (!out_col) {
    return RUDP_ERR_INVALID_ARG;
  }
  uint8_t lead_channel = primary_ack_channel;
  int len = build_datagram_internal(session, primary_ack_channel, scratch_buf, max_len,
                                    now, timeout, NULL, &lead_channel);
  if (len < 0) {
    return len;
  }
  populate_iovec_from_built(session, lead_channel, flow_hint, scratch_buf, len, out_col);
  return len;
}

int rudp_session_prepare_iovec(rudp_session_s *session, uint16_t flow_hint,
                               uint32_t now, uint32_t timeout, uint8_t *scratch_buf,
                               size_t cap, rudp_iovec_collection_s *out_col,
                               rudp_tx_plan_s *out_plan) {
  if (!session || !scratch_buf || !out_col || !out_plan) {
    return RUDP_ERR_INVALID_ARG;
  }
  uint8_t lead_channel = 0U;
  int len = build_datagram_internal(session, 0U, scratch_buf, cap, now, timeout,
                                    out_plan, &lead_channel);
  if (len < 0) {
    return len;
  }
  populate_iovec_from_built(session, lead_channel, flow_hint, scratch_buf, len, out_col);
  return len;
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
  for (uint8_t i = plan->record_count; i > 0U; --i) {
    uint8_t idx = (uint8_t)(i - 1U);
    uint8_t c = plan->channel_id[idx];
    uint16_t s = plan->slot_idx[idx];
    rudp_slot_s *slot = &session->channels[c].ctx.tx_buffer[s];
    slot->tx_count = plan->prev_tx_count[idx];
    slot->retries = plan->prev_retries[idx];
    slot->fast_retransmit = plan->prev_fast_retransmit[idx];
    slot->state = plan->prev_slot_state[idx];
    slot->timestamp = plan->prev_timestamp[idx];
  }
  for (uint8_t c = 0; c < RUDP_MAX_CHANNELS; ++c) {
    session->channels[c].chan_backoff = plan->prev_chan_backoff[c];
    session->channels[c].last_ack_sent = plan->prev_last_ack_sent[c];
    session->channels[c].ack_pending = plan->prev_ack_pending[c];
    session->channels[c].ctx.state = plan->prev_state[c];
    session->channels[c].ctx.tail = plan->prev_tail[c];
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
    if (chan->ack_pending || channel_has_ready_fec(chan)) {
      return 0U;
    }
    if (chan->ctx.tail == chan->ctx.head) {
      continue;
    }
    chan_tx_scan_s sc;
    inspect_channel_tail_state(chan, now, &sc);
    if (sc.has_unsent || sc.any_fast) {
      return 0U;
    }
    bool has_unsent = false;
    uint16_t rem_inflight = sc.total_in_flight;
    uint16_t rem_sacked = sc.total_sacked;

    uint16_t curr = chan->ctx.tail;
    while (curr != chan->ctx.head) {
      const rudp_slot_s *slot = &chan->ctx.tx_buffer[curr];
      if (slot->state == RUDP_SLOT_SACKED) {
        rem_sacked--;
      } else if (slot->state == RUDP_SLOT_IN_FLIGHT) {
        uint16_t unacked_above = --rem_inflight;
        if (rudp_slot_fast_state(slot) == RUDP_FAST_RETRANSMIT_PENDING) {
          return 0U;
        }
        if (fallback_timeout_ms == 0U && !chan->rto_min_ms) {
          return 0U;
        }
        uint32_t limit = effective_slot_deadline_ms(
            chan, curr, fallback_timeout_ms, has_unsent, unacked_above, rem_sacked, NULL);
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

bool rudp_session_has_pending_work(const rudp_session_s *session, uint32_t now,
                                   uint32_t fallback_timeout_ms) {
  return rudp_session_next_wakeup_ms(session, now, fallback_timeout_ms) == 0U;
}
