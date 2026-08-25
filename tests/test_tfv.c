#include "../include/protocol_tfv.h"
#include <stdio.h>
#include <assert.h>
#include <stddef.h>

/**
 * @brief Main function
 */
int main() {
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


    uint16_t detect_endianness = 0x0123;
    uint8_t *bytes = (uint8_t *)&detect_endianness;
    if (bytes[0] == 0x23 && bytes[1] == 0x01) {
        printf("System is little-endian\n");
    } else if (bytes[0] == 0x01 && bytes[1] == 0x23) {
        printf("System is big-endian\n");
    } else {
        printf("Unknown endianness\n");
        assert(0); // Fail the test if endianness is unknown
    }

    assert(offsetof(tfv_packet_u, type) == 0);
    assert(offsetof(tfv_packet_u, flags) == 1);
    assert(offsetof(tfv_packet_u, value) == 2);

    packet.type = UINT8_MAX;
    packet.flags = UINT8_MAX;
    packet.value = UINT16_MAX;
    assert(packet.type == UINT8_MAX);
    assert(packet.flags == UINT8_MAX);
    assert(packet.value == UINT16_MAX);




    return 0;
}
