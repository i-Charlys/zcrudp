#include "rudp_session.h"
#include "rudp_reliability.h"
#include "rudp_codec.h"
#include <string.h>

int rudp_session_send_reliable(rudp_session_s *session, uint8_t channel_id,
                               tfv_packet_u payload, uint32_t now) {
  if (!session || channel_id >= RUDP_MAX_CHANNELS) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (!(session->channels[channel_id].flags & RUDP_CHANNEL_FLAG_RELIABLE)) {
    return RUDP_ERR_INVALID_ARG;
  }

  return rudp_send(&session->channels[channel_id].ctx, payload, now);
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

  rudp_datagram_header_s d_header = {
      .ack = ack_chan->ctx.expected_seq_num, .ack_channel = ack_channel, .count = 1};

  int ret = rudp_pack_datagram_header(&d_header, out_buf, max_len);
  if (ret != RUDP_WIRE_DATAGRAM_HEADER_SIZE) {
    return ret;
  }

  ack_chan->last_ack_sent = d_header.ack;
  ack_chan->ack_pending = 0;

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

static uint32_t backed_off_timeout(uint32_t base, unsigned shift) {
  if (shift > 6)
    shift = 6;
  return base > (UINT32_MAX >> shift) ? UINT32_MAX : base << shift;
}

int rudp_session_build_datagram(rudp_session_s *session, uint8_t primary_ack_channel,
                                uint8_t *out_buf, size_t max_len, uint32_t now,
                                uint32_t timeout) {
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
  size_t cap_records =
      (effective_max - RUDP_WIRE_DATAGRAM_HEADER_SIZE) / RUDP_WIRE_RECORD_SIZE;
  if (cap_records > 255) {
    cap_records = 255;
  }

  uint8_t count = 0;

  /* 1. Bundling priority: Multi-channel explicit ACK records for any other channel with
   * ack_pending */
  for (uint8_t i = 0; i < RUDP_MAX_CHANNELS && count < cap_records; i++) {
    uint8_t c = (uint8_t)((session->rr_cursor + i) & (RUDP_MAX_CHANNELS - 1));
    if (c != primary_ack_channel && session->channels[c].ack_pending) {
      rudp_channel_s *chan = &session->channels[c];
      rudp_record_s rec = {.channel_id = c,
                           .flags = RUDP_RECORD_FLAG_ACK,
                           .seq_num = chan->ctx.expected_seq_num,
                           .payload = {.raw = 0}};

      size_t offset =
          RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)count * RUDP_WIRE_RECORD_SIZE);
      int ret = rudp_pack_record(&rec, out_buf + offset, effective_max - offset);
      if (ret != RUDP_WIRE_RECORD_SIZE) {
        return ret;
      }

      chan->last_ack_sent = rec.seq_num;
      chan->ack_pending = 0;
      count++;
    }
  }

  /* Stable priority order, rotating ties. ACKs remain ahead of all data. */
  uint8_t order[RUDP_MAX_CHANNELS];
  for (size_t i = 0; i < RUDP_MAX_CHANNELS; ++i) {
    uint8_t c = (uint8_t)((session->rr_cursor + i) & (RUDP_MAX_CHANNELS - 1));
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
      if (slot->state == RUDP_SLOT_IN_FLIGHT) {
        bool send_now = false;
        if (slot->tx_count == 0) {
          send_now = true;
        } else if (slot->fast_retransmit == RUDP_FAST_RETRANSMIT_PENDING) {
          send_now = true;
        } else if (timeout == 0 && !chan->rto_min_ms) {
          send_now = true;
        } else {
          uint32_t base = chan->rto_min_ms ? chan->rto_ms : timeout;
          unsigned shift =
              chan->rto_min_ms ? chan->timeout_backoffs[curr] : slot->retries;
          uint32_t current_timeout = backed_off_timeout(base, shift);
          if ((uint32_t)(now - slot->timestamp) > current_timeout) {
            send_now = true;
          }
        }

        if (send_now) {
          rudp_record_s rec = {.channel_id = c,
                               .flags = RUDP_RECORD_FLAG_RELIABLE,
                               .seq_num = slot->frame.header.seq_num,
                               .payload = slot->frame.packet};

          size_t offset =
              RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)count * RUDP_WIRE_RECORD_SIZE);
          int ret = rudp_pack_record(&rec, out_buf + offset, effective_max - offset);
          if (ret != RUDP_WIRE_RECORD_SIZE) {
            return ret;
          }

          if (slot->tx_count == 0) {
            chan->timeout_backoffs[curr] = 0;
            slot->tx_count = 1;
            slot->timestamp = now;
            slot->fast_retransmit = RUDP_FAST_RETRANSMIT_OFF;
          } else {
            if (slot->fast_retransmit != RUDP_FAST_RETRANSMIT_PENDING &&
                chan->timeout_backoffs[curr] < 6)
              chan->timeout_backoffs[curr]++;
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
  session->rr_cursor = (uint8_t)((session->rr_cursor + 1) & (RUDP_MAX_CHANNELS - 1));

  /* 3. Pack the primary 4-byte datagram header at the beginning of out_buf */
  rudp_datagram_header_s d_header = {.ack = prim_chan->ctx.expected_seq_num,
                                     .ack_channel = primary_ack_channel,
                                     .count = count};

  int ret = rudp_pack_datagram_header(&d_header, out_buf, effective_max);
  if (ret != RUDP_WIRE_DATAGRAM_HEADER_SIZE) {
    return ret;
  }

  prim_chan->last_ack_sent = d_header.ack;
  prim_chan->ack_pending = 0;

  return (int)(RUDP_WIRE_DATAGRAM_HEADER_SIZE +
               ((size_t)count * RUDP_WIRE_RECORD_SIZE));
}
