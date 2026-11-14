#include "rudp_scalar.h"
#include <string.h>

static uint16_t get16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

int rudp_scalar_encode(rudp_scalar_tx_s *tx, uint16_t value, bool rolling, uint8_t *out,
                       size_t capacity) {
  size_t length = rolling ? 8 : 4;
  if (!tx || !out)
    return RUDP_ERR_INVALID_ARG;
  if (capacity < length)
    return RUDP_ERR_BUFFER_FULL;
  put16(out, tx->next_seq);
  put16(out + 2, value);
  if (rolling) {
    put16(out + 4, tx->has_previous ? (uint16_t)(value - tx->previous) : 0);
    put16(out + 6, tx->has_previous ? 1 : 0);
  }
  tx->next_seq++;
  tx->previous = value;
  tx->has_previous = 1;
  return (int)length;
}

int rudp_scalar_decode(rudp_scalar_rx_s *rx, const uint8_t *in, size_t length,
                       bool rolling, rudp_scalar_sample_s *out, size_t capacity) {
  if (!rx || !in || !out || length != (rolling ? 8U : 4U))
    return RUDP_ERR_INVALID_ARG;
  uint16_t seq = get16(in), value = get16(in + 2);
  if (rolling && (get16(in + 6) > 1 || (!get16(in + 6) && get16(in + 4))))
    return RUDP_ERR_INVALID_ARG;
  uint16_t distance = (uint16_t)(seq - rx->last_seq);
  if (rx->initialized && (!distance || distance >= 32768U))
    return 0;
  bool previous = rolling && get16(in + 6) && (!rx->initialized || distance > 1);
  size_t count = previous ? 2 : 1;
  if (capacity < count)
    return RUDP_ERR_BUFFER_FULL;
  if (previous) {
    out[0] = (rudp_scalar_sample_s){
        .seq = (uint16_t)(seq - 1),
        .value = (uint16_t)(value - get16(in + 4)),
    };
  }
  out[count - 1] = (rudp_scalar_sample_s){
      .seq = seq,
      .value = value,
  };
  rx->last_seq = seq;
  rx->initialized = 1;
  return (int)count;
}
