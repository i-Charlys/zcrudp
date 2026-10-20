#ifndef RUDP_MOTION_H
#define RUDP_MOTION_H

#include "rudp_scalar.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  rudp_scalar_tx_s scalar;
  int32_t velocity, acceleration, jerk;
  uint8_t samples;
} rudp_motion_tx_s;

/** At a fixed caller-defined sampling period, clone the same rolling8 datagram
 * when |acceleration| or |jerk| crosses its threshold upward. Zero disables a
 * threshold. Returns datagram count 1 or 2, negative error. Output needs 16B.
 * The caller emits each 8B datagram; duplicates share a sequence and deduplicate.
 * This detects scalar changes, not physical units or cross-axis motion. */
int rudp_motion_encode(rudp_motion_tx_s *tx, uint16_t value,
                       uint32_t acceleration_threshold, uint32_t jerk_threshold,
                       uint8_t *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* RUDP_MOTION_H */
