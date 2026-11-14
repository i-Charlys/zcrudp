#ifndef RUDP_STREAM_H
#define RUDP_STREAM_H

#include "rudp_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Optional, explicitly bound profiles. Never infer a profile from UDP length.
 * All state and backing storage belong to the caller. Initialize with {.data = NULL}. */
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
RUDP_EXPORT int rudp_stream_start(rudp_stream_tx_s *tx, const void *data,
                                   size_t length);
RUDP_EXPORT int rudp_stream_pump(rudp_stream_tx_s *tx, rudp_session_s *session,
                                  uint8_t channel, uint32_t now);
/** Feed only ordered delivered payloads. Returns 1 on completed message, 0 while
 * assembling, negative error. Oversized messages are drained without writing.
 * Read data/length on return 1 before feeding the next descriptor. */
RUDP_EXPORT int rudp_stream_receive(rudp_stream_rx_s *rx, tfv_packet_u payload);
RUDP_EXPORT int rudp_stream_tx_reset(rudp_stream_tx_s *tx);
RUDP_EXPORT int rudp_stream_rx_reset(rudp_stream_rx_s *rx);

#ifdef __cplusplus
}
#endif

#endif /* RUDP_STREAM_H */
