#include "../include/protocol_rudp.h"
#include <stdio.h>
#include <assert.h> // Essential for testing

// --- 1. Basic Happy Path Test ---
void test_happy_path() {
    rudp_context_s ctx;
    tfv_packet_u dummy_packet;
    dummy_packet.raw = 0;                       // not good practice, but for testing it's ok
    uint16_t expired_slots[64];                 // Array to hold indices of expired slots

    assert(rudp_init(&ctx) == 0);               // check initialization
    assert(ctx.head == 0 && ctx.tail == 0);     // check that head and tail are initialized to 0

    // Send a packet (seq_num 0)
    assert(rudp_send(&ctx, dummy_packet, 1000) == 0);
    assert(ctx.head == 1);
    assert(ctx.tx_buffer[0].state == RUDP_SLOT_IN_FLIGHT);

    // Reject out-of-window / future ACK (e.g. ACK=50 when only seq 0 was sent)
    assert(rudp_recv_ack(&ctx, 50) == -1);

    // Tick before expiration (100ms timeout)
    assert(rudp_tick(&ctx, 1050, 100, expired_slots, 64) == 0);

    // Tick after expiration
    assert(rudp_tick(&ctx, 1150, 100, expired_slots, 64) == 1);
    assert(expired_slots[0] == 0);

    // Receive cumulative ACK under N+1 convention: ACK=1 acknowledges packet 0
    assert(rudp_recv_ack(&ctx, 1) == 0);
    assert(ctx.tail == 1);
    assert(ctx.tx_buffer[0].state == RUDP_SLOT_FREE);

    printf("[OK] Happy Path (Init, Send, Tick, N+1 ACK, Out-of-Window Rejection)\n");
}

// --- 2. Extreme Test: Domino Effect (Cumulative ACK) ---
void test_cumulative_ack() {
    rudp_context_s ctx;
    tfv_packet_u dummy;
    dummy.raw = 0;

    rudp_init(&ctx);

    // Send 10 packets (seq_num 0 to 9)
    for (int i = 0; i < 10; i++) {
        assert(rudp_send(&ctx, dummy, 1000) == 0);
    }
    assert(ctx.head == 10);
    assert(ctx.tail == 0);

    // Simulate receiving an ACK for packet sequence 10 (acknowledges 0 through 9)
    assert(rudp_recv_ack(&ctx, 10) == 0);

    // The engine must have freed ALL packets from 0 to 9 at once
    assert(ctx.tail == 10);
    assert(ctx.tx_buffer[0].state == RUDP_SLOT_FREE);
    assert(ctx.tx_buffer[9].state == RUDP_SLOT_FREE);

    printf("[EXTREME OK] Cumulative ACK (Domino effect valid with N+1)\n");
}

// --- 3. Extreme Test: The 65535 Crash (Rollover) ---
void test_seq_num_rollover() {
    rudp_context_s ctx;
    tfv_packet_u dummy;
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

    // Validate packet 1 (the 5th packet sent) by sending ACK=2 (N+1)
    assert(rudp_recv_ack(&ctx, 2) == 0);

    // The signed logic (int16_t) understands that 2 is AFTER 65535
    // The tail must therefore have caught up with the head
    assert(ctx.tail == 5);

    printf("[EXTREME OK] Sequence Number Rollover (65535 -> 0 comparison is safe with N+1)\n");
}

// --- 4. Extreme Test: Buffer Full and Memory Wrap-Around ---
void test_buffer_full_and_wrap() {
    rudp_context_s ctx;
    tfv_packet_u dummy;
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

    // Free the first half (packets 0 to 31) with ACK=32 (N+1)
    assert(rudp_recv_ack(&ctx, 32) == 0);
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

// --- 5. Wire Serialization & Portability Test (Pack / Unpack) ---
void test_wire_serialization() {
    rudp_frame_s original_frame;
    original_frame.header.seq_num = 0x1234;
    original_frame.header.ack     = 0x5678;
    original_frame.packet.type    = 42;
    original_frame.packet.flags   = 0x0F;
    original_frame.packet.value   = 0xABCD;

    uint8_t wire_buffer[RUDP_WIRE_FRAME_SIZE];

    // 1. Test de l'empaquetage (Pack)
    int written = rudp_pack_frame(&original_frame, wire_buffer, sizeof(wire_buffer));
    assert(written == RUDP_WIRE_FRAME_SIZE);

    // 2. Vérification binaire exacte sur le câble (Big-Endian)
    assert(wire_buffer[0] == 0x12 && wire_buffer[1] == 0x34); // seq_num
    assert(wire_buffer[2] == 0x56 && wire_buffer[3] == 0x78); // ack
    assert(wire_buffer[4] == 42);                             // type
    assert(wire_buffer[5] == 0x0F);                           // flags
    assert(wire_buffer[6] == 0xAB && wire_buffer[7] == 0xCD); // value

    // 3. Test du dépaquetage (Unpack)
    rudp_frame_s restored_frame;
    assert(rudp_unpack_frame(wire_buffer, sizeof(wire_buffer), &restored_frame) == 0);

    // Vérification de l'intégrité complète
    assert(restored_frame.header.seq_num == original_frame.header.seq_num);
    assert(restored_frame.header.ack     == original_frame.header.ack);
    assert(restored_frame.packet.type    == original_frame.packet.type);
    assert(restored_frame.packet.flags   == original_frame.packet.flags);
    assert(restored_frame.packet.value   == original_frame.packet.value);

    // 4. Tests de sécurité (Pointeurs NULL et tailles invalides)
    assert(rudp_pack_frame(NULL, wire_buffer, sizeof(wire_buffer)) == -1);
    assert(rudp_pack_frame(&original_frame, NULL, sizeof(wire_buffer)) == -1);
    assert(rudp_pack_frame(&original_frame, wire_buffer, 7) == -1); // Too small

    assert(rudp_unpack_frame(NULL, sizeof(wire_buffer), &restored_frame) == -1);
    assert(rudp_unpack_frame(wire_buffer, sizeof(wire_buffer), NULL) == -1);
    assert(rudp_unpack_frame(wire_buffer, 7, &restored_frame) == -1); // Too small

    printf("[OK] Wire Serialization & Security Limits (Pack/Unpack validated)\n");
}

// --- 6. Advanced Reliability Edge Cases Test ---
void test_reliability_edge_cases() {
    rudp_context_s ctx;
    tfv_packet_u dummy;
    dummy.raw = 0;
    uint16_t expired[64];

    rudp_init(&ctx);

    // --- A. Empty-window ACKs ---
    // At startup (0 packets sent), ACK=0 is valid (0 - 0 == 0), no-op
    assert(rudp_recv_ack(&ctx, 0) == 0);
    assert(ctx.head == 0 && ctx.tail == 0);
    // Future ACK=1 when nothing was sent must be rejected
    assert(rudp_recv_ack(&ctx, 1) == -1);

    // --- B. Timeout boundaries and anti-spam retransmission ---
    assert(rudp_send(&ctx, dummy, 1000) == 0); // Sent at t=1000, timeout=100ms
    assert(ctx.tx_buffer[0].timestamp == 1000);

    // Exact boundary at t=1100 (100ms elapsed, not strictly > 100): must NOT expire
    assert(rudp_tick(&ctx, 1100, 100, expired, 64) == 0);

    // At t=1101 (101ms elapsed): expired!
    assert(rudp_tick(&ctx, 1101, 100, expired, 64) == 1);
    assert(expired[0] == 0);
    assert(ctx.tx_buffer[0].timestamp == 1101); // Timestamp updated to prevent spam

    // Immediate tick 1ms later at t=1102: must NOT re-expire immediately
    assert(rudp_tick(&ctx, 1102, 100, expired, 64) == 0);

    // At t=1202 (101ms after retransmission): expires again
    assert(rudp_tick(&ctx, 1202, 100, expired, 64) == 1);

    // --- C. Stale / Duplicate ACKs ---
    // Acknowledge packet 0 with ACK=1 (N+1)
    assert(rudp_recv_ack(&ctx, 1) == 0);
    assert(ctx.tail == 1);

    // Receiving a stale duplicate ACK=1 (packet 0 already freed): safe no-op
    assert(rudp_recv_ack(&ctx, 1) == 0);
    assert(ctx.tail == 1);

    // --- D. Rollover ACK validation around 65535 -> 0 ---
    ctx.current_seq_num = 65535;
    assert(rudp_send(&ctx, dummy, 2000) == 0); // Sends packet 65535, current_seq_num becomes 0
    assert(ctx.current_seq_num == 0);

    // Valid ACK=0 acknowledges packet 65535 under N+1
    assert(rudp_recv_ack(&ctx, 0) == 0);

    // Future ACK=10 when current_seq_num is 0: rejected!
    assert(rudp_recv_ack(&ctx, 10) == -1);

    // --- E. Invalid tick arguments ---
    assert(rudp_tick(NULL, 1000, 100, expired, 64) == -1);
    assert(rudp_tick(&ctx, 1000, 100, NULL, 64) == -1);
    assert(rudp_tick(&ctx, 1000, 100, expired, 0) == -1);
    assert(rudp_tick(&ctx, 1000, 100, expired, -1) == -1);

    printf("[OK] Reliability Edge Cases (Timeout boundaries, anti-spam tick, stale ACKs, rollover rejection)\n");
}

// --- 7. Reception Engine (RX) & Full-Duplex Bi-Directional Test ---
void test_rx_and_full_duplex() {
    rudp_context_s alice_ctx;
    rudp_context_s bob_ctx;
    tfv_packet_u alice_msg;
    tfv_packet_u bob_msg;
    tfv_packet_u received_msg;

    alice_msg.type = 1;
    alice_msg.flags = 0xAA;
    alice_msg.value = 1234;

    bob_msg.type = 2;
    bob_msg.flags = 0xBB;
    bob_msg.value = 5678;

    assert(rudp_init(&alice_ctx) == 0);
    assert(rudp_init(&bob_ctx) == 0);

    // --- A. RX Basic In-Order Delivery ---
    // Alice sends packet 0 to Bob
    assert(rudp_send(&alice_ctx, alice_msg, 1000) == 0);
    assert(alice_ctx.tx_buffer[0].frame.header.seq_num == 0);
    assert(alice_ctx.tx_buffer[0].frame.header.ack == 0); // Bob hasn't sent anything yet

    // Bob receives Alice's packet 0
    rudp_frame_s wire_frame = alice_ctx.tx_buffer[0].frame;
    assert(rudp_recv(&bob_ctx, &wire_frame, &received_msg) == 1); // 1 = New packet delivered!
    assert(received_msg.type == 1 && received_msg.flags == 0xAA && received_msg.value == 1234);
    assert(bob_ctx.expected_seq_num == 1); // Bob is now expecting packet 1

    // --- B. Duplicate Packet Discarding ---
    // Alice's packet 0 is duplicated over network and arrives at Bob again
    assert(rudp_recv(&bob_ctx, &wire_frame, &received_msg) == 0); // 0 = Duplicate discarded!
    assert(bob_ctx.expected_seq_num == 1); // Bob's expectation does not advance

    // --- C. Out-of-Order / Future Packet Discarding ---
    rudp_frame_s future_frame = wire_frame;
    future_frame.header.seq_num = 10; // Future packet 10
    assert(rudp_recv(&bob_ctx, &future_frame, &received_msg) == 0); // 0 = Ignored (waiting for 1)
    assert(bob_ctx.expected_seq_num == 1);

    // --- D. Full-Duplex Automatic ACK Piggybacking ---
    // Bob sends packet 0 to Alice (his frame will automatically piggyback ack = 1)
    assert(rudp_send(&bob_ctx, bob_msg, 1050) == 0);
    assert(bob_ctx.tx_buffer[0].frame.header.seq_num == 0);
    assert(bob_ctx.tx_buffer[0].frame.header.ack == 1); // Automatic piggyback verified!

    // Alice receives Bob's frame
    assert(alice_ctx.tail == 0); // Alice's packet 0 was in-flight
    assert(rudp_recv(&alice_ctx, &bob_ctx.tx_buffer[0].frame, &received_msg) == 1);
    assert(received_msg.type == 2 && received_msg.flags == 0xBB && received_msg.value == 5678);
    assert(alice_ctx.expected_seq_num == 1);

    // Alice's in-flight packet 0 has been AUTOMATICALLY acknowledged and freed by Bob's piggybacked ACK!
    assert(alice_ctx.tail == 1);
    assert(alice_ctx.tx_buffer[0].state == RUDP_SLOT_FREE);

    // --- E. Error Handling ---
    assert(rudp_recv(NULL, &wire_frame, &received_msg) == -1);
    assert(rudp_recv(&alice_ctx, NULL, &received_msg) == -1);
    assert(rudp_recv(&alice_ctx, &wire_frame, NULL) == -1);

    printf("[OK] Reception Engine & Full-Duplex (In-Order delivery, duplicate drop, auto-piggybacked ACK)\n");
}

// --- 8. Fast Retransmit (Tri-ACK) Test ---
void test_fast_retransmit_tri_ack() {
    rudp_context_s ctx;
    tfv_packet_u dummy;
    dummy.raw = 0;
    uint16_t expired[64];

    assert(rudp_init(&ctx) == 0);

    // Send 5 packets (seq 0 to 4) at t=1000ms with a 100ms timeout
    for (int i = 0; i < 5; i++) {
        assert(rudp_send(&ctx, dummy, 1000) == 0);
    }
    assert(ctx.head == 5 && ctx.tail == 0);
    assert(ctx.tx_buffer[0].timestamp == 1000);

    // Under normal circumstances at t=1020ms (only 20ms elapsed), rudp_tick would return 0
    assert(rudp_tick(&ctx, 1020, 100, expired, 64) == 0);

    // 1st Duplicate ACK (ACK=0 arrives: receiver still waiting for 0)
    assert(rudp_recv_ack(&ctx, 0) == 0);
    assert(ctx.duplicate_ack_count == 1);
    assert(ctx.tx_buffer[0].timestamp == 1000); // Not expired yet

    // 2nd Duplicate ACK
    assert(rudp_recv_ack(&ctx, 0) == 0);
    assert(ctx.duplicate_ack_count == 2);
    assert(ctx.tx_buffer[0].timestamp == 1000); // Not expired yet

    // 3rd Duplicate ACK: TRI-ACK TRIGGER!
    assert(rudp_recv_ack(&ctx, 0) == 0);
    assert(ctx.duplicate_ack_count == 3);
    assert(ctx.tx_buffer[0].timestamp == 0); // Forced timestamp to 0!

    // Immediate tick at t=1025ms (well before the 100ms timeout!):
    // The slot at tail (0) MUST be reported as expired for immediate retransmission!
    assert(rudp_tick(&ctx, 1025, 100, expired, 64) == 1);
    assert(expired[0] == 0);
    assert(ctx.tx_buffer[0].timestamp == 1025); // Timer reset for next cycle

    // When the valid ACK arrives (ACK=1 acknowledging packet 0):
    assert(rudp_recv_ack(&ctx, 1) == 0);
    assert(ctx.tail == 1);
    assert(ctx.duplicate_ack_count == 0); // Counter reset on window advance!

    printf("[OK] Fast Retransmit (Tri-ACK detected, immediate loss recovery triggered)\n");
}

// --- 9. Dead Peer Detection & Retransmission Limit Test ---
void test_dead_peer_detection() {
    rudp_context_s ctx;
    tfv_packet_u dummy;
    dummy.raw = 0;
    uint16_t expired[64];

    assert(rudp_init(&ctx) == 0);
    assert(ctx.state == RUDP_STATE_CONNECTED);

    // Send a packet at t=1000ms with a 100ms timeout
    assert(rudp_send(&ctx, dummy, 1000) == 0);
    assert(ctx.tx_buffer[0].retries == 0);

    uint32_t current_time = 1000;

    // Simulate 10 consecutive timeouts (RUDP_MAX_RETRIES) without receiving an ACK
    for (int retry = 1; retry <= RUDP_MAX_RETRIES; retry++) {
        current_time += 110; // Advance past the 100ms timeout
        assert(rudp_tick(&ctx, current_time, 100, expired, 64) == 1);
        assert(ctx.tx_buffer[0].retries == retry);
        assert(ctx.state == RUDP_STATE_CONNECTED); // Still connected while retrying
    }

    // 11th timeout: Exceeds RUDP_MAX_RETRIES -> Dead peer declared!
    current_time += 110;
    assert(rudp_tick(&ctx, current_time, 100, expired, 64) == -1); // Fatal error returned
    assert(ctx.state == RUDP_STATE_DISCONNECTED);                  // State switched to DISCONNECTED

    printf("[OK] Dead Peer Detection (Exceeding max retries triggers RUDP_STATE_DISCONNECTED)\n");
}

int main(void) {
    printf("--- MEMORY SIZE TESTS ---\n");
    printf("Header Size  : %zu bytes\n", sizeof(rudp_header_s));
    printf("Frame Size   : %zu bytes\n", sizeof(rudp_frame_s));
    printf("Slot Size    : %zu bytes\n", sizeof(rudp_slot_s));
    printf("Context Size : %zu bytes\n\n", sizeof(rudp_context_s));

    printf("--- RUDP ENGINE TESTS ---\n");
    test_happy_path();
    test_cumulative_ack();
    test_seq_num_rollover();
    test_buffer_full_and_wrap();
    test_wire_serialization();
    test_reliability_edge_cases();
    test_rx_and_full_duplex();
    test_fast_retransmit_tri_ack();
    test_dead_peer_detection();

    printf("\n>>> ALL TESTS PASSED SUCCESSFULLY! <<<\n");

    return 0;
}
