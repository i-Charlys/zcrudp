#ifndef RUDP_SCALAR_H
#define RUDP_SCALAR_H

#include "rudp_config.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint16_t next_seq, previous;
  uint8_t has_previous;
} rudp_scalar_tx_s;
typedef struct {
  uint16_t last_seq;
  uint8_t initialized;
} rudp_scalar_rx_s;
typedef struct {
  uint16_t seq, value;
} rudp_scalar_sample_s;

/** Compact4 = seq16/value16; rolling8 = seq16/value16/delta16/previous_valid16.
 * Integers are network byte order. Rolling delta is unsigned modulo 65536.
 * Encode returns bytes; decode returns sample count (0 stale), negative error.
 * Failed capacity checks never consume sequence numbers or receiver state.
 * Serial ordering assumes fewer than 32768 outstanding sample intervals. */
int rudp_scalar_encode(rudp_scalar_tx_s *tx, uint16_t value, bool rolling, uint8_t *out,
                       size_t capacity);
int rudp_scalar_decode(rudp_scalar_rx_s *rx, const uint8_t *in, size_t length,
                       bool rolling, rudp_scalar_sample_s *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* RUDP_SCALAR_H */
