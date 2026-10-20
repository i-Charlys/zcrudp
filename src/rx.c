#include "rx_internal.h"

int rudp_rx_drain(rudp_channel_s *chan, rudp_record_s *out, size_t capacity) {
#if RUDP_EXTERNAL_RX_STORAGE
  if (!chan->rx_present)
    return 0;
#endif
  int count = 0;
  while ((size_t)count < capacity) {
    uint16_t slot = chan->ctx.expected_seq_num & (RUDP_WINDOW_SIZE - 1);
    uint8_t mask = (uint8_t)(1U << (slot & 7));
    if (!(chan->rx_present[slot / 8] & mask))
      break;
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
  }
  return count;
}

int rudp_rx_accept(rudp_channel_s *channel, const rudp_record_s *record,
                   rudp_record_s *out, size_t capacity) {
  /* Bounded RX window: retain gaps without acknowledging undelivered data. */
  uint16_t distance = (uint16_t)(record->seq_num - channel->ctx.expected_seq_num);
  if (distance < RUDP_WINDOW_SIZE) {
    uint16_t slot = record->seq_num & (RUDP_WINDOW_SIZE - 1);
    uint8_t mask = (uint8_t)(1U << (slot & 7));
    if (!(channel->rx_present[slot / 8] & mask)) {
      channel->rx_buffer[slot] = record->payload;
      channel->rx_present[slot / 8] |= mask;
    }
    if (distance)
      channel->ack_pending = 1;
    return rudp_rx_drain(channel, out, capacity);
  } else if (distance > 32768U) {
    channel->ack_pending = 1;
  }
  return 0;
}
