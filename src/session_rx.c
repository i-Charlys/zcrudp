#include "rudp_session.h"
#include "rx_internal.h"
#include "rudp_reliability.h"
#include "rudp_codec.h"
#include <string.h>

static void session_ack(rudp_channel_s *chan, uint16_t ack, bool duplicates, bool timed,
                        uint32_t now) {
  rudp_context_s *ctx = &chan->ctx;
  uint32_t sample = 0;
  bool eligible = timed && chan->rto_min_ms && ctx->tail != ctx->head;
  if (eligible) {
    uint16_t advance = (uint16_t)(ack - ctx->tx_buffer[ctx->tail].frame.header.seq_num);
    uint16_t pending = (ctx->head - ctx->tail) & (RUDP_WINDOW_SIZE - 1);
    eligible = advance > 0 && advance <= pending;
    for (uint16_t n = 0; eligible && n < advance; ++n) {
      rudp_slot_s *slot = &ctx->tx_buffer[(ctx->tail + n) & (RUDP_WINDOW_SIZE - 1)];
      /* Conservative Karn: skip cumulative ranges containing any repair. */
      if (slot->tx_count != 1 || slot->retries)
        eligible = false;
      else
        sample = (uint32_t)(now - slot->timestamp);
    }
  }
  int result = rudp_recv_ack_ex(ctx, ack, duplicates);
  if (result != RUDP_OK || !eligible || sample > 60000)
    return;
  if (!sample)
    sample = 1;
  if (chan->srtt_scaled &&
      (uint32_t)(now - chan->last_rtt_sample) < chan->srtt_scaled / 8)
    return;
  if (!chan->srtt_scaled) {
    chan->srtt_scaled = sample * 8;
    chan->rttvar_scaled = sample * 2;
  } else {
    uint32_t mean = chan->srtt_scaled / 8;
    uint32_t error = sample > mean ? sample - mean : mean - sample;
    chan->rttvar_scaled = chan->rttvar_scaled - chan->rttvar_scaled / 4 + error;
    chan->srtt_scaled = chan->srtt_scaled - chan->srtt_scaled / 8 + sample;
  }
  uint32_t rto =
      chan->srtt_scaled / 8 + (chan->rttvar_scaled ? chan->rttvar_scaled : 1);
  if (rto < chan->rto_min_ms)
    rto = chan->rto_min_ms;
  if (rto > chan->rto_max_ms)
    rto = chan->rto_max_ms;
  chan->rto_ms = rto;
  chan->last_rtt_sample = now;
}

static int process_datagram(rudp_session_s *session, const uint8_t *in_buf,
                            size_t in_len, rudp_record_s *out_delivered,
                            size_t max_delivered, bool timed, uint32_t now) {
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

  size_t expected_len =
      RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)header.count * RUDP_WIRE_RECORD_SIZE);
  if (in_len != expected_len) {
    return RUDP_ERR_INVALID_ARG;
  }

  /* PASS 1: Atomicity & Integrity Validation
   * Verify all records unpack successfully and have valid channel IDs BEFORE mutating
   * any session state. */
  if (header.count > 0 && (!out_delivered || max_delivered == 0)) {
    return RUDP_ERR_INVALID_ARG;
  }

  for (uint8_t i = 0; i < header.count; i++) {
    size_t offset =
        RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)i * RUDP_WIRE_RECORD_SIZE);
    rudp_record_s rec;
    ret = rudp_unpack_record(in_buf + offset, in_len - offset, &rec);
    if (ret != RUDP_OK) {
      return ret;
    }
    if (rec.channel_id >= RUDP_MAX_CHANNELS) {
      return RUDP_ERR_INVALID_ARG;
    }
#if RUDP_EXTERNAL_RX_STORAGE
    if (!(rec.flags & RUDP_RECORD_FLAG_ACK) &&
        (rec.flags & RUDP_RECORD_FLAG_RELIABLE) &&
        !session->channels[rec.channel_id].rx_present) {
      return RUDP_ERR_INVALID_ARG;
    }
#endif
  }

  /* PASS 2: State Mutation and Delivery */
  /* 1. Dispatch piggybacked ACK to target channel sliding window.
   * Note: Passive piggyback on datagrams with data (count > 0) does NOT count duplicate
   * ACKs (RFC 5681). */
  session_ack(&session->channels[header.ack_channel], header.ack, header.count == 0,
              timed, now);

  /* If standalone ACK (count == 0), no records to deliver */
  if (header.count == 0) {
    return 0;
  }

  /* 2. Process bundled message records */
  int delivered_count = 0;
  for (uint8_t i = 0; i < header.count; i++) {
    size_t offset =
        RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)i * RUDP_WIRE_RECORD_SIZE);
    rudp_record_s rec;
    (void)rudp_unpack_record(in_buf + offset, in_len - offset, &rec);

    rudp_channel_s *chan = &session->channels[rec.channel_id];

    if (rec.flags & RUDP_RECORD_FLAG_ACK) {
      /* Explicit multi-channel ACK record: routes cumulative ACK and counts duplicate
       * ACKs */
      session_ack(chan, rec.seq_num, true, timed, now);
    } else if (rec.flags & RUDP_RECORD_FLAG_RELIABLE) {
      delivered_count += rudp_rx_accept(chan, &rec, out_delivered + delivered_count,
                                        max_delivered - (size_t)delivered_count);
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

int rudp_session_process_datagram(rudp_session_s *session, const uint8_t *in,
                                  size_t length, rudp_record_s *out, size_t capacity) {
  return process_datagram(session, in, length, out, capacity, false, 0);
}

int rudp_session_process_datagram_at(rudp_session_s *session, const uint8_t *in,
                                     size_t length, rudp_record_s *out, size_t capacity,
                                     uint32_t now) {
  return process_datagram(session, in, length, out, capacity, true, now);
}

int rudp_session_poll(rudp_session_s *session, rudp_record_s *out, size_t capacity) {
  if (!session || !out || !capacity)
    return RUDP_ERR_INVALID_ARG;
  int count = 0;
  for (size_t i = 0; i < RUDP_MAX_CHANNELS && (size_t)count < capacity; ++i)
    count +=
        rudp_rx_drain(&session->channels[i], out + count, capacity - (size_t)count);
  return count;
}
