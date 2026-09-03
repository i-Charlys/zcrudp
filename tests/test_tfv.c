#include "protocol_tfv.h"
#include <stdio.h>
#include <assert.h>
#include <stddef.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    #define IS_LITTLE_ENDIAN 1
#else
    #define IS_LITTLE_ENDIAN 0
#endif

/**
 * @brief Main function
 */
int main(void) {
    tfv_packet_u packet;

    packet.raw = 0; // not good practice, but for testing it's ok

    packet.type = 1;
    packet.flags = 15;
    packet.value = 4096;
    printf("type: %u, flags: %u, value: %u\n", packet.type, packet.flags, packet.value);
    printf("memory length: %zu bytes\n", sizeof(tfv_packet_u));
    printf("raw: %x\n", packet.raw);
    assert(sizeof(tfv_packet_u) == 4);
    assert(sizeof(packet.type) == 1);
    assert(sizeof(packet.flags) == 1);
    assert(sizeof(packet.value) == 2);


    #if IS_LITTLE_ENDIAN
        printf("System is little-endian\n");
    #else
        printf("System is big-endian\n");
    #endif

    assert(offsetof(tfv_packet_u, type) == 0);
    assert(offsetof(tfv_packet_u, flags) == 1);
    assert(offsetof(tfv_packet_u, value) == 2);

    packet.type = UINT8_MAX;
    packet.flags = UINT8_MAX;
    packet.value = UINT16_MAX;
    assert(packet.type == UINT8_MAX);
    assert(packet.flags == UINT8_MAX);
    assert(packet.value == UINT16_MAX);

    /* Test zero-dependency byte swap macros */
    assert(rudp_bswap16(0x1234) == 0x3412);
    assert(rudp_bswap32(0x12345678) == 0x78563412);

    /* Test network byte order conversions roundtrip */
    uint16_t host16 = 0xABCD;
    uint16_t net16 = rudp_htons(host16);
    assert(rudp_ntohs(net16) == host16);

    uint32_t host32 = 0xDEADBEEF;
    uint32_t net32 = rudp_htonl(host32);
    assert(rudp_ntohl(net32) == host32);

    return 0;
}
