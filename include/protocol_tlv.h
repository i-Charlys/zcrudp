#ifndef PROTOCOL_TLV_H
#define PROTOCOL_TLV_H

#include <stdint.h>

/**
 * @brief TLV packet structure (32-bit fixed frame)
 */
typedef union {
    uint32_t raw;
    struct {
        uint32_t type  : 8;
        uint32_t flags : 4;
        uint32_t value : 20;
    };
} tlv_packet_u;

#endif // PROTOCOL_TLV_H