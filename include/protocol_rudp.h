#ifndef PROTOCOL_RUDP_H
#define PROTOCOL_RUDP_H

#ifndef RUDP_WINDOW_SIZE
#define RUDP_WINDOW_SIZE 64
#endif

#if RUDP_WINDOW_SIZE < 2 || RUDP_WINDOW_SIZE > 32768 || \
    (RUDP_WINDOW_SIZE & (RUDP_WINDOW_SIZE - 1)) != 0
#error "RUDP_WINDOW_SIZE must be a power of 2 between 2 and 32768"
#endif


#ifdef RUDP_PACKED_STRUCTURES
    #define RUDP_PACKED __attribute__((packed))
#else
    #define RUDP_PACKED
#endif

#define RUDP_SLOT_FREE      0
#define RUDP_SLOT_IN_FLIGHT 1

#define RUDP_WIRE_HEADER_SIZE 4 /**< Standalone header size (Tier 1: seq_num + ack) */
#define RUDP_WIRE_FRAME_SIZE  8 /**< Standard frame size (Tier 2: Header + TFV Packet) */
#define RUDP_WIRE_DYNAMIC_SIZE -1 /**< Dynamic stream frame size (Tier 3) */


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
    uint8_t state; /**< 0: RUDP_SLOT_FREE, 1: RUDP_SLOT_IN_FLIGHT */
    uint32_t timestamp;
    
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
 * @brief Serializes a RUDP frame into a network-ready Big-Endian byte buffer.
 *
 * @param frame Pointer to the source frame.
 * @param out_buf Destination byte buffer.
 * @param max_len Maximum writable capacity of out_buf (must be >= RUDP_WIRE_FRAME_SIZE).
 * @return Number of bytes written (8 on success), or -1 on error.
 */
int rudp_pack_frame(const rudp_frame_s *frame, uint8_t *out_buf, size_t max_len);

/**
 * @brief Deserializes a network byte buffer into a RUDP frame struct.
 *
 * @param in_buf Source byte buffer received from network.
 * @param in_len Number of bytes received (must be >= RUDP_WIRE_FRAME_SIZE).
 * @param out_frame Destination frame pointer.
 * @return 0 on success, or -1 on error.
 */
int rudp_unpack_frame(const uint8_t *in_buf, size_t in_len, rudp_frame_s *out_frame);


/**
 * @brief Handles retransmissions for timed-out packets.
 * * @param ctx The RUDP context.
 * @param now Current time in milliseconds.
 * @param timeout Retransmission timeout in milliseconds.
 * @param out_indices Array provided by the caller to be filled with expired slot indices.
 * @param max_indices The maximum number of indices the array can hold.
 * @return The number of packets marked for retransmission, or -1 on error.
 */
int rudp_tick(rudp_context_s *ctx, uint32_t now, uint32_t timeout, uint16_t *out_indices, int max_indices);


#endif // PROTOCOL_RUDP_H
