#ifndef PROTOCOL_TFV_H
#define PROTOCOL_TFV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    #define RUDP_IS_LITTLE_ENDIAN 1
#else
    #define RUDP_IS_LITTLE_ENDIAN 0
#endif

#if defined(__GNUC__) || defined(__clang__)
    #define rudp_bswap16(x) __builtin_bswap16(x)
    #define rudp_bswap32(x) __builtin_bswap32(x)
#else
    #define rudp_bswap16(x) ((uint16_t)(((x) >> 8) | ((x) << 8)))
    #define rudp_bswap32(x) ((uint32_t)(((x) >> 24) | (((x) & 0x00FF0000) >> 8) | \
                                       (((x) & 0x0000FF00) << 8) | ((x) << 24)))
#endif

/* Zero-dependency Network Byte Order conversions (Big-Endian wire format) */
#if RUDP_IS_LITTLE_ENDIAN
    #define rudp_htons(x) rudp_bswap16(x)
    #define rudp_ntohs(x) rudp_bswap16(x)
    #define rudp_htonl(x) rudp_bswap32(x)
    #define rudp_ntohl(x) rudp_bswap32(x)
#else
    #define rudp_htons(x) (x)
    #define rudp_ntohs(x) (x)
    #define rudp_htonl(x) (x)
    #define rudp_ntohl(x) (x)
#endif

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

#ifdef __cplusplus
}
#endif

#endif // PROTOCOL_TFV_H