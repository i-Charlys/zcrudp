#include "../include/protocol_rudp.h"
#include <stdio.h>
#include <assert.h> // Essential for testing

// --- 1. Basic Happy Path Test ---
void test_happy_path() {
    rudp_context_s ctx;
    tlv_packet_u dummy_packet;
    dummy_packet.raw = 0;
    uint16_t expired_slots[64];

    assert(rudp_init(&ctx) == 0);
    assert(ctx.head == 0 && ctx.tail == 0);

    // Send a packet
    assert(rudp_send(&ctx, dummy_packet, 1000) == 0);
    assert(ctx.head == 1);
    assert(ctx.tx_buffer[0].state == RUDP_SLOT_IN_FLIGHT);

    // Tick before expiration (100ms timeout)
    assert(rudp_tick(&ctx, 1050, 100, expired_slots, 64) == 0);

    // Tick after expiration
    assert(rudp_tick(&ctx, 1150, 100, expired_slots, 64) == 1);
    assert(expired_slots[0] == 0);

    // Receive ACK
    assert(rudp_recv_ack(&ctx, 0) == 0);
    assert(ctx.tail == 1);
    assert(ctx.tx_buffer[0].state == RUDP_SLOT_FREE);

    printf("[OK] Happy Path (Init, Send, Tick, ACK)\n");
}

// --- 2. Extreme Test: Domino Effect (Cumulative ACK) ---
void test_cumulative_ack() {
    rudp_context_s ctx;
    tlv_packet_u dummy;
    dummy.raw = 0;

    rudp_init(&ctx);

    // Send 10 packets (seq_num 0 to 9)
    for (int i = 0; i < 10; i++) {
        assert(rudp_send(&ctx, dummy, 1000) == 0);
    }
    assert(ctx.head == 10);
    assert(ctx.tail == 0);

    // Simulate that the network hasn't confirmed packets 0 to 8,
    // but we suddenly receive an ACK for packet 9
    assert(rudp_recv_ack(&ctx, 9) == 0);

    // The engine must have freed ALL packets from 0 to 9 at once
    assert(ctx.tail == 10);
    assert(ctx.tx_buffer[0].state == RUDP_SLOT_FREE);
    assert(ctx.tx_buffer[9].state == RUDP_SLOT_FREE);

    printf("[EXTREME OK] Cumulative ACK (Domino effect valid)\n");
}

// --- 3. Extreme Test: The 65535 Crash (Rollover) ---
void test_seq_num_rollover() {
    rudp_context_s ctx;
    tlv_packet_u dummy;
    dummy.raw = 0;

    rudp_init(&ctx);

    // Cheat to place ourselves just before the uint16_t limit
    ctx.current_seq_num = 65533;

    // Send 5 packets (they will have numbers 65533, 65534, 65535, 0, 1)
    for (int i = 0; i < 5; i++) {
        assert(rudp_send(&ctx, dummy, 1000) == 0);
    }

    // The sequence number must have rolled over to zero cleanly
    assert(ctx.current_seq_num == 2);
    assert(ctx.head == 5);

    // Validate packet 1 (which is the 5th packet sent)
    assert(rudp_recv_ack(&ctx, 1) == 0);

    // The logic (int16_t) must have understood that 1 is AFTER 65535
    // The tail must therefore have caught up with the head
    assert(ctx.tail == 5);

    printf("[EXTREME OK] Sequence Number Rollover (65535 -> 0 comparison is safe)\n");
}

// --- 4. Extreme Test: Buffer Full and Memory Wrap-Around ---
void test_buffer_full_and_wrap() {
    rudp_context_s ctx;
    tlv_packet_u dummy;
    dummy.raw = 0;

    rudp_init(&ctx);

    // A circular buffer of N slots can only store N-1 elements
    // to distinguish the "full" state from the "empty" state.
    int max_capacity = RUDP_WINDOW_SIZE - 1;

    // Fill the buffer to the maximum
    for (int i = 0; i < max_capacity; i++) {
        assert(rudp_send(&ctx, dummy, 1000) == 0);
    }

    // The extra packet MUST be rejected
    assert(rudp_send(&ctx, dummy, 1000) == -1);

    // Free the first half (packets 0 to 31)
    assert(rudp_recv_ack(&ctx, 31) == 0);
    assert(ctx.tail == 32); // The slot is freed

    // Send 32 new packets, which will force the "head" to
    // exceed index 63 and wrap around to 0
    for (int i = 0; i < 32; i++) {
        assert(rudp_send(&ctx, dummy, 1000) == 0);
    }

    // At this stage, the head was at 63. 63 + 32 = 95. 95 modulo 64 = 31.
    assert(ctx.head == 31);

    printf("[EXTREME OK] Buffer Full & Wrap-Around (RAM limit is respected)\n");
}

int main(void) {
    printf("--- MEMORY SIZE TESTS ---\n");
    printf("Header Size : %zu bytes\n", sizeof(rudp_header_s));
    printf("Frame Size : %zu bytes\n\n", sizeof(rudp_frame_s));

    printf("--- RUDP ENGINE TESTS ---\n");
    test_happy_path();
    test_cumulative_ack();
    test_seq_num_rollover();
    test_buffer_full_and_wrap();

    printf("\n>>> ALL TESTS PASSED SUCCESSFULLY! <<<\n");

    return 0;
}
