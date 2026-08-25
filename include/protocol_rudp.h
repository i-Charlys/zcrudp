#ifndef PROTOCOL_RUDP_H
#define PROTOCOL_RUDP_H

#ifndef RUDP_WINDOW_SIZE
#define RUDP_WINDOW_SIZE 64
#endif

#if RUDP_WINDOW_SIZE == 0 || \
    (RUDP_WINDOW_SIZE & (RUDP_WINDOW_SIZE - 1)) != 0
#error "RUDP_WINDOW_SIZE must be a non-zero power of 2"
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
 * @brief Represents the header of a RUDP frame, containing a sequence number and an acknowledgment number. Length: 4 bytes.
 */
typedef struct  {
    uint16_t seq_num;
    uint16_t ack;
} rudp_header_s ;


/**
 * @brief Represents a RUDP frame, containing a header and a TFV packet. Length: 8 bytes.
 */
typedef struct  { 
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
    
} rudp_slot_s ; 

typedef struct {
    rudp_slot_s tx_buffer[RUDP_WINDOW_SIZE];
    uint16_t head;
    uint16_t tail;
    uint16_t current_seq_num;
    
} rudp_context_s;


int rudp_init(rudp_context_s *ctx);
int rudp_send(rudp_context_s *ctx, tfv_packet_u packet, uint32_t now);
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
