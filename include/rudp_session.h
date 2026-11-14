#ifndef RUDP_SESSION_H
#define RUDP_SESSION_H

#include "rudp_io.h"
#include "rudp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes a multi-channel session.
 *
 * @param session Pointer to the session struct.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
RUDP_EXPORT int rudp_session_init(rudp_session_s *session);

/**
 * @brief Configures the pseudo-random session seed used for multipath flow entropy.
 */
RUDP_EXPORT int rudp_session_set_seed(rudp_session_s *session, uint16_t seed);

/**
 * @brief Drains reliable records that have become contiguous in sequence.
 *
 * The cumulative receive position advances only for records copied to @p out.
 * Call this after a previous receive filled the caller's output array.
 *
 * @param session Pointer to the session.
 * @param out Destination array for delivered records.
 * @param capacity Number of records available in @p out; must be nonzero.
 * @return Number of records delivered, or RUDP_ERR_INVALID_ARG.
 */
RUDP_EXPORT int rudp_session_poll(rudp_session_s *session, rudp_record_s *out,
                                  size_t capacity);

/**
 * @brief Sets egress priority and a DSCP integration hint for one channel.
 *
 * Lower priority values are scheduled first. Equal priorities use round-robin
 * order. The library stores @p dscp and propagates the active lead channel's
 * DSCP into `rudp_iovec_collection_s::dscp`, without touching OS sockets.
 *
 * @param session Pointer to the session.
 * @param channel Channel identifier (0 to RUDP_MAX_CHANNELS - 1).
 * @param priority Egress priority; lower values run first.
 * @param dscp DSCP value from 0 to 63 for the socket owner to apply.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG.
 */
RUDP_EXPORT int rudp_session_set_qos(rudp_session_s *session, uint8_t channel,
                                     uint8_t priority, uint8_t dscp);

/**
 * @brief Enables adaptive recovery (RFC 6298 RTT + 32-bit SACK/FACK + TLP) for one channel.
 *
 * Bounds: 1 <= min <= initial <= max <= 60000 ms.
 * Configure only with empty TX/RX windows. Uses standard 4 + 8*count framing.
 * Use rudp_session_process_datagram_at() to collect Karn-safe RTT samples.
 */
RUDP_EXPORT int rudp_session_config_recovery(rudp_session_s *session, uint8_t channel,
                                             uint32_t initial_ms, uint32_t min_ms,
                                             uint32_t max_ms);

/**
 * @brief Processes a session datagram and supplies receive time for RTT input.
 */
RUDP_EXPORT int rudp_session_process_datagram_at(rudp_session_s *session,
                                                 const uint8_t *in, size_t length,
                                                 rudp_record_s *out, size_t capacity,
                                                 uint32_t now);

/**
 * @brief Processes a session datagram with optional timing and saturating telemetry.
 */
RUDP_EXPORT int rudp_session_process_datagram_ex(rudp_session_s *session,
                                                 const uint8_t *in_buf, size_t in_len,
                                                 rudp_record_s *out_delivered,
                                                 size_t max_delivered, bool timed,
                                                 uint32_t now, rudp_stats_s *stats);

/**
 * @brief Clears one channel's sequence, window and recovery state while
 * retaining its capability and QoS configuration.
 */
RUDP_EXPORT int rudp_session_reset_channel(rudp_session_s *session, uint8_t channel_id);

/**
 * @brief Resets all channels and active channel count in a session.
 */
RUDP_EXPORT int rudp_session_reset(rudp_session_s *session);

/**
 * @brief Configures a channel within a session with specific capability flags.
 *
 * - `RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED`: reliable in-order.
 * - `RUDP_CHANNEL_FLAG_RELIABLE` (without ORDERED): zero-HoL reliable unordered.
 * - `RUDP_CHANNEL_FLAG_FEC_K2`: optional K=2 XOR parity in separate datagrams.
 */
RUDP_EXPORT int rudp_session_config_channel(rudp_session_s *session, uint8_t channel_id,
                                            uint8_t flags);

/**
 * @brief Sends a reliable message over a session channel, queuing it into that
 * channel's tx_buffer.
 */
RUDP_EXPORT int rudp_session_send_reliable(rudp_session_s *session, uint8_t channel_id,
                                           tfv_packet_u payload, uint32_t now);

/**
 * @brief Transaction snapshot for two-phase prepare / commit / cancel egress.
 *        Slot backoff and FEC-emitted state are packed inside `prev_fast_retransmit`.
 */
typedef struct {
  uint16_t slot_idx[RUDP_MAX_RECORDS_PER_DATAGRAM];
  uint8_t channel_id[RUDP_MAX_RECORDS_PER_DATAGRAM];
  uint8_t prev_tx_count[RUDP_MAX_RECORDS_PER_DATAGRAM];
  uint8_t prev_retries[RUDP_MAX_RECORDS_PER_DATAGRAM];
  uint8_t prev_fast_retransmit[RUDP_MAX_RECORDS_PER_DATAGRAM];
  uint8_t prev_slot_state[RUDP_MAX_RECORDS_PER_DATAGRAM];
  uint32_t prev_timestamp[RUDP_MAX_RECORDS_PER_DATAGRAM];
  uint8_t prev_chan_backoff[RUDP_MAX_CHANNELS];
  uint16_t prev_last_ack_sent[RUDP_MAX_CHANNELS];
  uint16_t prev_tail[RUDP_MAX_CHANNELS];
  uint8_t prev_ack_pending[RUDP_MAX_CHANNELS];
  uint8_t prev_state[RUDP_MAX_CHANNELS];
  uint8_t prev_rr_cursor;
  uint8_t record_count;
  uint8_t valid;
} rudp_tx_plan_s;

/**
 * @brief Builds a unified datagram aggregating piggybacked ACK, multi-channel
 * pending ACKs / 32-bit SACK bitmaps, optional K=2 XOR parity, and in-flight
 * reliable records into a single network packet up to max_len.
 */
RUDP_EXPORT int rudp_session_build_datagram(rudp_session_s *session,
                                            uint8_t primary_ack_channel,
                                            uint8_t *out_buf, size_t max_len,
                                            uint32_t now, uint32_t timeout);

/**
 * @brief Builds a scatter-gather `rudp_iovec_collection_s` backed by @p scratch_buf,
 *        populating header/record slices, lead-channel DSCP, and multipath `path_entropy`.
 */
RUDP_EXPORT int rudp_session_build_iovec(rudp_session_s *session,
                                         uint8_t primary_ack_channel,
                                         uint16_t flow_hint, uint8_t *scratch_buf,
                                         size_t max_len, uint32_t now,
                                         uint32_t timeout,
                                         rudp_iovec_collection_s *out_col);

/**
 * @brief Two-phase transactional datagram preparation defaulting to channel 0
 * for the primary ACK header. Snapshots mutable TX/ACK state in @p out_plan
 * so rudp_session_cancel_datagram() can roll back cleanly on EAGAIN/EMSGSIZE.
 */
RUDP_EXPORT int rudp_session_prepare_datagram(rudp_session_s *session, uint32_t now,
                                              uint32_t timeout, uint8_t *out_buf,
                                              size_t cap, rudp_tx_plan_s *out_plan);

/**
 * @brief Two-phase transactional scatter-gather preparation populating @p out_col
 *        (slices, DSCP, and `path_entropy`) alongside @p out_plan for rollback.
 */
RUDP_EXPORT int rudp_session_prepare_iovec(rudp_session_s *session, uint16_t flow_hint,
                                           uint32_t now, uint32_t timeout,
                                           uint8_t *scratch_buf, size_t cap,
                                           rudp_iovec_collection_s *out_col,
                                           rudp_tx_plan_s *out_plan);

/**
 * @brief Finalizes a prepared datagram transmission after socket send succeeds.
 */
RUDP_EXPORT int rudp_session_commit_datagram(rudp_session_s *session,
                                             const rudp_tx_plan_s *plan);

/**
 * @brief Rolls back TX/ACK mutations captured by rudp_session_prepare_datagram()
 * or rudp_session_prepare_iovec() when socket send fails with EAGAIN or EMSGSIZE.
 */
RUDP_EXPORT int rudp_session_cancel_datagram(rudp_session_s *session,
                                             const rudp_tx_plan_s *plan);

/**
 * @brief Computes milliseconds until the next scheduled transmission, TLP, or RTO.
 */
RUDP_EXPORT uint32_t rudp_session_next_wakeup_ms(const rudp_session_s *session,
                                                 uint32_t now,
                                                 uint32_t fallback_timeout_ms);

/**
 * @brief Configures and validates a per-session datagram MTU ceiling.
 */
RUDP_EXPORT int rudp_session_set_mtu(rudp_session_s *session, size_t mtu_bytes);

/**
 * @brief Sends an unreliable fire-and-forget payload bypassing the tx_buffer.
 */
RUDP_EXPORT int rudp_session_send_unreliable(rudp_session_s *session, uint8_t channel_id,
                                             tfv_packet_u payload, uint8_t ack_channel,
                                             uint8_t *out_buf, size_t max_len);

/**
 * @brief Stages an unreliable payload into the channel ring for intra-tick
 *        aggregation alongside reliable/ACK records by build_datagram().
 *        Freed immediately on emission with zero retransmission or ACK wait.
 */
RUDP_EXPORT int rudp_session_queue_unreliable(rudp_session_s *session,
                                              uint8_t channel_id,
                                              tfv_packet_u payload);

/**
 * @brief Returns true if any connected channel has an immediate transmission due
 *        (pending ACK, ready FEC parity, staged data, fast repair, or expired timer).
 */
RUDP_EXPORT bool rudp_session_has_pending_work(const rudp_session_s *session,
                                               uint32_t now,
                                               uint32_t fallback_timeout_ms);

/**
 * @brief Processes an incoming datagram across session channels.
 */
RUDP_EXPORT int rudp_session_process_datagram(rudp_session_s *session,
                                              const uint8_t *in_buf, size_t in_len,
                                              rudp_record_s *out_delivered,
                                              size_t max_delivered);

#if RUDP_EXTERNAL_RX_STORAGE
RUDP_EXPORT int rudp_session_attach_rx(rudp_session_s *session, uint8_t channel,
                                       rudp_rx_storage_s *storage);
#endif

#ifdef __cplusplus
}
#endif

#endif /* RUDP_SESSION_H */
