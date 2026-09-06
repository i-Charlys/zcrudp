#include "protocol_profiles.h"
#include <string.h>

static uint16_t get16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

int rudp_stream_start(rudp_stream_tx_s *tx, const void *data, size_t length) {
  if (!tx || (!data && length) || length > UINT16_MAX) return RUDP_ERR_INVALID_ARG;
  if (tx->active) return RUDP_ERR_BUFFER_FULL;
  tx->data = data; tx->length = (uint16_t)length; tx->offset = 0;
  tx->descriptor_sent = 0; tx->active = 1;
  return RUDP_OK;
}

int rudp_stream_pump(rudp_stream_tx_s *tx, rudp_session_s *session,
                     uint8_t channel, uint32_t now) {
  if (!tx || !session || channel >= RUDP_MAX_CHANNELS) return RUDP_ERR_INVALID_ARG;
  const unsigned required = RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED;
  if ((session->channels[channel].flags & required) != required) return RUDP_ERR_INVALID_ARG;
  while (tx->active) {
    tfv_packet_u payload;
    uint32_t advance = 0;
    if (!tx->descriptor_sent) {
      payload.type = 255; payload.flags = 211; payload.value = tx->length;
    } else {
      uint8_t bytes[4] = {0};
      advance = tx->length - tx->offset;
      if (advance > 4) advance = 4;
      memcpy(bytes, tx->data + tx->offset, advance);
      payload.type = bytes[0]; payload.flags = bytes[1]; payload.value = get16(bytes + 2);
    }
    int result = rudp_session_send_reliable(session, channel, payload, now);
    if (result == RUDP_ERR_BUFFER_FULL) return 0;
    if (result != RUDP_OK) return result;
    tx->descriptor_sent = 1; tx->offset += advance;
    if (tx->offset == tx->length) tx->active = 0;
  }
  return 1;
}

int rudp_stream_receive(rudp_stream_rx_s *rx, tfv_packet_u payload) {
  if (!rx || (!rx->data && rx->capacity)) return RUDP_ERR_INVALID_ARG;
  if (!rx->active) {
    if (payload.type != 255 || payload.flags != 211) return RUDP_ERR_INVALID_ARG;
    rx->length = payload.value; rx->offset = 0;
    rx->active = rx->length != 0;
    rx->discarding = rx->length > rx->capacity;
    if (rx->discarding) return RUDP_ERR_BUFFER_FULL;
    return rx->active ? 0 : 1;
  }
  uint8_t bytes[4] = {payload.type, payload.flags, 0, 0};
  put16(bytes + 2, payload.value);
  size_t count = rx->length - rx->offset;
  if (count > 4) count = 4;
  if (!rx->discarding) memcpy(rx->data + rx->offset, bytes, count);
  rx->offset += (uint32_t)count;
  if (rx->offset != rx->length) return 0;
  rx->active = 0;
  return rx->discarding ? RUDP_ERR_BUFFER_FULL : 1;
}

int rudp_scalar_encode(rudp_scalar_tx_s *tx, uint16_t value, bool rolling,
                       uint8_t *out, size_t capacity) {
  size_t length = rolling ? 8 : 4;
  if (!tx || !out) return RUDP_ERR_INVALID_ARG;
  if (capacity < length) return RUDP_ERR_BUFFER_FULL;
  put16(out, tx->next_seq); put16(out + 2, value);
  if (rolling) {
    put16(out + 4, tx->has_previous ? (uint16_t)(value - tx->previous) : 0);
    put16(out + 6, tx->has_previous ? 1 : 0);
  }
  tx->next_seq++; tx->previous = value; tx->has_previous = 1;
  return (int)length;
}

int rudp_scalar_decode(rudp_scalar_rx_s *rx, const uint8_t *in, size_t length,
                       bool rolling, rudp_scalar_sample_s *out, size_t capacity) {
  if (!rx || !in || !out || length != (rolling ? 8U : 4U)) return RUDP_ERR_INVALID_ARG;
  uint16_t seq = get16(in), value = get16(in + 2);
  if (rolling && (get16(in + 6) > 1 || (!get16(in + 6) && get16(in + 4))))
    return RUDP_ERR_INVALID_ARG;
  uint16_t distance = (uint16_t)(seq - rx->last_seq);
  if (rx->initialized && (!distance || distance >= 32768U)) return 0;
  bool previous = rolling && get16(in + 6) && (!rx->initialized || distance > 1);
  size_t count = previous ? 2 : 1;
  if (capacity < count) return RUDP_ERR_BUFFER_FULL;
  if (previous) {
    out[0].seq = (uint16_t)(seq - 1);
    out[0].value = (uint16_t)(value - get16(in + 4));
  }
  out[count - 1].seq = seq; out[count - 1].value = value;
  rx->last_seq = seq; rx->initialized = 1;
  return (int)count;
}

static uint32_t magnitude(int32_t value) {
  return (uint32_t)(value < 0 ? -value : value);
}

int rudp_motion_encode(rudp_motion_tx_s *tx, uint16_t value,
                       uint32_t acceleration_threshold, uint32_t jerk_threshold,
                       uint8_t *out, size_t capacity) {
  if (!tx || !out) return RUDP_ERR_INVALID_ARG;
  if (capacity < 16) return RUDP_ERR_BUFFER_FULL;
  uint16_t delta = (uint16_t)(value - tx->scalar.previous);
  int32_t velocity = delta < 32768U ? (int32_t)delta : (int32_t)delta - 65536;
  int32_t acceleration = velocity - tx->velocity;
  int32_t jerk = acceleration - tx->acceleration;
  bool clone = tx->samples >= 2 &&
    ((acceleration_threshold && magnitude(acceleration) >= acceleration_threshold &&
      magnitude(tx->acceleration) < acceleration_threshold) ||
     (jerk_threshold && magnitude(jerk) >= jerk_threshold &&
      magnitude(tx->jerk) < jerk_threshold));
  (void)rudp_scalar_encode(&tx->scalar, value, true, out, capacity);
  if (clone) memcpy(out + 8, out, 8);
  tx->velocity = tx->samples ? velocity : 0;
  tx->acceleration = tx->samples >= 2 ? acceleration : 0;
  tx->jerk = tx->samples >= 2 ? jerk : 0;
  if (tx->samples < 3) tx->samples++;
  return clone ? 2 : 1;
}
