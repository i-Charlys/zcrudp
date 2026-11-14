#include "rx_internal.h"

static void rx_hist_advance(rudp_channel_s *chan) {
  uint8_t hist = (uint8_t)(chan->has_unreliable_seq >> RUDP_RX_HIST_SHIFT);
  if (hist < RUDP_RX_HIST_MAX) {
    hist++;
    chan->has_unreliable_seq =
        (uint8_t)((chan->has_unreliable_seq & RUDP_UNREL_HAS_SEQ_MASK) |
                  (hist << RUDP_RX_HIST_SHIFT));
  }
}

uint32_t rudp_rx_sack_mask(const rudp_channel_s *chan) {
#if RUDP_EXTERNAL_RX_STORAGE
  if (!chan->rx_present) {
    return 0U;
  }
#endif
  uint32_t mask = 0U;
  unsigned limit = (RUDP_WINDOW_SIZE - 1U < 32U) ? (RUDP_WINDOW_SIZE - 1U) : 32U;
  for (unsigned b = 0U; b < limit; ++b) {
    uint16_t seq = (uint16_t)(chan->ctx.expected_seq_num + 1U + b);
    uint16_t slot = seq & (RUDP_WINDOW_SIZE - 1U);
    if (chan->rx_present[slot / 8U] & (uint8_t)(1U << (slot & 7U))) {
      mask |= (UINT32_C(1) << b);
    }
  }
  return mask;
}

int rudp_rx_drain(rudp_channel_s *chan, rudp_record_s *out, size_t capacity) {
#if RUDP_EXTERNAL_RX_STORAGE
  if (!chan->rx_present) {
    return 0;
  }
#endif
  if (!(chan->flags & RUDP_CHANNEL_FLAG_ORDERED)) {
    return 0;
  }
  int count = 0;
  while ((size_t)count < capacity) {
    uint16_t slot = chan->ctx.expected_seq_num & (RUDP_WINDOW_SIZE - 1);
    uint8_t mask = (uint8_t)(1U << (slot & 7));
    if (!(chan->rx_present[slot / 8] & mask)) {
      break;
    }
    out[count] = (rudp_record_s){
        .channel_id = chan->channel_id,
        .flags = RUDP_RECORD_FLAG_RELIABLE,
        .seq_num = chan->ctx.expected_seq_num,
        .payload = chan->rx_buffer[slot],
    };
    chan->ctx.expected_seq_num++;
    count++;
    chan->rx_present[slot / 8] &= (uint8_t)~mask;
    chan->ack_pending = 1;
    rx_hist_advance(chan);
  }
  return count;
}

int rudp_rx_accept(rudp_channel_s *channel, const rudp_record_s *record,
                   rudp_record_s *out, size_t capacity, bool *out_out_of_window) {
  if (out_out_of_window) {
    *out_out_of_window = false;
  }
  uint16_t distance = (uint16_t)(record->seq_num - channel->ctx.expected_seq_num);
  if (distance < RUDP_WINDOW_SIZE) {
    uint16_t slot = record->seq_num & (RUDP_WINDOW_SIZE - 1);
    uint8_t mask = (uint8_t)(1U << (slot & 7));

    if (!(channel->flags & RUDP_CHANNEL_FLAG_ORDERED)) {
      /* Zero-HoL Reliable Unordered: deliver fresh in-window packet immediately,
       * deduplicate via rx_present, and slide cumulative expected_seq_num across
       * contiguous delivered slots. */
      if (channel->rx_present[slot / 8] & mask) {
        channel->ack_pending = 1;
        channel->chan_backoff |= RUDP_CHAN_FLAG_GAP_ACK_PENDING;
        return 0;
      }
      if (capacity == 0U) {
        return 0;
      }
      channel->rx_buffer[slot] = record->payload;
      channel->rx_present[slot / 8] |= mask;
      out[0] = (rudp_record_s){
          .channel_id = channel->channel_id,
          .flags = RUDP_RECORD_FLAG_RELIABLE,
          .seq_num = record->seq_num,
          .payload = record->payload,
      };
      channel->ack_pending = 1;
      if (distance > 0U) {
        channel->chan_backoff |= RUDP_CHAN_FLAG_GAP_ACK_PENDING;
      }
      while (1) {
        uint16_t eslot = channel->ctx.expected_seq_num & (RUDP_WINDOW_SIZE - 1);
        uint8_t emask = (uint8_t)(1U << (eslot & 7));
        if (!(channel->rx_present[eslot / 8] & emask)) {
          break;
        }
        channel->rx_present[eslot / 8] &= (uint8_t)~emask;
        channel->ctx.expected_seq_num++;
        rx_hist_advance(channel);
      }
      return 1;
    }

    if (channel->rx_present[slot / 8] & mask) {
      /* Duplicate ahead-of-order segment already buffered: trigger duplicate ACK. */
      channel->ack_pending = 1;
      channel->chan_backoff |= RUDP_CHAN_FLAG_GAP_ACK_PENDING;
    } else {
      channel->rx_buffer[slot] = record->payload;
      channel->rx_present[slot / 8] |= mask;
    }
    if (distance) {
      channel->ack_pending = 1;
      channel->chan_backoff |= RUDP_CHAN_FLAG_GAP_ACK_PENDING;
    }
    return rudp_rx_drain(channel, out, capacity);
  }

  if (out_out_of_window) {
    *out_out_of_window = true;
  }
  if (distance > 32768U) {
    channel->ack_pending = 1;
    channel->chan_backoff |= RUDP_CHAN_FLAG_GAP_ACK_PENDING;
  }
  return 0;
}

static bool rx_lookup_known_payload(const rudp_channel_s *chan, uint16_t seq,
                                    tfv_packet_u *out_payload) {
  uint16_t dist = (uint16_t)(seq - chan->ctx.expected_seq_num);
  uint16_t slot = seq & (RUDP_WINDOW_SIZE - 1U);
  uint8_t mask = (uint8_t)(1U << (slot & 7U));
  if (dist < RUDP_WINDOW_SIZE) {
    if (chan->rx_present[slot / 8U] & mask) {
      *out_payload = chan->rx_buffer[slot];
      return true;
    }
    return false;
  }

  uint16_t behind = (uint16_t)(chan->ctx.expected_seq_num - seq);
  uint8_t hist = (uint8_t)(chan->has_unreliable_seq >> RUDP_RX_HIST_SHIFT);
  if (behind >= 1U && behind <= hist && behind < RUDP_WINDOW_SIZE &&
      !(chan->rx_present[slot / 8U] & mask)) {
    *out_payload = chan->rx_buffer[slot];
    return true;
  }
  return false;
}

static bool rx_is_missing_in_window(const rudp_channel_s *chan, uint16_t seq) {
  uint16_t dist = (uint16_t)(seq - chan->ctx.expected_seq_num);
  if (dist >= RUDP_WINDOW_SIZE) {
    return false;
  }
  uint16_t slot = seq & (RUDP_WINDOW_SIZE - 1U);
  uint8_t mask = (uint8_t)(1U << (slot & 7U));
  return (chan->rx_present[slot / 8U] & mask) == 0U;
}

int rudp_rx_accept_fec(rudp_channel_s *chan, const rudp_record_s *parity_rec,
                       rudp_record_s *out, size_t capacity, bool *out_recovered) {
  if (out_recovered) {
    *out_recovered = false;
  }
#if RUDP_EXTERNAL_RX_STORAGE
  if (!chan->rx_present) {
    return 0;
  }
#endif
  uint16_t even_seq = parity_rec->seq_num;
  uint16_t odd_seq = (uint16_t)(even_seq + 1U);
  tfv_packet_u known = {.raw = 0};
  uint16_t missing_seq = 0U;

  bool have_even = rx_lookup_known_payload(chan, even_seq, &known);
  if (have_even && rx_is_missing_in_window(chan, odd_seq)) {
    missing_seq = odd_seq;
  } else {
    bool have_odd = rx_lookup_known_payload(chan, odd_seq, &known);
    if (have_odd && rx_is_missing_in_window(chan, even_seq)) {
      missing_seq = even_seq;
    } else {
      return 0;
    }
  }

  rudp_record_s recovered = {
      .channel_id = chan->channel_id,
      .flags = RUDP_RECORD_FLAG_RELIABLE,
      .seq_num = missing_seq,
      .payload = rudp_payload_xor(parity_rec->payload, known),
  };
  if (out_recovered) {
    *out_recovered = true;
  }
  return rudp_rx_accept(chan, &recovered, out, capacity, NULL);
}
