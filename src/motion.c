#include "rudp_motion.h"
#include <string.h>

static uint32_t magnitude(int32_t value) {
  return (value < 0) ? (0U - (uint32_t)value) : (uint32_t)value;
}

static int32_t signed_mod16_delta(uint16_t cur, uint16_t prev) {
  uint16_t d = (uint16_t)(cur - prev);
  return (d < 32768U) ? (int32_t)d : ((int32_t)d - 65536);
}

int rudp_motion_encode(rudp_motion_tx_s *tx, uint16_t value,
                       uint32_t acceleration_threshold, uint32_t jerk_threshold,
                       uint8_t *out, size_t capacity) {
  if (!tx || !out)
    return RUDP_ERR_INVALID_ARG;
  if (capacity < 16)
    return RUDP_ERR_BUFFER_FULL;
  int32_t velocity =
      (tx->samples >= 1) ? signed_mod16_delta(value, tx->scalar.previous) : 0;
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

static void motion_rx_step(rudp_motion_rx_s *rx, uint16_t value, uint16_t seq_step,
                           uint32_t accel_threshold, uint32_t jerk_threshold) {
  if (rx->samples == 0U || seq_step == 0U) {
    rx->last_value = value;
    rx->velocity = 0;
    rx->acceleration = 0;
    rx->jerk = 0;
    rx->samples = 1U;
    rx->regime = (uint8_t)RUDP_DEAD_RECKON_HOLD;
    return;
  }

  int32_t total_delta = signed_mod16_delta(value, rx->last_value);
  int32_t v = total_delta / (int32_t)seq_step;
  int32_t a = (rx->samples >= 2U) ? ((v - rx->velocity) / (int32_t)seq_step) : 0;
  int32_t j = (rx->samples >= 3U) ? ((a - rx->acceleration) / (int32_t)seq_step) : 0;

  rx->last_value = value;
  rx->velocity = v;
  rx->acceleration = a;
  rx->jerk = j;
  if (rx->samples < 3U) {
    rx->samples++;
  }

  if (v == 0 && a == 0) {
    rx->regime = (uint8_t)RUDP_DEAD_RECKON_HOLD;
  } else if ((rx->samples >= 2U && accel_threshold > 0U &&
              magnitude(a) >= accel_threshold) ||
             (rx->samples >= 3U && jerk_threshold > 0U &&
              magnitude(j) >= jerk_threshold)) {
    rx->regime = (uint8_t)RUDP_DEAD_RECKON_MANEUVER;
  } else {
    rx->regime = (uint8_t)RUDP_DEAD_RECKON_LINEAR;
  }
}

int rudp_motion_observe(rudp_motion_rx_s *rx, const uint8_t *in, size_t len,
                        uint32_t acceleration_threshold, uint32_t jerk_threshold,
                        rudp_scalar_sample_s *out, size_t capacity) {
  if (!rx || !in || !out) {
    return RUDP_ERR_INVALID_ARG;
  }
  uint8_t had_prev = rx->scalar.initialized;
  uint16_t prev_seq = rx->scalar.last_seq;
  int n = rudp_scalar_decode(&rx->scalar, in, len, true, out, capacity);
  if (n <= 0) {
    return n;
  }

  for (int i = 0; i < n; ++i) {
    uint16_t step = had_prev ? (uint16_t)(out[i].seq - prev_seq) : 1U;
    motion_rx_step(rx, out[i].value, step ? step : 1U, acceleration_threshold,
                   jerk_threshold);
    prev_seq = out[i].seq;
    had_prev = 1U;
  }
  return n;
}

uint16_t rudp_motion_predict(const rudp_motion_rx_s *rx, uint8_t steps_ahead,
                             uint8_t max_steps) {
  if (!rx || rx->samples == 0U) {
    return 0U;
  }
  uint8_t s = (steps_ahead > max_steps) ? max_steps : steps_ahead;
  if (s == 0U || rx->regime == (uint8_t)RUDP_DEAD_RECKON_HOLD) {
    return rx->last_value;
  }
  int64_t pred = (int64_t)rx->last_value + (int64_t)rx->velocity * (int64_t)s;
  if (rx->regime == (uint8_t)RUDP_DEAD_RECKON_MANEUVER) {
    pred += ((int64_t)rx->acceleration * (int64_t)s * (int64_t)s) / 2;
  }
  return (uint16_t)((uint64_t)pred & 0xFFFFULL);
}
