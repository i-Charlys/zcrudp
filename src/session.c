#include "rudp_session.h"
#include "rx_internal.h"
#include "rudp_reliability.h"
#include "rudp_codec.h"
#include <string.h>

int rudp_session_init(rudp_session_s *session) {
  if (!session) {
    return RUDP_ERR_INVALID_ARG;
  }

  *session = (rudp_session_s){.active_channels = 0};
  for (uint8_t i = 0; i < RUDP_MAX_CHANNELS; i++) {
    session->channels[i].channel_id = i;
    session->channels[i].flags = RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED;
    session->channels[i].priority = 128;
    rudp_init(&session->channels[i].ctx);
  }

  return RUDP_OK;
}

int rudp_session_set_seed(rudp_session_s *session, uint16_t seed) {
  if (!session) {
    return RUDP_ERR_INVALID_ARG;
  }
  session->session_seed = seed;
  return RUDP_OK;
}

int rudp_session_config_channel(rudp_session_s *session, uint8_t channel_id,
                                uint8_t flags) {
  if (!session || channel_id >= RUDP_MAX_CHANNELS) {
    return RUDP_ERR_INVALID_ARG;
  }

  rudp_channel_s *chan = &session->channels[channel_id];
  if (chan->ctx.head != chan->ctx.tail) {
    return RUDP_ERR_INVALID_ARG;
  }
#if RUDP_EXTERNAL_RX_STORAGE
  if (chan->rx_present)
#endif
    for (size_t i = 0; i < RUDP_RX_BITMAP_SIZE; ++i) {
      if (chan->rx_present[i]) {
        return RUDP_ERR_INVALID_ARG;
      }
    }

  chan->flags = flags;
  if (channel_id >= session->active_channels) {
    session->active_channels = (uint8_t)(channel_id + 1);
  }

  return RUDP_OK;
}

int rudp_session_set_qos(rudp_session_s *session, uint8_t channel, uint8_t priority,
                         uint8_t dscp) {
  if (!session || channel >= RUDP_MAX_CHANNELS || dscp > 63)
    return RUDP_ERR_INVALID_ARG;
  session->channels[channel].priority = priority;
  session->channels[channel].dscp = dscp;
  return RUDP_OK;
}

int rudp_session_config_recovery(rudp_session_s *session, uint8_t channel,
                                 uint32_t initial_ms, uint32_t min_ms,
                                 uint32_t max_ms) {
  if (!session || channel >= RUDP_MAX_CHANNELS || !min_ms || min_ms > initial_ms ||
      initial_ms > max_ms || max_ms > 60000)
    return RUDP_ERR_INVALID_ARG;
  rudp_channel_s *chan = &session->channels[channel];
  if (chan->ctx.head != chan->ctx.tail)
    return RUDP_ERR_BUFFER_FULL;
#if RUDP_EXTERNAL_RX_STORAGE
  if (chan->rx_present)
#endif
    for (size_t i = 0; i < RUDP_RX_BITMAP_SIZE; ++i)
      if (chan->rx_present[i])
        return RUDP_ERR_BUFFER_FULL;
  chan->rto_min_ms = (uint16_t)min_ms;
  chan->rto_max_ms = (uint16_t)max_ms;
  chan->rto_ms = (uint16_t)initial_ms;
  chan->rto_initial_ms = (uint16_t)initial_ms;
  chan->srtt_scaled = chan->rttvar_scaled = chan->last_rtt_sample = 0;
  chan->chan_backoff &= (uint8_t)~RUDP_CHAN_BACKOFF_MASK;
  for (size_t i = 0; i < RUDP_WINDOW_SIZE; ++i) {
    rudp_slot_set_backoff(&chan->ctx.tx_buffer[i], 0U);
  }
  return RUDP_OK;
}

int rudp_session_reset_channel(rudp_session_s *session, uint8_t channel_id) {
  if (!session || channel_id >= RUDP_MAX_CHANNELS) {
    return RUDP_ERR_INVALID_ARG;
  }

  rudp_channel_s *chan = &session->channels[channel_id];
  chan->last_unreliable_seq = 0;
  chan->has_unreliable_seq = 0;
  chan->ack_pending = 0;
  chan->last_ack_sent = 0;
  chan->next_unreliable_seq = 0;
  chan->unrel_candidate_seq = 0;
  chan->unrel_ahead_streak &= (uint8_t)~RUDP_CHAN_STREAK_MASK;
  chan->chan_backoff &=
      (uint8_t)~(RUDP_CHAN_BACKOFF_MASK | RUDP_CHAN_FLAG_GAP_ACK_PENDING);
#if RUDP_EXTERNAL_RX_STORAGE
  if (chan->rx_present)
#endif
    memset(chan->rx_present, 0, RUDP_RX_BITMAP_SIZE);
  chan->srtt_scaled = chan->rttvar_scaled = chan->last_rtt_sample = 0;
  chan->rto_ms = chan->rto_initial_ms;

  return rudp_reset(&chan->ctx);
}

int rudp_session_reset(rudp_session_s *session) {
  if (!session) {
    return RUDP_ERR_INVALID_ARG;
  }

  for (uint8_t i = 0; i < RUDP_MAX_CHANNELS; i++) {
    rudp_session_reset_channel(session, i);
  }
  session->channels[0].unrel_ahead_streak = 0;
  session->channels[0].chan_backoff = 0;
  session->active_channels = 0;
  session->rr_cursor = 0;
  session->session_seed = 0;

  return RUDP_OK;
}

int rudp_session_set_mtu(rudp_session_s *session, size_t mtu_bytes) {
  if (!session ||
      mtu_bytes < (size_t)(RUDP_WIRE_DATAGRAM_HEADER_SIZE + RUDP_WIRE_RECORD_SIZE)) {
    return RUDP_ERR_INVALID_ARG;
  }
  size_t recs = (mtu_bytes - RUDP_WIRE_DATAGRAM_HEADER_SIZE) / RUDP_WIRE_RECORD_SIZE;
  if (recs > RUDP_MAX_RECORDS_PER_DATAGRAM) {
    recs = RUDP_MAX_RECORDS_PER_DATAGRAM;
  }
  uint8_t r = (uint8_t)recs;
  session->channels[0].unrel_ahead_streak =
      (uint8_t)((session->channels[0].unrel_ahead_streak & RUDP_CHAN_STREAK_MASK) |
                ((r & 0x1FU) << RUDP_CHAN_MTU_LO_SHIFT));
  session->channels[0].chan_backoff =
      (uint8_t)((session->channels[0].chan_backoff &
                 (RUDP_CHAN_BACKOFF_MASK | RUDP_CHAN_FLAG_GAP_ACK_PENDING)) |
                (r & RUDP_CHAN_MTU_HI_MASK));
  return (int)recs;
}

#if RUDP_EXTERNAL_RX_STORAGE
int rudp_session_attach_rx(rudp_session_s *session, uint8_t channel,
                           rudp_rx_storage_s *storage) {
  if (!session || channel >= RUDP_MAX_CHANNELS)
    return RUDP_ERR_INVALID_ARG;
  rudp_channel_s *chan = &session->channels[channel];
  if (chan->ctx.head != chan->ctx.tail)
    return RUDP_ERR_BUFFER_FULL;
  if (chan->rx_present) {
    for (size_t i = 0; i < RUDP_RX_BITMAP_SIZE; ++i)
      if (chan->rx_present[i])
        return RUDP_ERR_BUFFER_FULL;
  }
  if (storage) {
    for (size_t i = 0; i < RUDP_MAX_CHANNELS; ++i)
      if (i != channel && session->channels[i].rx_buffer == storage->packets)
        return RUDP_ERR_INVALID_ARG;
    memset(storage->present, 0, sizeof(storage->present));
  }
  chan->rx_buffer = storage ? storage->packets : NULL;
  chan->rx_present = storage ? storage->present : NULL;
  return RUDP_OK;
}
#endif
