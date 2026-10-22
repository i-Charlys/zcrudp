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
int rudp_session_init(rudp_session_s *session);

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
int rudp_session_poll(rudp_session_s *session, rudp_record_s *out, size_t capacity);

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
int rudp_session_set_qos(rudp_session_s *session, uint8_t channel, uint8_t priority,
                         uint8_t dscp);

/**
 * @brief Enables adaptive recovery for one session channel.
 *
 * Bounds: 1 <= min <= initial <= max <= 60000 ms.
 * Configure only with empty TX/RX windows. Does not change the wire format.
 * Use rudp_session_process_datagram_at() to collect Karn-safe RTT samples. The
 * bundler uses its timeout argument only in fixed mode; adaptive mode uses the
 * channel RTO. Reset retains the bounds and resets that RTO to the maximum.
 *
 * @param session Pointer to the session.
 * @param channel Channel identifier (0 to RUDP_MAX_CHANNELS - 1).
 * @param initial_ms Initial retransmission timeout in milliseconds.
 * @param min_ms Minimum adaptive timeout in milliseconds.
 * @param max_ms Maximum adaptive timeout in milliseconds.
 * @return RUDP_OK on success, RUDP_ERR_BUFFER_FULL if the channel has queued
 * data, or RUDP_ERR_INVALID_ARG.
 */
int rudp_session_config_recovery(rudp_session_s *session, uint8_t channel,
                                 uint32_t initial_ms, uint32_t min_ms, uint32_t max_ms);

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
int rudp_session_process_datagram_at(rudp_session_s *session, const uint8_t *in,
                                     size_t length, rudp_record_s *out, size_t capacity,
                                     uint32_t now);

/**
 * @brief Clears one channel's sequence, window and recovery state while
 * retaining its capability and QoS configuration.
 *
 * @param session Pointer to the session struct.
 * @param channel_id Channel identifier (0 to RUDP_MAX_CHANNELS - 1).
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_session_reset_channel(rudp_session_s *session, uint8_t channel_id);

/**
 * @brief Resets all channels and active channel count in a session.
 *
 * @param session Pointer to the session struct.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_session_reset(rudp_session_s *session);

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
int rudp_session_config_channel(rudp_session_s *session, uint8_t channel_id,
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
int rudp_session_send_reliable(rudp_session_s *session, uint8_t channel_id,
                               tfv_packet_u payload, uint32_t now);

/**
 * @brief Builds a unified datagram aggregating piggybacked ACK, multi-channel
 * pending ACKs, and in-flight reliable records into a single network packet up
 * to max_len (Intra-Tick Bundler).
 *
 * The 4-byte datagram header is always packed, even when no record is eligible,
 * and the primary channel's ack_pending flag is always cleared. A header-only
 * result (exactly 4 bytes, count = 0) is a pure ACK: send it only when
 * ack_pending was set before the call. Snapshot the flag first:
 *
 *   int ack_pending = session->channels[primary_ack_channel].ack_pending;
 *   int len = rudp_session_build_datagram(...);
 *   if (len > 4 || ack_pending) send(out_buf, len);
 *
 * Sending the header unconditionally floods the peer with duplicate ACKs, which
 * triggers spurious fast retransmits and starves RTT sampling. Returning 0 for
 * "nothing to send" is tracked in TODO.md (P0).
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
 * or RUDP_ERR_INVALID_ARG.
 */
int rudp_session_build_datagram(rudp_session_s *session, uint8_t primary_ack_channel,
                                uint8_t *out_buf, size_t max_len, uint32_t now,
                                uint32_t timeout);

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
 * @return Number of bytes written (12 on success), or RUDP_ERR_INVALID_ARG on
 * error.
 */
int rudp_session_send_unreliable(rudp_session_s *session, uint8_t channel_id,
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
int rudp_session_process_datagram(rudp_session_s *session, const uint8_t *in_buf,
                                  size_t in_len, rudp_record_s *out_delivered,
                                  size_t max_delivered);

#if RUDP_EXTERNAL_RX_STORAGE
/** Attach or detach caller-owned RX storage; NULL detaches it.
 * Requires empty TX and RX windows; returns BUFFER_FULL otherwise. A storage
 * object must belong to one channel only and outlive attachment. Reset preserves
 * the attachment and clears its bitmap. init starts with no attached RX storage.
 * Reliable input for an unattached channel rejects the complete datagram before
 * mutation. Unreliable input and ACKs do not require RX storage. Single owner:
 * all session calls and attached-memory access must be externally serialized. */
int rudp_session_attach_rx(rudp_session_s *session, uint8_t channel,
                           rudp_rx_storage_s *storage);
#endif

#ifdef __cplusplus
}
#endif

#endif /* RUDP_SESSION_H */
