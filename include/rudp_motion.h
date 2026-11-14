#ifndef RUDP_MOTION_H
#define RUDP_MOTION_H

#include "rudp_scalar.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Kinematic regime classification for receiver-side dead reckoning.
 */
typedef enum {
  RUDP_DEAD_RECKON_HOLD = 0,     /**< Stationary or cold start: hold last value */
  RUDP_DEAD_RECKON_LINEAR = 1,   /**< Steady motion: 1st-order velocity extrapolation */
  RUDP_DEAD_RECKON_MANEUVER = 2  /**< High accel/jerk turn: bounded 2nd-order curve */
} rudp_dead_reckon_e;

typedef struct {
  rudp_scalar_tx_s scalar;
  int32_t velocity, acceleration, jerk;
  uint8_t samples;
} rudp_motion_tx_s;

typedef struct {
  rudp_scalar_rx_s scalar;
  int32_t velocity, acceleration, jerk;
  uint16_t last_value;
  uint8_t samples;
  uint8_t regime; /**< Active rudp_dead_reckon_e classification */
} rudp_motion_rx_s;

/** At a fixed caller-defined sampling period, clone the same rolling8 datagram
 * when |acceleration| or |jerk| crosses its threshold upward. Zero disables a
 * threshold. Returns datagram count 1 or 2, negative error. Output needs 16B.
 * The caller emits each 8B datagram; duplicates share a sequence and deduplicate.
 * This detects scalar changes, not physical units or cross-axis motion. */
RUDP_EXPORT int rudp_motion_encode(rudp_motion_tx_s *tx, uint16_t value,
                                    uint32_t acceleration_threshold,
                                    uint32_t jerk_threshold, uint8_t *out,
                                    size_t capacity);

/**
 * @brief Decodes a rolling8 motion datagram, updates receiver kinematic derivatives
 *        scaled by sequence delta, and classifies the active dead-reckoning regime.
 *
 * @return Number of fresh scalar samples written to @p out (0..2), or negative error.
 */
RUDP_EXPORT int rudp_motion_observe(rudp_motion_rx_s *rx, const uint8_t *in,
                                    size_t len, uint32_t acceleration_threshold,
                                    uint32_t jerk_threshold,
                                    rudp_scalar_sample_s *out, size_t capacity);

/**
 * @brief Extrapolates the 16-bit modular scalar @p steps_ahead ticks across a loss gap
 *        (clamped to @p max_steps) according to `rx->regime`.
 */
RUDP_EXPORT uint16_t rudp_motion_predict(const rudp_motion_rx_s *rx,
                                         uint8_t steps_ahead, uint8_t max_steps);

#ifdef __cplusplus
}
#endif

#endif /* RUDP_MOTION_H */
