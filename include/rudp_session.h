#ifndef RUDP_SESSION_H
#define RUDP_SESSION_H

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
 * order. The library stores @p dscp but does not configure a socket.
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
 * @brief Enables adaptive recovery for one session channel.
 *
 * Bounds: 1 <= min <= initial <= max <= 60000 ms.
 * Configure only with empty TX/RX windows. Does not change the wire format.
 * Use rudp_session_process_datagram_at() to collect Karn-safe RTT samples. The
 * bundler uses its timeout argument only in fixed mode; adaptive mode uses the
 * channel RTO. Reset retains the bounds and resets RTO to initial_ms.
 *
 * @param session Pointer to the session.
 * @param channel Channel identifier (0 to RUDP_MAX_CHANNELS - 1).
 * @param initial_ms Initial retransmission timeout in milliseconds.
 * @param min_ms Minimum adaptive timeout in milliseconds.
 * @param max_ms Maximum adaptive timeout in milliseconds.
 * @return RUDP_OK on success, RUDP_ERR_BUFFER_FULL if the channel has queued
 * data, or RUDP_ERR_INVALID_ARG.
 */
RUDP_EXPORT int rudp_session_config_recovery(rudp_session_s *session, uint8_t channel,
                                             uint32_t initial_ms, uint32_t min_ms,
                                             uint32_t max_ms);

/**
 * @brief Processes a session datagram and supplies receive time for RTT input.
 *
 * Apart from adaptive RTT sampling, delivery and validation match
 * rudp_session_process_datagram().
 *
 * @param session Pointer to the session.
 * @param in Received datagram bytes.
 * @param length Exact datagram length.
 * @param out Destination array for delivered records.
 * @param capacity Number of records available in @p out.
 * @param now Receive timestamp in milliseconds using the sender's local clock.
 * @return Number of records delivered, or RUDP_ERR_INVALID_ARG.
 */
RUDP_EXPORT int rudp_session_process_datagram_at(rudp_session_s *session,
                                                 const uint8_t *in, size_t length,
                                                 rudp_record_s *out, size_t capacity,
                                                 uint32_t now);

/**
 * @brief Clears one channel's sequence, window and recovery state while
 * retaining its capability and QoS configuration.
 *
 * @param session Pointer to the session struct.
 * @param channel_id Channel identifier (0 to RUDP_MAX_CHANNELS - 1).
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
RUDP_EXPORT int rudp_session_reset_channel(rudp_session_s *session, uint8_t channel_id);

/**
 * @brief Resets all channels and active channel count in a session.
 *
 * @param session Pointer to the session struct.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
RUDP_EXPORT int rudp_session_reset(rudp_session_s *session);

/**
 * @brief Configures a channel within a session with specific capability flags.
 *
 * @param session Pointer to the session struct.
 * @param channel_id Channel identifier (0 to RUDP_MAX_CHANNELS - 1).
 * @param flags Bitwise combination of RUDP_CHANNEL_FLAG_*. RELIABLE controls
 * whether reliable sends are accepted. ORDERED and ENCRYPTED are currently
 * metadata only; reliable session receive is always ordered and encryption is
 * the responsibility of the integration.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
RUDP_EXPORT int rudp_session_config_channel(rudp_session_s *session, uint8_t channel_id,
                                            uint8_t flags);

/**
 * @brief Sends a reliable message over a session channel, queuing it into that
 * channel's tx_buffer.
 *
 * @param session Pointer to the session.
 * @param channel_id Channel identifier.
 * @param payload 4-byte game payload.
 * @param now Current timestamp in milliseconds.
 * @return RUDP_OK on success, RUDP_ERR_BUFFER_FULL if the TX window is full,
 * RUDP_ERR_DISCONNECTED if the channel has failed, or RUDP_ERR_INVALID_ARG.
 */
RUDP_EXPORT int rudp_session_send_reliable(rudp_session_s *session, uint8_t channel_id,
                                           tfv_packet_u payload, uint32_t now);

/**
 * @brief Transaction snapshot for two-phase prepare / commit / cancel egress.
 */
typedef struct {
  uint16_t slot_idx[RUDP_MAX_RECORDS_PER_DATAGRAM];
  uint8_t channel_id[RUDP_MAX_RECORDS_PER_DATAGRAM];
  uint8_t prev_tx_count[RUDP_MAX_RECORDS_PER_DATAGRAM];
  uint8_t prev_retries[RUDP_MAX_RECORDS_PER_DATAGRAM];
  uint8_t prev_fast_retransmit[RUDP_MAX_RECORDS_PER_DATAGRAM];
  uint8_t prev_slot_backoff[RUDP_MAX_RECORDS_PER_DATAGRAM];
  uint32_t prev_timestamp[RUDP_MAX_RECORDS_PER_DATAGRAM];
  uint8_t prev_chan_backoff[RUDP_MAX_CHANNELS];
  uint16_t prev_last_ack_sent[RUDP_MAX_CHANNELS];
  uint8_t prev_ack_pending[RUDP_MAX_CHANNELS];
  uint8_t prev_state[RUDP_MAX_CHANNELS];
  uint8_t prev_rr_cursor;
  uint8_t record_count;
  uint8_t valid;
} rudp_tx_plan_s;

/**
 * @brief Builds a unified datagram aggregating piggybacked ACK, multi-channel
 * pending ACKs, and in-flight reliable records into a single network packet up
 * to max_len (Intra-Tick Bundler).
 *
 * @param session Pointer to the session.
 * @param primary_ack_channel Channel ID to acknowledge in the 4-byte datagram
 * header.
 * @param out_buf Destination byte buffer.
 * @param max_len Maximum writable buffer capacity.
 * @param now Current timestamp in milliseconds.
 * @param timeout Retransmission timeout threshold in milliseconds.
 * @return Total bytes written (at least 4: the datagram header alone when no
 * record is eligible), RUDP_ERR_BUFFER_FULL if even the header does not fit,
 * RUDP_ERR_DISCONNECTED if a channel exhausted retries, or RUDP_ERR_INVALID_ARG.
 */
RUDP_EXPORT int rudp_session_build_datagram(rudp_session_s *session,
                                            uint8_t primary_ack_channel,
                                            uint8_t *out_buf, size_t max_len,
                                            uint32_t now, uint32_t timeout);

/**
 * @brief Two-phase transactional datagram preparation defaulting to channel 0
 * for the primary ACK header. Snapshots mutable TX/ACK state in @p out_plan
 * so rudp_session_cancel_datagram() can roll back cleanly on EAGAIN/EMSGSIZE.
 */
RUDP_EXPORT int rudp_session_prepare_datagram(rudp_session_s *session, uint32_t now,
                                              uint32_t timeout, uint8_t *out_buf,
                                              size_t cap, rudp_tx_plan_s *out_plan);

/**
 * @brief Finalizes a prepared datagram transmission after socket send succeeds.
 */
RUDP_EXPORT int rudp_session_commit_datagram(rudp_session_s *session,
                                             const rudp_tx_plan_s *plan);

/**
 * @brief Rolls back TX/ACK mutations captured by rudp_session_prepare_datagram()
 * when socket send fails with EAGAIN/EWOULDBLOCK or EMSGSIZE.
 */
RUDP_EXPORT int rudp_session_cancel_datagram(rudp_session_s *session,
                                             const rudp_tx_plan_s *plan);

/**
 * @brief Computes milliseconds until the next scheduled transmission or timeout.
 *
 * @return 0 if unsent records, pending ACKs, or fast retransmits are ready now;
 *         minimum positive delay until the earliest in-flight slot expires;
 *         UINT32_MAX when completely idle or arguments invalid.
 */
RUDP_EXPORT uint32_t rudp_session_next_wakeup_ms(const rudp_session_s *session,
                                                 uint32_t now,
                                                 uint32_t fallback_timeout_ms);

/**
 * @brief Configures and validates a per-session datagram MTU ceiling.
 *
 * @return Maximum number of 8-byte records fitting within @p mtu_bytes
 *         (capped at RUDP_MAX_RECORDS_PER_DATAGRAM), or RUDP_ERR_INVALID_ARG.
 */
RUDP_EXPORT int rudp_session_set_mtu(rudp_session_s *session, size_t mtu_bytes);

/**
 * @brief Sends an unreliable fire-and-forget payload bypassing the tx_buffer.
 *        Packs a complete 12-byte datagram (4B header + 8B record) with
 * piggybacked ACK.
 *
 * @param session Pointer to the session struct.
 * @param channel_id Channel identifier (0 to RUDP_MAX_CHANNELS - 1).
 * @param payload 4-byte game payload.
 * @param ack_channel Channel identifier whose expected_seq_num is piggybacked.
 * @param out_buf Destination byte buffer.
 * @param max_len Capacity of destination buffer (must be >= 12 bytes).
 * @return Number of bytes written (12 on success), RUDP_ERR_DISCONNECTED, or
 * RUDP_ERR_INVALID_ARG on error.
 */
RUDP_EXPORT int rudp_session_send_unreliable(rudp_session_s *session, uint8_t channel_id,
                                             tfv_packet_u payload, uint8_t ack_channel,
                                             uint8_t *out_buf, size_t max_len);

/**
 * @brief Processes an incoming datagram across session channels.
 *        Dispatches piggybacked ACK, processes bundled records, and applies the
 *        16-bit anti-rollback sequence filter on unreliable channels.
 *        Reliable ahead-of-order records are retained in the bounded RX window.
 *        Delivery may include previously buffered records. If output fills,
 *        call rudp_session_poll() until it returns zero, then transmit pending
 *        ACKs. Buffered records are never cumulatively ACKed before delivery.
 *
 * @param session Pointer to the session.
 * @param in_buf Received datagram bytes.
 * @param in_len Received byte length.
 * @param out_delivered Destination array to store delivered records for the
 * game.
 * @param max_delivered Capacity of out_delivered.
 * @return Number of game records delivered, or RUDP_ERR_INVALID_ARG.
 */
RUDP_EXPORT int rudp_session_process_datagram(rudp_session_s *session,
                                              const uint8_t *in_buf, size_t in_len,
                                              rudp_record_s *out_delivered,
                                              size_t max_delivered);

#if RUDP_EXTERNAL_RX_STORAGE
/** Attach or detach caller-owned RX storage; NULL detaches it.
 * Requires empty TX and RX windows; returns BUFFER_FULL otherwise. A storage
 * object must belong to one channel only and outlive attachment. Reset preserves
 * the attachment and clears its bitmap. init starts with no attached RX storage.
 * Reliable input for an unattached channel rejects the complete datagram before
 * mutation. Unreliable input and ACKs do not require RX storage. Single owner:
 * all session calls and attached-memory access must be externally serialized. */
RUDP_EXPORT int rudp_session_attach_rx(rudp_session_s *session, uint8_t channel,
                                       rudp_rx_storage_s *storage);
#endif

#ifdef __cplusplus
}
#endif

#endif /* RUDP_SESSION_H */
