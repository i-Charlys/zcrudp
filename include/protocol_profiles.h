#ifndef PROTOCOL_PROFILES_H
#define PROTOCOL_PROFILES_H

#include "protocol_rudp.h"

/** Optional, explicitly bound profiles. Never infer a profile from UDP length.
 * All state and backing storage belong to the caller. Initialize with {0}. */
typedef struct {
  const uint8_t *data;
  uint16_t length;
  uint32_t offset;
  uint8_t active, descriptor_sent;
} rudp_stream_tx_s;

typedef struct {
  uint8_t *data;
  size_t capacity;
  uint32_t offset;
  uint16_t length;
  uint8_t active, discarding;
} rudp_stream_rx_s;

/** One stream per dedicated reliable+ordered channel. Source stays valid until
 * pump returns 1 (all chunks copied to TX slots). No interleaved TFV messages.
 * start: 0 success; pump: 1 queued completely, 0 backpressure, negative error. */
int rudp_stream_start(rudp_stream_tx_s *tx, const void *data, size_t length);
int rudp_stream_pump(rudp_stream_tx_s *tx, rudp_session_s *session,
                     uint8_t channel, uint32_t now);
/** Feed only ordered delivered payloads. Returns 1 on completed message, 0 while
 * assembling, negative error. Oversized messages are drained without writing.
 * Read data/length on return 1 before feeding the next descriptor. */
int rudp_stream_receive(rudp_stream_rx_s *rx, tfv_packet_u payload);

typedef struct { uint16_t next_seq, previous; uint8_t has_previous; } rudp_scalar_tx_s;
typedef struct { uint16_t last_seq; uint8_t initialized; } rudp_scalar_rx_s;
typedef struct { uint16_t seq, value; } rudp_scalar_sample_s;

/** Compact4 = seq16/value16; rolling8 = seq16/value16/delta16/previous_valid16.
 * Integers are network byte order. Rolling delta is unsigned modulo 65536.
 * Encode returns bytes; decode returns sample count (0 stale), negative error.
 * Failed capacity checks never consume sequence numbers or receiver state.
 * Serial ordering assumes fewer than 32768 outstanding sample intervals. */
int rudp_scalar_encode(rudp_scalar_tx_s *tx, uint16_t value, bool rolling,
                       uint8_t *out, size_t capacity);
int rudp_scalar_decode(rudp_scalar_rx_s *rx, const uint8_t *in, size_t length,
                       bool rolling, rudp_scalar_sample_s *out, size_t capacity);

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

#endif
