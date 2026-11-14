#ifndef PROTOCOL_TFV_H
#define PROTOCOL_TFV_H

#include <stdint.h>

// Compatibility as a C++ library
#ifdef __cplusplus
extern "C" {
#endif

// Check BYTE ORDER (ENDIANNESS)
#ifndef RUDP_IS_LITTLE_ENDIAN
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) &&                     \
    defined(__ORDER_BIG_ENDIAN__)
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define RUDP_IS_LITTLE_ENDIAN 1
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define RUDP_IS_LITTLE_ENDIAN 0
#endif
#elif defined(_WIN32) || defined(_WIN64) || defined(__LITTLE_ENDIAN__) ||              \
    defined(__ARMEL__) || defined(__THUMBEL__) || defined(__AARCH64EL__) ||            \
    defined(_MIPSEL) || defined(__MIPSEL) || defined(__MIPSEL__) ||                    \
    defined(_M_IX86) || defined(_M_X64) || defined(_M_AMD64) || defined(_M_ARM) ||     \
    defined(_M_ARM64) || (defined(__riscv) && (!defined(__BYTE_ORDER__) || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__))
#define RUDP_IS_LITTLE_ENDIAN 1
#elif defined(__BIG_ENDIAN__) || defined(__ARMEB__) || defined(__THUMBEB__) ||         \
    defined(__AARCH64EB__) || defined(_MIPSEB) || defined(__MIPSEB) ||                 \
    defined(__MIPSEB__)
#define RUDP_IS_LITTLE_ENDIAN 0
#else
#error "Unable to determine target endianness; define RUDP_IS_LITTLE_ENDIAN to 0 or 1"
#endif
#endif

// Check compiler built-in bit swap for ENDIANNESS change
#if defined(__GNUC__) || defined(__clang__)
#define rudp_bswap16(x) __builtin_bswap16(x)
#define rudp_bswap32(x) __builtin_bswap32(x)
#elif defined(_MSC_VER)
#include <stdlib.h>
#define rudp_bswap16(x) _byteswap_ushort(x)
#define rudp_bswap32(x) _byteswap_ulong(x)
#else
#define rudp_bswap16(x) ((uint16_t)(((x) >> 8) | ((x) << 8)))
#define rudp_bswap32(x)                                                                \
  ((uint32_t)(((x) >> 24) | (((x) & 0x00FF0000) >> 8) | (((x) & 0x0000FF00) << 8) |    \
              ((x) << 24)))
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
    uint8_t type;
    uint8_t flags;
    uint16_t value;
  };
} tfv_packet_u;

/**
 * @brief Endian-independent helper to construct a tfv_packet_u.
 */
static inline tfv_packet_u rudp_tfv_make(uint8_t type_v, uint8_t flags_v,
                                         uint16_t value_v) {
  tfv_packet_u pkt;
  pkt.type = type_v;
  pkt.flags = flags_v;
  pkt.value = value_v;
  return pkt;
}

#define RUDP_TFV(type_v, flags_v, value_v)                                             \
  rudp_tfv_make((uint8_t)(type_v), (uint8_t)(flags_v), (uint16_t)(value_v))

#ifdef __cplusplus
}
#endif

#endif // PROTOCOL_TFV_H