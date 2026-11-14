#include "rudp_session.h"
#include "rx_internal.h"
#include "rudp_reliability.h"
#include "rudp_codec.h"
#include <string.h>

static void apply_rtt_sample(rudp_channel_s *chan, uint32_t sample, uint32_t now) {
  if (sample > 60000U) {
    return;
  }
  /* Any clean Karn-eligible acknowledgement proves path liveness and clears
   * inherited channel congestion/route backoff, even within the 1-SRTT cadence. */
  chan->chan_backoff &= (uint8_t)~RUDP_CHAN_BACKOFF_MASK;
  if (!sample) {
    sample = 1U;
  }
  if (chan->srtt_scaled &&
      (uint32_t)(now - chan->last_rtt_sample) < chan->srtt_scaled / 8U) {
    return;
  }
  if (!chan->srtt_scaled) {
    chan->srtt_scaled = sample * 8U;
    chan->rttvar_scaled = sample * 2U;
  } else {
    uint32_t mean = chan->srtt_scaled / 8U;
    uint32_t error = (sample > mean) ? (sample - mean) : (mean - sample);
    chan->rttvar_scaled = chan->rttvar_scaled - chan->rttvar_scaled / 4U + error;
    chan->srtt_scaled = chan->srtt_scaled - chan->srtt_scaled / 8U + sample;
  }
  uint32_t rto =
      chan->srtt_scaled / 8U + (chan->rttvar_scaled ? chan->rttvar_scaled : 1U);
  if (rto < chan->rto_min_ms) {
    rto = chan->rto_min_ms;
  }
  if (rto > chan->rto_max_ms) {
    rto = chan->rto_max_ms;
  }
  chan->rto_ms = (uint16_t)rto;
  chan->last_rtt_sample = now;
}

static uint16_t find_first_reliable_slot(const rudp_context_s *ctx) {
  uint16_t curr = ctx->tail;
  while (curr != ctx->head &&
         (ctx->tx_buffer[curr].state == RUDP_SLOT_FREE ||
          ctx->tx_buffer[curr].state == RUDP_SLOT_UNRELIABLE)) {
    curr = (uint16_t)((curr + 1U) & (RUDP_WINDOW_SIZE - 1U));
  }
  return curr;
}

static void session_ack_and_sack(rudp_channel_s *chan, uint16_t ack, uint32_t sack_mask,
                                 bool duplicates, bool timed, uint32_t now,
                                 bool allow_sack_rtt, bool is_ext_sack,
                                 rudp_stats_s *stats) {
  rudp_context_s *ctx = &chan->ctx;
  if (ctx->state != RUDP_STATE_CONNECTED) {
    return;
  }

  uint16_t prev_tail = ctx->tail;
  uint16_t first_rel = find_first_reliable_slot(ctx);
  uint32_t sample = 0U;
  bool eligible =
      !is_ext_sack && timed && (chan->rto_min_ms > 0U) && (first_rel != ctx->head);
  if (eligible) {
    uint16_t advance =
        (uint16_t)(ack - ctx->tx_buffer[first_rel].frame.header.seq_num);
    uint16_t pending = (uint16_t)((ctx->head - ctx->tail) & (RUDP_WINDOW_SIZE - 1U));
    eligible = (advance > 0U) && (advance <= pending);
    if (eligible) {
      uint16_t acked_rel = 0U;
      uint16_t scan = first_rel;
      while (scan != ctx->head) {
        const rudp_slot_s *slot = &ctx->tx_buffer[scan];
        if (slot->state == RUDP_SLOT_FREE || slot->state == RUDP_SLOT_UNRELIABLE) {
          scan = (uint16_t)((scan + 1U) & (RUDP_WINDOW_SIZE - 1U));
          continue;
        }
        if ((int16_t)(ack - slot->frame.header.seq_num) <= 0) {
          break;
        }
        /* Conservative Karn: skip cumulative ranges containing any repair or SACKed
         * slot whose RTT was already sampled. */
        if (slot->state != RUDP_SLOT_IN_FLIGHT || slot->tx_count != 1U ||
            slot->retries != 0U) {
          eligible = false;
          break;
        }
        sample = (uint32_t)(now - slot->timestamp);
        acked_rel++;
        scan = (uint16_t)((scan + 1U) & (RUDP_WINDOW_SIZE - 1U));
      }
      if (acked_rel != advance) {
        eligible = false;
      }
    }
  }

  if (!is_ext_sack) {
    int result = rudp_recv_ack_ex(ctx, ack, duplicates);
    if (result != RUDP_OK) {
      if (stats) {
        rudp_stat_inc(&stats->rx_ack_rejected);
      }
      return;
    }

    if (chan->srtt_scaled > 0U && ctx->tail != prev_tail) {
      chan->chan_backoff &= (uint8_t)~RUDP_CHAN_BACKOFF_MASK;
    }
    if (eligible) {
      apply_rtt_sample(chan, sample, now);
    }
  }

  first_rel = find_first_reliable_slot(ctx);
  if (sack_mask == 0U || first_rel == ctx->head) {
    return;
  }

  uint16_t tail_seq = ctx->tx_buffer[first_rel].frame.header.seq_num;
  uint16_t base_offset = (uint16_t)(ack - tail_seq);
  if ((!is_ext_sack && base_offset != 0U) || (is_ext_sack && base_offset != 32U)) {
    return;
  }

  uint16_t rel_slots[65];
  uint16_t rel_count = 0U;
  uint16_t scan = first_rel;
  while (scan != ctx->head) {
    const rudp_slot_s *s = &ctx->tx_buffer[scan];
    if (s->state == RUDP_SLOT_IN_FLIGHT || s->state == RUDP_SLOT_SACKED) {
      if (rel_count < 65U) {
        rel_slots[rel_count] = scan;
      }
      rel_count++;
    }
    scan = (uint16_t)((scan + 1U) & (RUDP_WINDOW_SIZE - 1U));
  }

  /* Validate all SACK bits BEFORE mutating slot state: reject forged SACKs
   * targeting sequences ahead of head or queued slots with tx_count == 0. */
  unsigned highest_sack_bit = 0U;
  for (unsigned b = 0U; b < 32U; ++b) {
    if ((sack_mask >> b) & UINT32_C(1)) {
      highest_sack_bit = b;
      uint16_t offset = (uint16_t)(base_offset + 1U + b);
      if (offset >= rel_count) {
        if (stats) {
          rudp_stat_inc(&stats->rx_ack_rejected);
        }
        return;
      }
      rudp_slot_s *s = &ctx->tx_buffer[rel_slots[offset]];
      if (s->state == RUDP_SLOT_FREE || s->tx_count == 0U) {
        if (stats) {
          rudp_stat_inc(&stats->rx_ack_rejected);
        }
        return;
      }
    }
  }

  bool any_sacked_above_mask = false;
  uint16_t above_scan =
      (uint16_t)((rel_slots[base_offset + 1U + highest_sack_bit] + 1U) &
                 (RUDP_WINDOW_SIZE - 1U));
  while (above_scan != ctx->head) {
    if (ctx->tx_buffer[above_scan].state == RUDP_SLOT_SACKED) {
      any_sacked_above_mask = true;
      break;
    }
    above_scan = (uint16_t)((above_scan + 1U) & (RUDP_WINDOW_SIZE - 1U));
  }

  unsigned newly_sacked_count = 0U;
  bool top_newly_sacked_clean = false;
  uint32_t sack_sample = 0U;
  for (unsigned b = 0U; b < 32U; ++b) {
    if ((sack_mask >> b) & UINT32_C(1)) {
      uint16_t offset = (uint16_t)(base_offset + 1U + b);
      rudp_slot_s *s = &ctx->tx_buffer[rel_slots[offset]];
      if (s->state == RUDP_SLOT_IN_FLIGHT) {
        newly_sacked_count++;
        if (b == highest_sack_bit && timed && chan->rto_min_ms > 0U &&
            s->tx_count == 1U && s->retries == 0U) {
          sack_sample = (uint32_t)(now - s->timestamp);
          top_newly_sacked_clean = true;
        }
        s->state = RUDP_SLOT_SACKED;
        rudp_slot_set_fast_state(s, RUDP_FAST_RETRANSMIT_OFF);
        if (stats) {
          rudp_stat_inc(&stats->rx_sack_blocks);
        }
      }
    }
  }

  /* Strict Karn SACK RTT (RFC 6675): sample only when tail did not just jump
   * (which would expose long-buffered >32-offset packets into the SACK bitmap)
   * and a single fresh top-of-flight SACK block triggered this feedback. */
  if (!eligible && allow_sack_rtt && ctx->tail == prev_tail &&
      !any_sacked_above_mask && newly_sacked_count == 1U && top_newly_sacked_clean) {
    apply_rtt_sample(chan, sack_sample, now);
  }

  /* RFC 6675 FACK Multi-Loss Recovery: scan backwards from highest pending slot
   * and arm fast retransmit on any unrepaired hole with >= 3 SACKed slots above it. */
  uint16_t sacked_above = 0U;
  uint16_t cur = ctx->head;
  while (cur != ctx->tail) {
    cur = (uint16_t)((cur - 1U) & (RUDP_WINDOW_SIZE - 1U));
    rudp_slot_s *s = &ctx->tx_buffer[cur];
    if (s->state == RUDP_SLOT_SACKED) {
      sacked_above++;
    } else if (s->state == RUDP_SLOT_IN_FLIGHT && s->tx_count > 0U &&
               s->retries == 0U && sacked_above >= 3U) {
      rudp_slot_set_fast_state(s, RUDP_FAST_RETRANSMIT_PENDING);
      if (cur == first_rel && ctx->duplicate_ack_count < 3U) {
        ctx->last_ack_received = ack;
        ctx->duplicate_ack_count = 3U;
      }
    }
  }
}

int rudp_session_process_datagram_ex(rudp_session_s *session, const uint8_t *in_buf,
                                     size_t in_len, rudp_record_s *out_delivered,
                                     size_t max_delivered, bool timed, uint32_t now,
                                     rudp_stats_s *stats) {
  if (!session || !in_buf || in_len < RUDP_WIRE_DATAGRAM_HEADER_SIZE) {
    if (stats) {
      rudp_stat_inc(&stats->rx_malformed);
    }
    return RUDP_ERR_INVALID_ARG;
  }

  rudp_datagram_header_s header;
  int ret = rudp_unpack_datagram_header(in_buf, in_len, &header);
  if (ret != RUDP_OK) {
    if (stats) {
      rudp_stat_inc(&stats->rx_malformed);
    }
    return ret;
  }

  if (header.ack_channel >= RUDP_MAX_CHANNELS) {
    if (stats) {
      rudp_stat_inc(&stats->rx_malformed);
    }
    return RUDP_ERR_INVALID_ARG;
  }

  size_t expected_len =
      RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)header.count * RUDP_WIRE_RECORD_SIZE);
  if (in_len != expected_len) {
    if (stats) {
      rudp_stat_inc(&stats->rx_malformed);
    }
    return RUDP_ERR_INVALID_ARG;
  }

  /* PASS 1: Atomicity & Integrity Validation */
  bool needs_delivery = false;
  for (uint8_t i = 0; i < header.count; i++) {
    size_t offset =
        RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)i * RUDP_WIRE_RECORD_SIZE);
    rudp_record_s rec;
    ret = rudp_unpack_record(in_buf + offset, in_len - offset, &rec);
    if (ret != RUDP_OK || rec.channel_id >= RUDP_MAX_CHANNELS) {
      if (stats) {
        rudp_stat_inc(&stats->rx_malformed);
      }
      return RUDP_ERR_INVALID_ARG;
    }
    if (rec.flags != RUDP_RECORD_FLAG_UNRELIABLE &&
        rec.flags != RUDP_RECORD_FLAG_RELIABLE && rec.flags != RUDP_RECORD_FLAG_ACK &&
        rec.flags != RUDP_RECORD_FLAG_FEC_PARITY) {
      if (stats) {
        rudp_stat_inc(&stats->rx_malformed);
      }
      return RUDP_ERR_INVALID_ARG;
    }
    if ((rec.flags == RUDP_RECORD_FLAG_RELIABLE ||
         rec.flags == RUDP_RECORD_FLAG_FEC_PARITY) &&
        !(session->channels[rec.channel_id].flags & RUDP_CHANNEL_FLAG_RELIABLE)) {
      if (stats) {
        rudp_stat_inc(&stats->rx_malformed);
      }
      return RUDP_ERR_INVALID_ARG;
    }
    if (rec.flags == RUDP_RECORD_FLAG_FEC_PARITY && (rec.seq_num & 1U) != 0U) {
      if (stats) {
        rudp_stat_inc(&stats->rx_malformed);
      }
      return RUDP_ERR_INVALID_ARG;
    }
    if (rec.flags != RUDP_RECORD_FLAG_ACK) {
      needs_delivery = true;
    }
#if RUDP_EXTERNAL_RX_STORAGE
    if ((rec.flags == RUDP_RECORD_FLAG_RELIABLE ||
         rec.flags == RUDP_RECORD_FLAG_FEC_PARITY) &&
        !session->channels[rec.channel_id].rx_present) {
      if (stats) {
        rudp_stat_inc(&stats->rx_malformed);
      }
      return RUDP_ERR_INVALID_ARG;
    }
#endif
  }

  if (needs_delivery && (!out_delivered || max_delivered == 0)) {
    if (stats) {
      rudp_stat_inc(&stats->rx_malformed);
    }
    return RUDP_ERR_INVALID_ARG;
  }

  if (stats) {
    rudp_stat_inc(&stats->rx_datagrams);
  }

  /* PASS 2: State Mutation and Delivery */
  rudp_channel_s *ack_chan = &session->channels[header.ack_channel];
  bool hdr_tail_advanced = false;
  if (ack_chan->ctx.state == RUDP_STATE_CONNECTED) {
    uint16_t before_tail = ack_chan->ctx.tail;
    if (timed) {
      rudp_touch(&ack_chan->ctx, now);
    }
    /* Passive piggyback on datagrams with records (count > 0) does NOT count duplicate
     * ACKs (RFC 5681); explicit RUDP_RECORD_FLAG_ACK records handle SACK & dup-ACKs. */
    session_ack_and_sack(ack_chan, header.ack, 0U, header.count == 0, timed, now,
                         false, false, stats);
    hdr_tail_advanced = (ack_chan->ctx.tail != before_tail);
  }

  if (header.count == 0) {
    return 0;
  }

  uint8_t ack_record_seen[RUDP_MAX_CHANNELS] = {0};
  int delivered_count = 0;

  for (uint8_t i = 0; i < header.count; i++) {
    size_t offset =
        RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)i * RUDP_WIRE_RECORD_SIZE);
    rudp_record_s rec;
    (void)rudp_unpack_record(in_buf + offset, in_len - offset, &rec);

    rudp_channel_s *chan = &session->channels[rec.channel_id];
    if (chan->ctx.state != RUDP_STATE_CONNECTED) {
      continue;
    }
    if (timed) {
      rudp_touch(&chan->ctx, now);
    }

    if (rec.flags == RUDP_RECORD_FLAG_ACK) {
      /* Allow primary SACK (offsets 1..32) and one extended SACK (offsets 33..63)
       * per channel per datagram; only the first counts duplicate ACKs. */
      if (ack_record_seen[rec.channel_id] < 2U) {
        uint8_t seen = ack_record_seen[rec.channel_id]++;
        bool allow_sack_rtt =
            !(rec.channel_id == header.ack_channel && hdr_tail_advanced);
        session_ack_and_sack(chan, rec.seq_num, rudp_payload_to_mask(rec.payload),
                             seen == 0U, timed, now, allow_sack_rtt, seen > 0U,
                             stats);
      }
    } else if (rec.flags == RUDP_RECORD_FLAG_RELIABLE) {
      bool oow = false;
      delivered_count += rudp_rx_accept(chan, &rec, out_delivered + delivered_count,
                                        max_delivered - (size_t)delivered_count, &oow);
      if (oow && stats) {
        rudp_stat_inc(&stats->rx_out_of_window);
      }
    } else if (rec.flags == RUDP_RECORD_FLAG_FEC_PARITY) {
      bool recovered = false;
      delivered_count += rudp_rx_accept_fec(chan, &rec, out_delivered + delivered_count,
                                            max_delivered - (size_t)delivered_count,
                                            &recovered);
      if (recovered && stats) {
        rudp_stat_inc(&stats->rx_fec_recoveries);
      }
    } else {
      /* Unreliable delivery via 16-bit anti-rollback filter + outage resync */
      if (!(chan->has_unreliable_seq & RUDP_UNREL_HAS_SEQ_MASK)) {
        if ((size_t)delivered_count < max_delivered) {
          chan->last_unreliable_seq = rec.seq_num;
          chan->has_unreliable_seq |= RUDP_UNREL_HAS_SEQ_MASK;
          chan->unrel_ahead_streak &= (uint8_t)~RUDP_CHAN_STREAK_MASK;
          out_delivered[delivered_count++] = rec;
        }
      } else {
        uint16_t distance = (uint16_t)(rec.seq_num - chan->last_unreliable_seq);
        if (distance != 0U && distance <= RUDP_UNRELIABLE_MAX_AHEAD) {
          if ((size_t)delivered_count < max_delivered) {
            chan->last_unreliable_seq = rec.seq_num;
            chan->unrel_ahead_streak &= (uint8_t)~RUDP_CHAN_STREAK_MASK;
            out_delivered[delivered_count++] = rec;
          }
        } else if (distance == 0U) {
          chan->unrel_ahead_streak &= (uint8_t)~RUDP_CHAN_STREAK_MASK;
        } else {
          uint8_t streak = (uint8_t)(chan->unrel_ahead_streak & RUDP_CHAN_STREAK_MASK);
          uint16_t cand_dist = (uint16_t)(rec.seq_num - chan->unrel_candidate_seq);
          if (streak == 0U || cand_dist == 0U ||
              cand_dist > RUDP_UNRELIABLE_MAX_AHEAD) {
            streak = 1U;
          } else {
            streak++;
          }
          chan->unrel_candidate_seq = rec.seq_num;
          if (streak >= RUDP_UNREL_RESYNC_STREAK &&
              (size_t)delivered_count < max_delivered) {
            chan->last_unreliable_seq = rec.seq_num;
            chan->unrel_ahead_streak &= (uint8_t)~RUDP_CHAN_STREAK_MASK;
            out_delivered[delivered_count++] = rec;
            if (stats) {
              rudp_stat_inc(&stats->rx_unreliable_resyncs);
            }
          } else {
            chan->unrel_ahead_streak =
                (uint8_t)((chan->unrel_ahead_streak & (uint8_t)~RUDP_CHAN_STREAK_MASK) |
                          (streak & RUDP_CHAN_STREAK_MASK));
            if (stats) {
              rudp_stat_inc(&stats->rx_unreliable_jump_rejected);
            }
          }
        }
      }
    }
  }

  return delivered_count;
}

int rudp_session_process_datagram(rudp_session_s *session, const uint8_t *in,
                                  size_t length, rudp_record_s *out, size_t capacity) {
  return rudp_session_process_datagram_ex(session, in, length, out, capacity, false, 0U,
                                          NULL);
}

int rudp_session_process_datagram_at(rudp_session_s *session, const uint8_t *in,
                                     size_t length, rudp_record_s *out, size_t capacity,
                                     uint32_t now) {
  return rudp_session_process_datagram_ex(session, in, length, out, capacity, true, now,
                                          NULL);
}

int rudp_session_poll(rudp_session_s *session, rudp_record_s *out, size_t capacity) {
  if (!session || !out || !capacity) {
    return RUDP_ERR_INVALID_ARG;
  }
  int count = 0;
  for (size_t i = 0; i < RUDP_MAX_CHANNELS && (size_t)count < capacity; ++i) {
    count +=
        rudp_rx_drain(&session->channels[i], out + count, capacity - (size_t)count);
  }
  return count;
}
