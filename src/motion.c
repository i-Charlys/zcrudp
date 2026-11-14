#include "rudp_motion.h"
#include <string.h>

static uint32_t magnitude(int32_t value) {
  return (uint32_t)(value < 0 ? -value : value);
}

int rudp_motion_encode(rudp_motion_tx_s *tx, uint16_t value,
                       uint32_t acceleration_threshold, uint32_t jerk_threshold,
                       uint8_t *out, size_t capacity) {
  if (!tx || !out)
    return RUDP_ERR_INVALID_ARG;
  if (capacity < 16)
    return RUDP_ERR_BUFFER_FULL;
  uint16_t delta = (uint16_t)(value - tx->scalar.previous);
  int32_t velocity =
      (tx->samples >= 1) ? (delta < 32768U ? (int32_t)delta : (int32_t)delta - 65536) : 0;
  int32_t acceleration = (tx->samples >= 2) ? (velocity - tx->velocity) : 0;
  int32_t jerk = (tx->samples >= 3) ? (acceleration - tx->acceleration) : 0;
  bool clone =
      (tx->samples >= 2 && acceleration_threshold &&
       magnitude(acceleration) >= acceleration_threshold &&
       magnitude(tx->acceleration) < acceleration_threshold) ||
      (tx->samples >= 3 && jerk_threshold && magnitude(jerk) >= jerk_threshold &&
       magnitude(tx->jerk) < jerk_threshold);
  (void)rudp_scalar_encode(&tx->scalar, value, true, out, capacity);
  if (clone)
    memcpy(out + 8, out, 8);
  tx->velocity = velocity;
  tx->acceleration = acceleration;
  tx->jerk = jerk;
  if (tx->samples < 3)
    tx->samples++;
  return clone ? 2 : 1;
}
