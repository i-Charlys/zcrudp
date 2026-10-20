#ifndef RUDP_RX_INTERNAL_H
#define RUDP_RX_INTERNAL_H
#include "rudp_types.h"
/* Internal: input is fully validated by the session before any RX mutation. */
int rudp_rx_drain(rudp_channel_s *chan, rudp_record_s *out, size_t capacity);
int rudp_rx_accept(rudp_channel_s *channel, const rudp_record_s *record,
                   rudp_record_s *out, size_t capacity);
#endif
