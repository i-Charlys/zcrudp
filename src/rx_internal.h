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

#define RUDP_UNREL_HAS_SEQ_MASK 0x01U
#define RUDP_RX_HIST_SHIFT 2U
#define RUDP_RX_HIST_MAX 63U

static inline void rudp_stat_inc(uint32_t *counter) {
  if (counter && *counter < UINT32_MAX) {
    (*counter)++;
  }
}

static inline tfv_packet_u rudp_mask_to_payload(uint32_t mask) {
  tfv_packet_u p;
  p.type = (uint8_t)((mask >> 24) & 0xFFU);
  p.flags = (uint8_t)((mask >> 16) & 0xFFU);
  p.value = (uint16_t)(mask & 0xFFFFU);
  return p;
}

static inline uint32_t rudp_payload_to_mask(tfv_packet_u p) {
  return ((uint32_t)p.type << 24) | ((uint32_t)p.flags << 16) | (uint32_t)p.value;
}

static inline tfv_packet_u rudp_payload_xor(tfv_packet_u a, tfv_packet_u b) {
  tfv_packet_u r;
  r.type = (uint8_t)(a.type ^ b.type);
  r.flags = (uint8_t)(a.flags ^ b.flags);
  r.value = (uint16_t)(a.value ^ b.value);
  return r;
}

/* Internal: input is fully validated by the session before any RX mutation. */
uint32_t rudp_rx_sack_mask_offset(const rudp_channel_s *chan, uint16_t base_offset);
uint32_t rudp_rx_sack_mask(const rudp_channel_s *chan);
int rudp_rx_drain(rudp_channel_s *chan, rudp_record_s *out, size_t capacity);
int rudp_rx_accept(rudp_channel_s *channel, const rudp_record_s *record,
                   rudp_record_s *out, size_t capacity, bool *out_out_of_window);
int rudp_rx_accept_fec(rudp_channel_s *chan, const rudp_record_s *parity_rec,
                       rudp_record_s *out, size_t capacity, bool *out_recovered);

#endif
