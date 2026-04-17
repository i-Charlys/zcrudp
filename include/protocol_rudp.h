#ifndef PROTOCOL_RUDP_H
#define PROTOCOL_RUDP_H

#ifndef RUDP_WINDOW_SIZE
#define RUDP_WINDOW_SIZE 64
#endif

#include "protocol_tlv.h"
#include <stdint.h>


/**
 * @brief Represents the header of a RUDP frame, containing a sequence number and an acknowledgment number. Length : 4 octets.
 */
typedef struct  {
    uint16_t seq_num;
    uint16_t ack;
} rudp_header_s ;


/**
 * @brief Represents a RUDP frame, containing a header and a TLV packet. Length : 8 octets.
 */
typedef struct  { 
    rudp_header_s header; /** Length : 4 octets */
    tlv_packet_u packet; /** Length : variable , max : 4 octets */
} rudp_frame_s;


/**
 * @brief Represents a RUDP slot, containing a frame, a state, and a timestamp. Length : 13 octets. But with the padding the length is 16 octets.
 */
typedef struct {
    rudp_frame_s frame;
    uint8_t state;
    uint32_t timestamp;
    
} rudp_slot_s ; 

typedef struct {
    rudp_slot_s tx_buffer[RUDP_WINDOW_SIZE];
    uint16_t head;
    uint16_t tail;
    uint16_t current_seq_num;
    
} rudp_context_s;


int rudp_init(rudp_context_s *ctx);
int rudp_send(rudp_context_s *ctx, tlv_packet_u packet);
int rudp_recv_ack(rudp_context_s *ctx, uint16_t ack_num);
int rudp_tick(rudp_context_s *ctx, uint32_t now, uint32_t timeout);


#endif // PROTOCOL_RUDP_H
