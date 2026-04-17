#include "../include/protocol_tlv.h"
#include <stdio.h>

/**
 * @brief Main function
 */
int main() {
    tlv_packet_u packet;
    
    packet.raw = 0;
    
    packet.type = 1;
    packet.flags = 15;
    packet.value = 4096;
    printf("type: %u, flags: %u, value: %u\n", packet.type, packet.flags, packet.value);
    printf("Taille memoire : %zu octets\n", sizeof(tlv_packet_u));
    printf("raw: %x\n", packet.raw);
    
    return 0;
}
