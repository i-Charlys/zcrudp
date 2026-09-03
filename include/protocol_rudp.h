#ifndef PROTOCOL_RUDP_H
#define PROTOCOL_RUDP_H

#ifndef RUDP_WINDOW_SIZE
#define RUDP_WINDOW_SIZE 64
#endif

#if RUDP_WINDOW_SIZE < 2 || RUDP_WINDOW_SIZE > 32768 || \
    (RUDP_WINDOW_SIZE & (RUDP_WINDOW_SIZE - 1)) != 0
#error "RUDP_WINDOW_SIZE must be a power of 2 between 2 and 32768"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RUDP_SLOT_FREE      0
#define RUDP_SLOT_IN_FLIGHT 1

#define RUDP_FAST_RETRANSMIT_OFF     0 /**< Normal timer-based transmission */
#define RUDP_FAST_RETRANSMIT_PENDING 1 /**< Fast retransmit flag raised by Tri-ACK */

#define RUDP_WIRE_HEADER_SIZE 4 /**< Standalone header size (Tier 1: seq_num + ack) */
#define RUDP_WIRE_FRAME_SIZE  8 /**< Standard frame size (Tier 2: Header + TFV Packet) */

#define RUDP_STATE_DISCONNECTED 0 /**< Peer is disconnected or timed out */
#define RUDP_STATE_CONNECTED    1 /**< Active healthy connection */

#ifndef RUDP_MAX_RETRIES
#define RUDP_MAX_RETRIES        10 /**< Max retransmissions before declaring dead peer */
#endif

/* Standardized Return & Error Codes */
#define RUDP_OK                  0  /**< Success / operation completed */
#define RUDP_ERR_INVALID_ARG    -1  /**< NULL pointer or invalid argument */
#define RUDP_ERR_DISCONNECTED   -2  /**< Connection dead or disconnected */
#define RUDP_ERR_BUFFER_FULL    -3  /**< Transmission buffer is full */
#define RUDP_ERR_OUT_OF_WINDOW  -4  /**< Sequence or ACK is outside active window boundaries */





#include "protocol_tfv.h"
#include <stdint.h>
#include <stddef.h>


/**
 * @brief Represents the header of a RUDP frame. Length: 4 bytes.
 */
typedef struct {
    uint16_t seq_num; /**< Sequence number of this packet */
    uint16_t ack;     /**< Cumulative ACK: Next expected sequence number (N+1) */
} rudp_header_s;


/**
 * @brief Represents a RUDP frame, containing a header and a TFV packet. Length: 8 bytes.
 */
typedef struct { 
    rudp_header_s header; /**< Header: 4 bytes */
    tfv_packet_u packet;  /**< TFV packet: 4 bytes */
} rudp_frame_s;


/**
 * @brief Represents a RUDP slot, containing a frame, a state, and a timestamp. Length: 13 bytes (padded to 16 bytes).
 */
typedef struct {
    rudp_frame_s frame;
    uint32_t timestamp;
    uint8_t state; /**< 0: RUDP_SLOT_FREE, 1: RUDP_SLOT_IN_FLIGHT */
    uint8_t retries; /**< Number of retransmission attempts for this slot */
    uint8_t fast_retransmit; /**< Flag indicating if fast retransmit is needed */
    uint8_t reserved; /**< Padding for alignment */

} rudp_slot_s; 

/**
 * @brief Represents the full RUDP context / protocol state machine.
 */
typedef struct {
    rudp_slot_s tx_buffer[RUDP_WINDOW_SIZE]; /**< Sliding transmission window (ring buffer) */
    uint16_t head;                           /**< Next write index in tx_buffer */
    uint16_t tail;                           /**< Oldest unacknowledged in-flight index */
    uint16_t current_seq_num;                /**< Next sequence number to assign for outgoing packets */
    uint16_t expected_seq_num;               /**< Next expected sequence number for incoming packets */
    uint16_t last_ack_received;              /**< Last ACK number received from peer */
    uint8_t  duplicate_ack_count;            /**< Count of duplicate ACKs received (Fast Retransmit) */
    uint8_t  state;                          /**< Connection lifecycle state */
} rudp_context_s;


/**
 * @brief Initializes a RUDP context.
 *
 * @param ctx Pointer to the RUDP context to initialize.
 * @return 0 on success, -1 on error.
 */
int rudp_init(rudp_context_s *ctx);

/**
 * @brief Resets an existing RUDP context back to its initial connected state.
 *
 * @param ctx Pointer to the RUDP context.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_reset(rudp_context_s *ctx);

/**
 * @brief Queues a packet for transmission into the sliding window.
 *
 * @param ctx Pointer to the RUDP context.
 * @param packet TFV packet payload to send.
 * @param now Current timestamp in milliseconds.
 * @return 0 on success, -1 if the transmission buffer is full.
 */
int rudp_send(rudp_context_s *ctx, tfv_packet_u packet, uint32_t now);


/**
 * @brief Processes an incoming RUDP frame, updates cumulative ACK, and extracts the TFV packet.
 *
 * @param ctx Pointer to the RUDP context.
 * @param frame Pointer to the received RUDP frame.
 * @param out_packet Pointer to store the extracted TFV packet.
 * @return 1 on new in-order packet delivered, 0 if duplicate/out-of-order, -1 on error (e.g., NULL pointers).
 */
int rudp_recv(rudp_context_s *ctx, const rudp_frame_s *frame, tfv_packet_u *out_packet);

/**
 * @brief Processes an incoming cumulative ACK under the N+1 convention.
 *
 * @param ctx Pointer to the RUDP context.
 * @param ack_num Next expected sequence number from peer (N+1).
 * @return 0 on success, -1 if the ACK is out-of-window or corrupted.
 */
int rudp_recv_ack(rudp_context_s *ctx, uint16_t ack_num);

/**
 * @brief Serializes a 4-byte RUDP header into Big-Endian network format.
 *
 * @param header Pointer to the source header struct.
 * @param out_buf Destination byte buffer.
 * @param max_len Maximum capacity of destination buffer (must be >= RUDP_WIRE_HEADER_SIZE).
 * @return Number of bytes written (4 on success), or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_pack_header(const rudp_header_s *header, uint8_t *out_buf, size_t max_len);

/**
 * @brief Serializes a 4-byte TFV packet payload into Big-Endian network format.
 *
 * @param packet Pointer to the source TFV packet.
 * @param out_buf Destination byte buffer.
 * @param max_len Maximum capacity of destination buffer (must be >= sizeof(tfv_packet_u)).
 * @return Number of bytes written (4 on success), or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_pack_payload(const tfv_packet_u *packet, uint8_t *out_buf, size_t max_len);

/**
 * @brief Serializes a RUDP frame (Tier 2: 8 bytes) into a network-ready Big-Endian byte buffer.
 *
 * @param frame Pointer to the source frame.
 * @param out_buf Destination byte buffer.
 * @param max_len Maximum writable capacity of out_buf (must be >= RUDP_WIRE_FRAME_SIZE).
 * @return Number of bytes written (8 on success), or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_pack_frame(const rudp_frame_s *frame, uint8_t *out_buf, size_t max_len);

/**
 * @brief Serializes a standalone cumulative ACK (Tier 1: 4 bytes) into network Big-Endian format.
 *
 * @param ack_num The cumulative sequence number being acknowledged.
 * @param out_buf Destination byte buffer.
 * @param max_len Maximum writable capacity of out_buf (must be >= RUDP_WIRE_HEADER_SIZE).
 * @return Number of bytes written (4 on success), or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_pack_ack(uint16_t ack_num, uint8_t *out_buf, size_t max_len);

/**
 * @brief Decodes a 4-byte RUDP header from network Big-Endian format.
 *
 * @param in_buf Raw network bytes.
 * @param in_len Length of input buffer (must be >= RUDP_WIRE_HEADER_SIZE).
 * @param out_header Pointer to store the decoded header.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_unpack_header(const uint8_t *in_buf, size_t in_len, rudp_header_s *out_header);

/**
 * @brief Decodes a 4-byte TFV packet payload from Big-Endian network format.
 *
 * @param in_buf Raw network bytes.
 * @param in_len Length of input buffer (must be >= sizeof(tfv_packet_u)).
 * @param out_packet Pointer to store the extracted TFV packet.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_unpack_payload(const uint8_t *in_buf, size_t in_len, tfv_packet_u *out_packet);

/**
 * @brief Decodes a standalone ACK (Tier 1: 4 bytes) from network format.
 *
 * @param in_buf Raw network bytes.
 * @param in_len Length of input buffer (must be >= RUDP_WIRE_HEADER_SIZE).
 * @param out_ack Pointer to store the extracted ACK number.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_unpack_ack(const uint8_t *in_buf, size_t in_len, uint16_t *out_ack);

/**
 * @brief Deserializes a network byte buffer into a full RUDP frame struct (Tier 2: 8 bytes).
 *
 * @param in_buf Source byte buffer received from network.
 * @param in_len Number of bytes received (must be >= RUDP_WIRE_FRAME_SIZE).
 * @param out_frame Destination frame pointer.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_unpack_frame(const uint8_t *in_buf, size_t in_len, rudp_frame_s *out_frame);

/**
 * @brief Accessor retrieving a read-only pointer to the frame stored at a specific slot.
 *
 * @param ctx Pointer to the RUDP context.
 * @param slot_idx Index of the slot in tx_buffer (0 to RUDP_WINDOW_SIZE - 1).
 * @return Const pointer to the frame on success, or NULL if arguments are invalid.
 */
const rudp_frame_s *rudp_get_slot_frame(const rudp_context_s *ctx, uint16_t slot_idx);

/**
 * @brief Handles retransmissions for timed-out packets.
 *
 * @param ctx The RUDP context.
 * @param now Current time in milliseconds.
 * @param timeout Retransmission timeout in milliseconds.
 * @param out_indices Array provided by the caller to be filled with expired slot indices.
 * @param max_indices The maximum number of indices the array can hold.
 * @return The number of packets marked for retransmission, or negative error code.
 */
int rudp_tick(rudp_context_s *ctx, uint32_t now, uint32_t timeout, uint16_t *out_indices, int max_indices);

#ifdef __cplusplus
}
#endif

#endif // PROTOCOL_RUDP_H
