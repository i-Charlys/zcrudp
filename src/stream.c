#include "rudp_stream.h"
#include <string.h>

static uint16_t get16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

int rudp_stream_start(rudp_stream_tx_s *tx, const void *data, size_t length) {
  if (!tx || (!data && length) || length > UINT16_MAX)
    return RUDP_ERR_INVALID_ARG;
  if (tx->active)
    return RUDP_ERR_BUFFER_FULL;
  *tx = (rudp_stream_tx_s){
      .data = data,
      .length = (uint16_t)length,
      .offset = 0,
      .active = 1,
      .descriptor_sent = 0,
  };
  return RUDP_OK;
}

int rudp_stream_pump(rudp_stream_tx_s *tx, rudp_session_s *session, uint8_t channel,
                     uint32_t now) {
  if (!tx || !session || channel >= RUDP_MAX_CHANNELS)
    return RUDP_ERR_INVALID_ARG;
  const unsigned required = RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED;
  if ((session->channels[channel].flags & required) != required)
    return RUDP_ERR_INVALID_ARG;
  while (tx->active) {
    tfv_packet_u payload;
    uint32_t advance = 0;
    if (!tx->descriptor_sent) {
      payload = (tfv_packet_u){
          .type = 255,
          .flags = 211,
          .value = tx->length,
      };
    } else {
      uint8_t bytes[4] = {0};
      advance = tx->length - tx->offset;
      if (advance > 4)
        advance = 4;
      memcpy(bytes, tx->data + tx->offset, advance);
      payload = (tfv_packet_u){
          .type = bytes[0],
          .flags = bytes[1],
          .value = get16(bytes + 2),
      };
    }
    int result = rudp_session_send_reliable(session, channel, payload, now);
    if (result == RUDP_ERR_BUFFER_FULL)
      return 0;
    if (result != RUDP_OK)
      return result;
    tx->descriptor_sent = 1;
    tx->offset += advance;
    if (tx->offset == tx->length)
      tx->active = 0;
  }
  return 1;
}

int rudp_stream_receive(rudp_stream_rx_s *rx, tfv_packet_u payload) {
  if (!rx || (!rx->data && rx->capacity))
    return RUDP_ERR_INVALID_ARG;
  if (!rx->active) {
    if (payload.type != 255 || payload.flags != 211)
      return RUDP_ERR_INVALID_ARG;
    rx->length = payload.value;
    rx->offset = 0;
    rx->active = rx->length != 0;
    rx->discarding = rx->length > rx->capacity;
    if (rx->discarding)
      return RUDP_ERR_BUFFER_FULL;
    return rx->active ? 0 : 1;
  }
  uint8_t bytes[4] = {payload.type, payload.flags, 0, 0};
  put16(bytes + 2, payload.value);
  size_t count = rx->length - rx->offset;
  if (count > 4)
    count = 4;
  if (!rx->discarding)
    memcpy(rx->data + rx->offset, bytes, count);
  rx->offset += (uint32_t)count;
  if (rx->offset != rx->length)
    return 0;
  rx->active = 0;
  return rx->discarding ? RUDP_ERR_BUFFER_FULL : 1;
}

int rudp_stream_tx_reset(rudp_stream_tx_s *tx) {
  if (!tx)
    return RUDP_ERR_INVALID_ARG;
  *tx = (rudp_stream_tx_s){.data = NULL};
  return RUDP_OK;
}

int rudp_stream_rx_reset(rudp_stream_rx_s *rx) {
  if (!rx)
    return RUDP_ERR_INVALID_ARG;
  rx->offset = 0;
  rx->length = 0;
  rx->active = 0;
  rx->discarding = 0;
  return RUDP_OK;
}
