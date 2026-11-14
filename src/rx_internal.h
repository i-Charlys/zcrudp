#ifndef RUDP_RX_INTERNAL_H
#define RUDP_RX_INTERNAL_H
#include "rudp_types.h"
#define RUDP_CHAN_BACKOFF_MASK 0x0FU
#define RUDP_CHAN_FLAG_GAP_ACK_PENDING 0x10U
#define RUDP_CHAN_MTU_HI_SHIFT 5U
#define RUDP_CHAN_MTU_HI_MASK 0xE0U
#define RUDP_CHAN_STREAK_MASK 0x07U
#define RUDP_CHAN_MTU_LO_SHIFT 3U
#define RUDP_CHAN_MTU_LO_MASK 0xF8U
#define RUDP_UNREL_RESYNC_STREAK 4U
/* Internal: input is fully validated by the session before any RX mutation. */
int rudp_rx_drain(rudp_channel_s *chan, rudp_record_s *out, size_t capacity);
int rudp_rx_accept(rudp_channel_s *channel, const rudp_record_s *record,
                   rudp_record_s *out, size_t capacity);
#endif
