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
#define RUDP_WIRE_DATAGRAM_HEADER_SIZE 4 /**< Datagram header size on wire (ack + ack_channel + count) */
#define RUDP_WIRE_RECORD_SIZE          8 /**< Message record size on wire (channel_id + flags + seq_num + tfv) */

#define RUDP_RECORD_FLAG_UNRELIABLE  (0U)      /**< Unreliable fire-and-forget message */
#define RUDP_RECORD_FLAG_RELIABLE    (1U << 0) /**< Reliable message requiring sliding window delivery */
#define RUDP_RECORD_FLAG_ACK         (1U << 1) /**< Explicit multi-channel ACK record (seq_num carries ACK N+1) */

#define RUDP_STATE_DISCONNECTED 0 /**< Peer is disconnected or timed out */
#define RUDP_STATE_CONNECTED    1 /**< Active healthy connection */

#ifndef RUDP_MAX_RETRIES
#define RUDP_MAX_RETRIES        10 /**< Max retransmissions before declaring dead peer */
#endif

#ifndef RUDP_MAX_CHANNELS
#define RUDP_MAX_CHANNELS       4  /**< Default number of independent channels per session */
#endif

/* Channel Capability Bitwise Flags */
#define RUDP_CHANNEL_FLAG_UNRELIABLE  (0U)      /**< Fire-and-forget unreliable channel */
#define RUDP_CHANNEL_FLAG_RELIABLE    (1U << 0) /**< Delivery guaranteed via sliding window retransmission */
#define RUDP_CHANNEL_FLAG_ORDERED     (1U << 1) /**< Packets delivered strictly in sequential order */
#define RUDP_CHANNEL_FLAG_ENCRYPTED   (1U << 2) /**< Egress payload encapsulated in WireGuard/Noise AEAD */

#ifndef RUDP_DEFAULT_MTU
#define RUDP_DEFAULT_MTU        1400 /**< Safe default UDP datagram payload limit preventing IP fragmentation */
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
 * @brief Represents the result of a RUDP tick operation.
 */
typedef struct {
    int count;  /**< Count of already-collected expired indices (>= 0) */
    int status; /**< RUDP_OK (0), RUDP_ERR_DISCONNECTED (-2), RUDP_ERR_INVALID_ARG (-1) */
} rudp_tick_result_s;


/**
 * @brief Datagram Header (4 bytes, sent once per UDP datagram).
 *        If count == 0, this datagram represents a pure Standalone ACK.
 */
typedef struct {
    uint16_t ack;         /**< Piggybacked cumulative ACK sequence number */
    uint8_t  ack_channel; /**< Channel ID to which the ACK applies */
    uint8_t  count;       /**< Number of bundled records in this datagram (0 = Standalone ACK) */
} rudp_datagram_header_s;

/**
 * @brief Individual Message Record (8 bytes, repeated 'count' times in datagram).
 */
typedef struct {
    uint8_t      channel_id; /**< Target channel identifier (0..RUDP_MAX_CHANNELS-1) */
    uint8_t      flags;      /**< Record flags (RUDP_RECORD_FLAG_*) */
    uint16_t     seq_num;    /**< 16-bit sequence (reliable sliding window or unreliable sequence) */
    tfv_packet_u payload;    /**< 4-byte game payload */
} rudp_record_s;

/**
 * @brief Represents an individual logical communication channel.
 */
typedef struct {
    uint8_t flags;                /**< Combination of RUDP_CHANNEL_FLAG_* */
    uint8_t channel_id;           /**< Channel identifier (0 to RUDP_MAX_CHANNELS - 1) */
    uint16_t last_unreliable_seq; /**< Last accepted unreliable sequence number (RX anti-rollback) */
    uint8_t  has_unreliable_seq;  /**< Flag: 1 if last_unreliable_seq is initialized, 0 otherwise */
    uint8_t  ack_pending;         /**< Flag: 1 if new reliable packet received needing ACK */
    uint16_t last_ack_sent;       /**< Last cumulative ACK transmitted for this channel */
    uint16_t next_unreliable_seq; /**< Next unreliable sequence number to transmit (TX) */
    uint16_t reserved;            /**< Explicit padding for strict 32-bit boundary alignment */
    rudp_context_s ctx;           /**< Dedicated sliding window context (used if RELIABLE) */
} rudp_channel_s;

/**
 * @brief Represents a peer session holding multiple independent channels.
 */
typedef struct {
    rudp_channel_s channels[RUDP_MAX_CHANNELS]; /**< Multi-channel array */
    uint8_t active_channels;                     /**< Number of configured channels */
    uint8_t reserved[3];                         /**< Explicit padding to 32-bit boundary */
} rudp_session_s;

/**
 * @brief Initializes a multi-channel session.
 *
 * @param session Pointer to the session struct.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_session_init(rudp_session_s *session);

/**
 * @brief Configures a channel within a session with specific capability flags.
 *
 * @param session Pointer to the session struct.
 * @param channel_id Channel identifier (0 to RUDP_MAX_CHANNELS - 1).
 * @param flags Bitwise combination of RUDP_CHANNEL_FLAG_*.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_session_config_channel(rudp_session_s *session, uint8_t channel_id, uint8_t flags);

/**
 * @brief Serializes a 4-byte datagram header into Big-Endian network format.
 *
 * @param header Pointer to the source datagram header.
 * @param out_buf Destination byte buffer.
 * @param max_len Maximum capacity of destination buffer (must be >= RUDP_WIRE_DATAGRAM_HEADER_SIZE).
 * @return Number of bytes written (4 on success), or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_pack_datagram_header(const rudp_datagram_header_s *header, uint8_t *out_buf, size_t max_len);

/**
 * @brief Deserializes a 4-byte datagram header from Big-Endian network format.
 *
 * @param in_buf Raw network bytes.
 * @param in_len Length of input buffer (must be >= RUDP_WIRE_DATAGRAM_HEADER_SIZE).
 * @param out_header Pointer to destination datagram header.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_unpack_datagram_header(const uint8_t *in_buf, size_t in_len, rudp_datagram_header_s *out_header);

/**
 * @brief Serializes an 8-byte message record into Big-Endian network format.
 *
 * @param record Pointer to the source record.
 * @param out_buf Destination byte buffer.
 * @param max_len Maximum capacity of destination buffer (must be >= RUDP_WIRE_RECORD_SIZE).
 * @return Number of bytes written (8 on success), or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_pack_record(const rudp_record_s *record, uint8_t *out_buf, size_t max_len);

/**
 * @brief Deserializes an 8-byte message record from Big-Endian network format.
 *
 * @param in_buf Raw network bytes.
 * @param in_len Length of input buffer (must be >= RUDP_WIRE_RECORD_SIZE).
 * @param out_record Pointer to destination record.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_unpack_record(const uint8_t *in_buf, size_t in_len, rudp_record_s *out_record);

/**
 * @brief Deserializes and validates an entire bundled datagram (header + count * records).
 *        Strictly checks exact length: in_len == 4 + count * 8.
 *
 * @param in_buf Raw network bytes.
 * @param in_len Length of input buffer.
 * @param out_header Pointer to destination datagram header.
 * @param out_records Pointer to array to receive unpacked records.
 * @param max_records Capacity of out_records array.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_unpack_datagram(const uint8_t *in_buf, size_t in_len,
                         rudp_datagram_header_s *out_header,
                         rudp_record_s *out_records, size_t max_records);

/**
 * @brief Sends an unreliable fire-and-forget payload bypassing the tx_buffer.
 *        Packs a complete 12-byte datagram (4B header + 8B record) with piggybacked ACK.
 *
 * @param session Pointer to the session struct.
 * @param channel_id Channel identifier (0 to RUDP_MAX_CHANNELS - 1).
 * @param payload 4-byte game payload.
 * @param ack_channel Channel identifier whose expected_seq_num is piggybacked.
 * @param out_buf Destination byte buffer.
 * @param max_len Capacity of destination buffer (must be >= 12 bytes).
 * @return Number of bytes written (12 on success), or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_session_send_unreliable(rudp_session_s *session, uint8_t channel_id,
                                 tfv_packet_u payload, uint8_t ack_channel,
                                 uint8_t *out_buf, size_t max_len);

/**
 * @brief Processes an incoming datagram across session channels.
 *        Dispatches piggybacked ACK, processes bundled records, and applies the
 *        16-bit anti-rollback sequence filter on unreliable channels.
 *
 * @param session Pointer to the session.
 * @param in_buf Received datagram bytes.
 * @param in_len Received byte length.
 * @param out_delivered Destination array to store delivered records for the game.
 * @param max_delivered Capacity of out_delivered.
 * @return Number of game records delivered (>= 0), or negative error code.
 */
int rudp_session_process_datagram(rudp_session_s *session, const uint8_t *in_buf, size_t in_len,
                                  rudp_record_s *out_delivered, size_t max_delivered);

/**
 * @brief Initializes a RUDP context.
 *
 * @param ctx Pointer to the RUDP context to initialize.
 * @return 0 on success, -1 on error.
 */
int rudp_init(rudp_context_s *ctx);

/**
 * @brief Resets a RUDP context to a healthy connected state, clearing in-flight buffers and sequence numbers.
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
 * @brief Collects the indices of all slots currently in flight and unacknowledged.
 *
 * @param ctx Pointer to the RUDP context.
 * @param out_indices Destination array to store slot indices.
 * @param max_indices Maximum capacity of out_indices.
 * @return Number of indices written (>= 0), or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_get_unacked_slots(const rudp_context_s *ctx, uint16_t *out_indices, int max_indices);

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
rudp_tick_result_s rudp_tick(rudp_context_s *ctx, uint32_t now, uint32_t timeout, uint16_t *out_indices, int max_indices);

#ifdef __cplusplus
}
#endif

#endif // PROTOCOL_RUDP_H
