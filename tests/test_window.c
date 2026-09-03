#include "protocol_rudp.h"
#include <stdio.h>
#include <assert.h>

/**
 * @brief Generic test verifying the lifecycle, boundary capacity, and modulo wrap-around
 *        for ANY valid compile-time RUDP_WINDOW_SIZE (from MIN=2 to MAX=32768).
 */
void test_window_lifecycle(void) {
    rudp_context_s ctx;
    tfv_packet_u dummy;
    dummy.raw = 0;

    assert(rudp_init(&ctx) == 0);
    printf("  Testing RUDP_WINDOW_SIZE = %u (Max In-Flight Capacity = %u slots)...\n", 
           (unsigned int)RUDP_WINDOW_SIZE, (unsigned int)(RUDP_WINDOW_SIZE - 1));

    uint16_t capacity = RUDP_WINDOW_SIZE - 1;

    // 1. Fill buffer to exact maximum capacity (N - 1 slots)
    for (uint16_t i = 0; i < capacity; i++) {
        assert(rudp_send(&ctx, dummy, 1000) == 0);
    }
    assert(ctx.head == capacity);
    assert(ctx.tail == 0);

    // 2. Overfill test: The N-th packet MUST be rejected (Buffer Full)
    assert(rudp_send(&ctx, dummy, 1000) == RUDP_ERR_BUFFER_FULL);

    // 3. Free the first packet (seq 0) with ACK=1 (N+1 convention)
    assert(rudp_recv_ack(&ctx, 1) == RUDP_OK);
    assert(ctx.tail == 1);

    // 4. Send 1 new packet: head wraps around in circular modulo: (capacity + 1) & (N - 1) == 0
    assert(rudp_send(&ctx, dummy, 1000) == RUDP_OK);
    assert(ctx.head == 0);

    // 5. Buffer is full again
    assert(rudp_send(&ctx, dummy, 1000) == RUDP_ERR_BUFFER_FULL);

    // 6. Cumulative ACK acknowledging all in-flight packets
    assert(rudp_recv_ack(&ctx, (uint16_t)RUDP_WINDOW_SIZE) == 0);
    assert(ctx.tail == 0);
    assert(ctx.head == 0);

    printf("  [OK] RUDP_WINDOW_SIZE = %u PASSED (Capacity, Rejection & Modulo Wrap-Around)\n", 
           (unsigned int)RUDP_WINDOW_SIZE);
}

int main(void) {
    printf("--- WINDOW SIZE CONFORMANCE TEST ---\n");
    test_window_lifecycle();
    printf("\n");
    return 0;
}
