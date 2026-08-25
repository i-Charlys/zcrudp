#ifndef PROTOCOL_TFV_H
#define PROTOCOL_TFV_H

#include <stdint.h>

/**
 * @brief TFV (Type-Flags-Value) packet structure (32-bit fixed frame)
 */
typedef union {
    uint32_t raw;
    struct {
        uint8_t type ;
        uint8_t flags ;
        uint16_t value ;
    };
} tfv_packet_u;

#endif // PROTOCOL_TFV_H