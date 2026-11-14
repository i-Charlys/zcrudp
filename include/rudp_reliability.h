#ifndef RUDP_RELIABILITY_H
#define RUDP_RELIABILITY_H

#include "rudp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Updates the last received timestamp on a RUDP context for liveness
 * tracking.
 *
 * @param ctx Pointer to the RUDP context.
 * @param now Current timestamp in milliseconds.
 */
RUDP_EXPORT void rudp_touch(rudp_context_s *ctx, uint32_t now);

/**
 * @brief Checks if a RUDP context connection is active and responsive within an
 * idle timeout.
 *
 * @param ctx Pointer to the RUDP context.
 * @param now Current timestamp in milliseconds.
 * @param idle_timeout Max elapsed milliseconds without incoming traffic (0
 * disables idle check).
 * @return true if connected and within idle timeout, false otherwise.
 */
RUDP_EXPORT bool rudp_is_alive(const rudp_context_s *ctx, uint32_t now,
                               uint32_t idle_timeout);

/**
 * @brief Initializes a RUDP context.
 *
 * @param ctx Pointer to the RUDP context to initialize.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG.
 */
RUDP_EXPORT int rudp_init(rudp_context_s *ctx);

/**
 * @brief Resets a RUDP context to a healthy connected state, clearing in-flight
 * buffers and sequence numbers.
 *
 * @param ctx Pointer to the RUDP context.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
RUDP_EXPORT int rudp_reset(rudp_context_s *ctx);

/**
 * @brief Queues a packet for transmission into the sliding window.
 *
 * @param ctx Pointer to the RUDP context.
 * @param packet TFV packet payload to send.
 * @param now Current timestamp in milliseconds.
 * @return RUDP_OK on success, RUDP_ERR_BUFFER_FULL if buffer is full,
 * RUDP_ERR_DISCONNECTED if disconnected, or RUDP_ERR_INVALID_ARG.
 */
RUDP_EXPORT int rudp_send(rudp_context_s *ctx, tfv_packet_u packet, uint32_t now);

/**
 * @brief Processes an incoming RUDP frame, updates cumulative ACK, and extracts
 * the TFV packet.
 *
 * @param ctx Pointer to the RUDP context.
 * @param frame Pointer to the received RUDP frame.
 * @param out_packet Pointer to store the extracted TFV packet.
 * @return 1 on new packet delivered, 0 if duplicate/out-of-order, or negative
 * RUDP_ERR_* code on error.
 */
RUDP_EXPORT int rudp_recv(rudp_context_s *ctx, const rudp_frame_s *frame,
                          tfv_packet_u *out_packet);

/**
 * @brief Processes an incoming cumulative ACK under the N+1 convention with
 * explicit control over whether duplicate ACKs count towards Tri-ACK Fast
 * Retransmit.
 *
 * @param ctx Pointer to the RUDP context.
 * @param ack_num Next expected sequence number from peer (N+1).
 * @param count_duplicate_ack true if this ACK is a deliberate standalone ACK or
 * explicit ACK record, false if this is a passive piggybacked ACK on unrelated
 * data.
 * @return RUDP_OK on success, RUDP_ERR_OUT_OF_WINDOW if stale/ahead, or
 * RUDP_ERR_INVALID_ARG.
 */
RUDP_EXPORT int rudp_recv_ack_ex(rudp_context_s *ctx, uint16_t ack_num,
                                 bool count_duplicate_ack);

/**
 * @brief Processes an incoming cumulative ACK under the N+1 convention.
 *
 * @param ctx Pointer to the RUDP context.
 * @param ack_num Next expected sequence number from peer (N+1).
 * @return RUDP_OK on success, RUDP_ERR_OUT_OF_WINDOW if stale/ahead, or
 * RUDP_ERR_INVALID_ARG.
 */
RUDP_EXPORT int rudp_recv_ack(rudp_context_s *ctx, uint16_t ack_num);

/**
 * @brief Accessor retrieving a read-only pointer to the frame stored at a
 * specific slot.
 *
 * @param ctx Pointer to the RUDP context.
 * @param slot_idx Index of the slot in tx_buffer (0 to RUDP_WINDOW_SIZE - 1).
 * @return Const pointer to the frame on success, or NULL if arguments are
 * invalid.
 */
RUDP_EXPORT const rudp_frame_s *rudp_get_slot_frame(const rudp_context_s *ctx,
                                                    uint16_t slot_idx);

/**
 * @brief Collects the indices of all slots currently in flight and
 * unacknowledged.
 *
 * @param ctx Pointer to the RUDP context.
 * @param out_indices Destination array to store slot indices.
 * @param max_indices Maximum capacity of out_indices.
 * @return Number of indices written (>= 0), or RUDP_ERR_INVALID_ARG on error.
 */
RUDP_EXPORT int rudp_get_unacked_slots(const rudp_context_s *ctx, uint16_t *out_indices,
                                       int max_indices);

/**
 * @brief Handles retransmissions for timed-out packets.
 *
 * @param ctx The RUDP context.
 * @param now Current time in milliseconds.
 * @param timeout Retransmission timeout in milliseconds.
 * @param out_indices Array provided by the caller to be filled with expired
 * slot indices.
 * @param max_indices The maximum number of indices the array can hold.
 * @return A result whose count is the number of collected slot indices and
 * whose status is RUDP_OK, RUDP_ERR_INVALID_ARG, or RUDP_ERR_DISCONNECTED.
 */
RUDP_EXPORT rudp_tick_result_s rudp_tick(rudp_context_s *ctx, uint32_t now,
                                         uint32_t timeout, uint16_t *out_indices,
                                         int max_indices);

#ifdef __cplusplus
}
#endif

#endif /* RUDP_RELIABILITY_H */
