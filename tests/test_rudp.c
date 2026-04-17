#include "../include/protocol_rudp.h"
#include <stdio.h>


int main(void) {
    
    // Le test de vérité pour l'alignement mémoire
    printf("Taille du Header : %zu octets\n", sizeof(rudp_header_s));
    printf("Taille de la Trame : %zu octets\n", sizeof(rudp_frame_s));
    
    return 0;
}