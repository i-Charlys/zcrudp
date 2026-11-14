#ifndef COMPARE_ENET_ZPL_H
#define COMPARE_ENET_ZPL_H

#include <stdint.h>
#include <stddef.h>

void link_send(int from, const void *bytes, unsigned len);
unsigned link_receive(int to, void *bytes);
void delivery(const uint8_t *bytes, size_t len);
int link_has_pending(void);

void zpl_enet_init_engine(uint32_t *now_ptr, uint32_t *loss_percent_ptr, int *failed_ptr);
void zpl_enet_service(int side, int setup);
unsigned zpl_enet_pending(void);
void zpl_enet_submit(uint16_t admitted);
void zpl_enet_cleanup(void);

#endif /* COMPARE_ENET_ZPL_H */
