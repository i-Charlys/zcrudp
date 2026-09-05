#include "protocol_rudp.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

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
    assert(rudp_recv_ack(&ctx, 50) == RUDP_ERR_OUT_OF_WINDOW);

    // Tick before expiration (100ms timeout)
    rudp_tick_result_s res = rudp_tick(&ctx, 1050, 100, expired_slots, 64);
    assert(res.count == 0 && res.status == RUDP_OK);

    // Tick after expiration
    res = rudp_tick(&ctx, 1150, 100, expired_slots, 64);
    assert(res.count == 1 && res.status == RUDP_OK);
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
    assert(rudp_send(&ctx, dummy, 1000) == RUDP_ERR_BUFFER_FULL);

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

    // 4. Tests de l'ACK Standalone (Tier 1 : 4 octets)
    uint8_t ack_wire_buffer[RUDP_WIRE_HEADER_SIZE];
    int ack_written = rudp_pack_ack(0x5678, ack_wire_buffer, sizeof(ack_wire_buffer));
    assert(ack_written == RUDP_WIRE_HEADER_SIZE);
    assert(ack_wire_buffer[0] == 0x00 && ack_wire_buffer[1] == 0x00); // seq_num is 0
    assert(ack_wire_buffer[2] == 0x56 && ack_wire_buffer[3] == 0x78); // ack is Big-Endian 0x5678

    // 5. Test du déballage de header (rudp_unpack_header)
    rudp_header_s standalone_header;
    assert(rudp_unpack_header(ack_wire_buffer, sizeof(ack_wire_buffer), &standalone_header) == RUDP_OK);
    assert(standalone_header.seq_num == 0);
    assert(standalone_header.ack == 0x5678);

    // 6. Test du déballage direct d'ACK (rudp_unpack_ack)
    uint16_t extracted_ack = 0;
    assert(rudp_unpack_ack(ack_wire_buffer, sizeof(ack_wire_buffer), &extracted_ack) == RUDP_OK);
    assert(extracted_ack == 0x5678);

    // 7. Tests atomiques de payload (rudp_pack_payload, rudp_unpack_payload)
    uint8_t payload_wire[sizeof(tfv_packet_u)];
    tfv_packet_u orig_p;
    orig_p.type = 99;
    orig_p.flags = 0xAA;
    orig_p.value = 0x1234;
    assert(rudp_pack_payload(&orig_p, payload_wire, sizeof(payload_wire)) == (int)sizeof(tfv_packet_u));
    assert(payload_wire[0] == 99 && payload_wire[1] == 0xAA);
    assert(payload_wire[2] == 0x12 && payload_wire[3] == 0x34);

    tfv_packet_u restored_p;
    assert(rudp_unpack_payload(payload_wire, sizeof(payload_wire), &restored_p) == RUDP_OK);
    assert(restored_p.type == 99 && restored_p.flags == 0xAA && restored_p.value == 0x1234);

    // 8. Tests de sécurité (Pointeurs NULL et tailles invalides)
    assert(rudp_pack_header(NULL, wire_buffer, sizeof(wire_buffer)) == RUDP_ERR_INVALID_ARG);
    assert(rudp_pack_header(&original_frame.header, NULL, sizeof(wire_buffer)) == RUDP_ERR_INVALID_ARG);
    assert(rudp_pack_header(&original_frame.header, wire_buffer, 3) == RUDP_ERR_INVALID_ARG);

    assert(rudp_pack_payload(NULL, payload_wire, sizeof(payload_wire)) == RUDP_ERR_INVALID_ARG);
    assert(rudp_pack_payload(&orig_p, NULL, sizeof(payload_wire)) == RUDP_ERR_INVALID_ARG);
    assert(rudp_pack_payload(&orig_p, payload_wire, 3) == RUDP_ERR_INVALID_ARG);

    assert(rudp_unpack_payload(NULL, sizeof(payload_wire), &restored_p) == RUDP_ERR_INVALID_ARG);
    assert(rudp_unpack_payload(payload_wire, sizeof(payload_wire), NULL) == RUDP_ERR_INVALID_ARG);
    assert(rudp_unpack_payload(payload_wire, 3, &restored_p) == RUDP_ERR_INVALID_ARG);

    assert(rudp_pack_frame(NULL, wire_buffer, sizeof(wire_buffer)) == RUDP_ERR_INVALID_ARG);
    assert(rudp_pack_frame(&original_frame, NULL, sizeof(wire_buffer)) == RUDP_ERR_INVALID_ARG);
    assert(rudp_pack_frame(&original_frame, wire_buffer, 7) == RUDP_ERR_INVALID_ARG); // Too small

    assert(rudp_pack_ack(10, NULL, sizeof(ack_wire_buffer)) == RUDP_ERR_INVALID_ARG);
    assert(rudp_pack_ack(10, ack_wire_buffer, 3) == RUDP_ERR_INVALID_ARG); // Too small

    assert(rudp_unpack_header(NULL, sizeof(ack_wire_buffer), &standalone_header) == RUDP_ERR_INVALID_ARG);
    assert(rudp_unpack_header(ack_wire_buffer, sizeof(ack_wire_buffer), NULL) == RUDP_ERR_INVALID_ARG);
    assert(rudp_unpack_header(ack_wire_buffer, 3, &standalone_header) == RUDP_ERR_INVALID_ARG); // Too small

    assert(rudp_unpack_ack(NULL, sizeof(ack_wire_buffer), &extracted_ack) == RUDP_ERR_INVALID_ARG);
    assert(rudp_unpack_ack(ack_wire_buffer, sizeof(ack_wire_buffer), NULL) == RUDP_ERR_INVALID_ARG);
    assert(rudp_unpack_ack(ack_wire_buffer, 3, &extracted_ack) == RUDP_ERR_INVALID_ARG); // Too small

    assert(rudp_unpack_frame(NULL, sizeof(wire_buffer), &restored_frame) == RUDP_ERR_INVALID_ARG);
    assert(rudp_unpack_frame(wire_buffer, sizeof(wire_buffer), NULL) == RUDP_ERR_INVALID_ARG);
    assert(rudp_unpack_frame(wire_buffer, 7, &restored_frame) == RUDP_ERR_INVALID_ARG); // Too small

    // 9. Test de l'accesseur de slot (rudp_get_slot_frame)
    rudp_context_s dummy_ctx;
    rudp_init(&dummy_ctx);
    dummy_ctx.tx_buffer[0].frame = original_frame;
    const rudp_frame_s *slot_frame = rudp_get_slot_frame(&dummy_ctx, 0);
    assert(slot_frame != NULL);
    assert(slot_frame->header.seq_num == 0x1234);
    assert(rudp_get_slot_frame(NULL, 0) == NULL);
    assert(rudp_get_slot_frame(&dummy_ctx, RUDP_WINDOW_SIZE) == NULL); // Out of bounds

    printf("[OK] Wire Serialization & Modular Architecture (Pack/Unpack Header, Payload, Frame & Slot Accessor validated)\n");
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
    assert(rudp_recv_ack(&ctx, 0) == RUDP_OK);
    assert(ctx.head == 0 && ctx.tail == 0);
    // Future ACK=1 when nothing was sent must be rejected
    assert(rudp_recv_ack(&ctx, 1) == RUDP_ERR_OUT_OF_WINDOW);

    // --- B. Timeout boundaries and anti-spam retransmission ---
    assert(rudp_send(&ctx, dummy, 1000) == RUDP_OK); // Sent at t=1000, timeout=100ms
    assert(ctx.tx_buffer[0].timestamp == 1000);

    // Exact boundary at t=1100 (100ms elapsed, not strictly > 100): must NOT expire
    assert(rudp_tick(&ctx, 1100, 100, expired, 64).count == 0);

    // At t=1101 (101ms elapsed): expired!
    rudp_tick_result_s res_tick = rudp_tick(&ctx, 1101, 100, expired, 64);
    assert(res_tick.count == 1 && res_tick.status == RUDP_OK);
    assert(expired[0] == 0);
    assert(ctx.tx_buffer[0].timestamp == 1101); // Timestamp updated to prevent spam

    // Immediate tick 1ms later at t=1102: must NOT re-expire immediately
    assert(rudp_tick(&ctx, 1102, 100, expired, 64).count == 0);

    // At t=1202 (101ms after retransmission): expires again
    assert(rudp_tick(&ctx, 1202, 100, expired, 64).count == 1);

    // --- C. Stale / Duplicate ACKs ---
    // Acknowledge packet 0 with ACK=1 (N+1)
    assert(rudp_recv_ack(&ctx, 1) == RUDP_OK);
    assert(ctx.tail == 1);

    // Receiving a stale duplicate ACK=1 (packet 0 already freed): safe no-op
    assert(rudp_recv_ack(&ctx, 1) == RUDP_OK);
    assert(ctx.tail == 1);

    // --- D. Rollover ACK validation around 65535 -> 0 ---
    ctx.current_seq_num = 65535;
    assert(rudp_send(&ctx, dummy, 2000) == RUDP_OK); // Sends packet 65535, current_seq_num becomes 0
    assert(ctx.current_seq_num == 0);

    // Valid ACK=0 acknowledges packet 65535 under N+1
    assert(rudp_recv_ack(&ctx, 0) == RUDP_OK);

    // Future ACK=10 when current_seq_num is 0: rejected!
    assert(rudp_recv_ack(&ctx, 10) == RUDP_ERR_OUT_OF_WINDOW);

    // --- E. Invalid tick arguments ---
    assert(rudp_tick(NULL, 1000, 100, expired, 64).status == RUDP_ERR_INVALID_ARG);
    assert(rudp_tick(&ctx, 1000, 100, NULL, 64).status == RUDP_ERR_INVALID_ARG);
    assert(rudp_tick(&ctx, 1000, 100, expired, 0).status == RUDP_ERR_INVALID_ARG);
    assert(rudp_tick(&ctx, 1000, 100, expired, -1).status == RUDP_ERR_INVALID_ARG);

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
    assert(rudp_recv(NULL, &wire_frame, &received_msg) == RUDP_ERR_INVALID_ARG);
    assert(rudp_recv(&alice_ctx, NULL, &received_msg) == RUDP_ERR_INVALID_ARG);
    assert(rudp_recv(&alice_ctx, &wire_frame, NULL) == RUDP_ERR_INVALID_ARG);

    printf("[OK] Reception Engine & Full-Duplex (In-Order delivery, duplicate drop, auto-piggybacked ACK)\n");
}

// --- 8. Fast Retransmit (Tri-ACK) Test ---
void test_fast_retransmit_tri_ack() {
    rudp_context_s ctx;
    tfv_packet_u dummy;
    dummy.raw = 0;
    uint16_t expired[64];

    assert(rudp_init(&ctx) == RUDP_OK);

    // Send 5 packets (seq 0 to 4) at t=1000ms with a 100ms timeout
    for (int i = 0; i < 5; i++) {
        assert(rudp_send(&ctx, dummy, 1000) == RUDP_OK);
    }
    assert(ctx.head == 5 && ctx.tail == 0);
    assert(ctx.tx_buffer[0].timestamp == 1000);

    // Under normal circumstances at t=1020ms (only 20ms elapsed), rudp_tick would return 0
    assert(rudp_tick(&ctx, 1020, 100, expired, 64).count == 0);

    // Initial ACK 0 arrives (1st observation: baseline reference recorded)
    assert(rudp_recv_ack(&ctx, 0) == RUDP_OK);
    assert(ctx.duplicate_ack_count == 0);
    assert(ctx.tx_buffer[0].timestamp == 1000);

    // 1st Duplicate ACK
    assert(rudp_recv_ack(&ctx, 0) == RUDP_OK);
    assert(ctx.duplicate_ack_count == 1);
    assert(ctx.tx_buffer[0].timestamp == 1000);

    // 2nd Duplicate ACK
    assert(rudp_recv_ack(&ctx, 0) == RUDP_OK);
    assert(ctx.duplicate_ack_count == 2);
    assert(ctx.tx_buffer[0].timestamp == 1000);

    // 3rd Duplicate ACK: TRI-ACK TRIGGER!
    assert(rudp_recv_ack(&ctx, 0) == RUDP_OK);
    assert(ctx.duplicate_ack_count == 3);
    assert(ctx.tx_buffer[0].fast_retransmit == RUDP_FAST_RETRANSMIT_PENDING); // Explicit flag set!

    // 4th Duplicate ACK: Anti-storm check (should NOT re-trigger if already cleared)
    ctx.tx_buffer[0].fast_retransmit = RUDP_FAST_RETRANSMIT_OFF;
    assert(rudp_recv_ack(&ctx, 0) == RUDP_OK);
    assert(ctx.duplicate_ack_count == 4);
    assert(ctx.tx_buffer[0].fast_retransmit == RUDP_FAST_RETRANSMIT_OFF); // Anti-storm verified (not re-triggered)!

    // Simulate incoming data having moved our expected_seq_num to 42
    ctx.expected_seq_num = 42;
    ctx.tx_buffer[0].fast_retransmit = RUDP_FAST_RETRANSMIT_PENDING; // Restore flag for test

    // Immediate tick at t=1025ms (well before the 100ms timeout!):
    rudp_tick_result_s tri_res = rudp_tick(&ctx, 1025, 100, expired, 64);
    assert(tri_res.count == 1 && tri_res.status == RUDP_OK);
    assert(expired[0] == 0);
    assert(ctx.tx_buffer[0].fast_retransmit == RUDP_FAST_RETRANSMIT_OFF); // Flag cleared after tick
    assert(ctx.tx_buffer[0].frame.header.ack == 42); // Header ACK dynamically refreshed!
    assert(ctx.tx_buffer[0].timestamp == 1025); // Timer reset for next cycle

    // When the valid ACK arrives (ACK=1 acknowledging packet 0):
    assert(rudp_recv_ack(&ctx, 1) == RUDP_OK);
    assert(ctx.tail == 1);
    assert(ctx.duplicate_ack_count == 0); // Counter reset on window advance!

    printf("[OK] Fast Retransmit (Tri-ACK flag, anti-storm lock, and dynamic ACK refresh verified)\n");
}

// --- 9. Dead Peer Detection & Retransmission Limit Test ---
void test_dead_peer_detection() {
    rudp_context_s ctx;
    tfv_packet_u dummy;
    dummy.raw = 0;
    uint16_t expired[64];

    assert(rudp_init(&ctx) == RUDP_OK);
    assert(ctx.state == RUDP_STATE_CONNECTED);

    // Send a packet at t=1000ms with a 100ms timeout
    assert(rudp_send(&ctx, dummy, 1000) == RUDP_OK);
    assert(ctx.tx_buffer[0].retries == 0);

    uint32_t current_time = 1000;

    // Simulate 10 consecutive timeouts (RUDP_MAX_RETRIES) without receiving an ACK
    for (int retry = 1; retry <= RUDP_MAX_RETRIES; retry++) {
        current_time += 110; // Advance past the 100ms timeout
        rudp_tick_result_s tick_res = rudp_tick(&ctx, current_time, 100, expired, 64);
        assert(tick_res.count == 1 && tick_res.status == RUDP_OK);
        assert(ctx.tx_buffer[0].retries == retry);
        assert(ctx.state == RUDP_STATE_CONNECTED); // Still connected while retrying
    }

    // 11th timeout: Exceeds RUDP_MAX_RETRIES -> Dead peer declared!
    current_time += 110;
    rudp_tick_result_s dead_res = rudp_tick(&ctx, current_time, 100, expired, 64);
    assert(dead_res.status == RUDP_ERR_DISCONNECTED); // Dead peer error returned
    assert(ctx.state == RUDP_STATE_DISCONNECTED);      // State switched to DISCONNECTED

    // Test Zombie prevention: subsequent ticks are inert and return RUDP_ERR_DISCONNECTED immediately
    assert(rudp_tick(&ctx, current_time + 110, 100, expired, 64).status == RUDP_ERR_DISCONNECTED);
    // Test send rejection on disconnected context
    assert(rudp_send(&ctx, dummy, current_time + 110) == RUDP_ERR_DISCONNECTED);

    // Test Preservation of collected indices when slot trips limit:
    assert(rudp_reset(&ctx) == RUDP_OK);
    assert(rudp_send(&ctx, dummy, 1000) == RUDP_OK); // Slot 0
    assert(rudp_send(&ctx, dummy, 1000) == RUDP_OK); // Slot 1
    ctx.tx_buffer[0].retries = 3;                  // Slot 0 expired but below limit
    ctx.tx_buffer[1].retries = RUDP_MAX_RETRIES;   // Slot 1 will trip on next tick
    rudp_tick_result_s multi_dead = rudp_tick(&ctx, 1200, 100, expired, 64);
    assert(multi_dead.status == RUDP_ERR_DISCONNECTED);
    assert(multi_dead.count == 1);                 // Slot 0 was preserved in out_indices!
    assert(expired[0] == 0);                       // Slot 0 index preserved!

    // Test rudp_reset restores healthy connection
    assert(rudp_reset(&ctx) == RUDP_OK);
    assert(ctx.state == RUDP_STATE_CONNECTED);
    assert(rudp_send(&ctx, dummy, current_time + 110) == RUDP_OK);

    // Test rudp_get_unacked_slots accessor
    uint16_t unacked[64];
    assert(rudp_get_unacked_slots(NULL, unacked, 64) == RUDP_ERR_INVALID_ARG);
    assert(rudp_get_unacked_slots(&ctx, NULL, 64) == RUDP_ERR_INVALID_ARG);
    assert(rudp_get_unacked_slots(&ctx, unacked, 0) == RUDP_ERR_INVALID_ARG);
    assert(rudp_get_unacked_slots(&ctx, unacked, 64) == 1); // 1 packet in flight from line 487
    assert(unacked[0] == 0);

    printf("[OK] Dead Peer & Zombie Prevention (Preserve in-flight slots on dead peer & rudp_reset validated)\n");
}

// --- 10. Multi-Channel Session Architecture Test ---
void test_multi_channel_session(void) {
    rudp_session_s session;

    assert(rudp_session_init(NULL) == RUDP_ERR_INVALID_ARG);
    assert(rudp_session_init(&session) == RUDP_OK);
    assert(session.active_channels == 0);

    // Channel 0: Unreliable movement
    assert(rudp_session_config_channel(&session, 0, 0) == RUDP_OK);
    assert(session.channels[0].flags == 0);
    assert(session.active_channels == 1);

    // Channel 1: High-priority reliable actions
    assert(rudp_session_config_channel(&session, 1, RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED) == RUDP_OK);
    assert(session.channels[1].flags == (RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED));
    assert(session.active_channels == 2);

    // Channel 2: Encrypted reliable inventory
    assert(rudp_session_config_channel(&session, 2, RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ENCRYPTED) == RUDP_OK);
    assert((session.channels[2].flags & RUDP_CHANNEL_FLAG_ENCRYPTED) != 0);
    assert(session.active_channels == 3);

    // Out-of-bounds channel configuration
    assert(rudp_session_config_channel(&session, RUDP_MAX_CHANNELS, 0) == RUDP_ERR_INVALID_ARG);

    printf("[OK] Multi-Channel Session (Session Init, Capability Flags & Bounds Check validated)\n");
}

// --- 11. Datagram & Record Wire Serialization Test ---
void test_datagram_wire_serialization(void) {
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));

    // A. Datagram Header Packing and Unpacking
    rudp_datagram_header_s hdr_tx = {
        .ack = 0x1234,
        .ack_channel = 1,
        .count = 2
    };

    assert(rudp_pack_datagram_header(NULL, buf, sizeof(buf)) == RUDP_ERR_INVALID_ARG);
    assert(rudp_pack_datagram_header(&hdr_tx, NULL, sizeof(buf)) == RUDP_ERR_INVALID_ARG);
    assert(rudp_pack_datagram_header(&hdr_tx, buf, 3) == RUDP_ERR_INVALID_ARG);

    rudp_datagram_header_s invalid_hdr = hdr_tx;
    invalid_hdr.ack_channel = RUDP_MAX_CHANNELS;
    assert(rudp_pack_datagram_header(&invalid_hdr, buf, sizeof(buf)) == RUDP_ERR_INVALID_ARG);

    int pack_len = rudp_pack_datagram_header(&hdr_tx, buf, sizeof(buf));
    assert(pack_len == RUDP_WIRE_DATAGRAM_HEADER_SIZE);
    assert(buf[0] == 0x12);
    assert(buf[1] == 0x34);
    assert(buf[2] == 1);
    assert(buf[3] == 2);

    rudp_datagram_header_s hdr_rx;
    assert(rudp_unpack_datagram_header(NULL, sizeof(buf), &hdr_rx) == RUDP_ERR_INVALID_ARG);
    assert(rudp_unpack_datagram_header(buf, sizeof(buf), NULL) == RUDP_ERR_INVALID_ARG);
    assert(rudp_unpack_datagram_header(buf, 3, &hdr_rx) == RUDP_ERR_INVALID_ARG);

    assert(rudp_unpack_datagram_header(buf, sizeof(buf), &hdr_rx) == RUDP_OK);
    assert(hdr_rx.ack == 0x1234);
    assert(hdr_rx.ack_channel == 1);
    assert(hdr_rx.count == 2);

    // B. Record Packing and Unpacking
    rudp_record_s rec_tx = {
        .channel_id = 2,
        .flags = RUDP_RECORD_FLAG_RELIABLE,
        .seq_num = 0xABCD,
        .payload = { .type = 42, .flags = 7, .value = 0x5678 }
    };

    memset(buf, 0, sizeof(buf));
    assert(rudp_pack_record(NULL, buf, sizeof(buf)) == RUDP_ERR_INVALID_ARG);
    assert(rudp_pack_record(&rec_tx, NULL, sizeof(buf)) == RUDP_ERR_INVALID_ARG);
    assert(rudp_pack_record(&rec_tx, buf, 7) == RUDP_ERR_INVALID_ARG);

    rudp_record_s invalid_rec = rec_tx;
    invalid_rec.channel_id = RUDP_MAX_CHANNELS;
    assert(rudp_pack_record(&invalid_rec, buf, sizeof(buf)) == RUDP_ERR_INVALID_ARG);

    pack_len = rudp_pack_record(&rec_tx, buf, sizeof(buf));
    assert(pack_len == RUDP_WIRE_RECORD_SIZE);
    assert(buf[0] == 2);
    assert(buf[1] == RUDP_RECORD_FLAG_RELIABLE);
    assert(buf[2] == 0xAB);
    assert(buf[3] == 0xCD);
    assert(buf[4] == 42);
    assert(buf[5] == 7);
    assert(buf[6] == 0x56);
    assert(buf[7] == 0x78);

    rudp_record_s rec_rx;
    assert(rudp_unpack_record(NULL, sizeof(buf), &rec_rx) == RUDP_ERR_INVALID_ARG);
    assert(rudp_unpack_record(buf, sizeof(buf), NULL) == RUDP_ERR_INVALID_ARG);
    assert(rudp_unpack_record(buf, 7, &rec_rx) == RUDP_ERR_INVALID_ARG);

    assert(rudp_unpack_record(buf, sizeof(buf), &rec_rx) == RUDP_OK);
    assert(rec_rx.channel_id == 2);
    assert(rec_rx.flags == RUDP_RECORD_FLAG_RELIABLE);
    assert(rec_rx.seq_num == 0xABCD);
    assert(rec_rx.payload.type == 42);
    assert(rec_rx.payload.flags == 7);
    assert(rec_rx.payload.value == 0x5678);

    printf("[OK] Datagram & Record Wire Serialization (Header 4B, Record 8B Big-Endian verified)\n");
}

// --- 12. Datagram Bundling & Bounded Validation Test ---
void test_datagram_bundling_and_validation(void) {
    uint8_t buf[128];
    memset(buf, 0, sizeof(buf));

    // A. Standalone ACK Datagram (count == 0, exactly 4 bytes)
    rudp_datagram_header_s ack_hdr = {
        .ack = 999,
        .ack_channel = 0,
        .count = 0
    };
    int hdr_len = rudp_pack_datagram_header(&ack_hdr, buf, sizeof(buf));
    assert(hdr_len == 4);

    rudp_datagram_header_s parsed_hdr;
    assert(rudp_unpack_datagram(buf, 4, &parsed_hdr, NULL, 0) == RUDP_OK);
    assert(parsed_hdr.ack == 999);
    assert(parsed_hdr.ack_channel == 0);
    assert(parsed_hdr.count == 0);

    // Exact length mismatch: 3 bytes or 5 bytes must fail
    assert(rudp_unpack_datagram(buf, 3, &parsed_hdr, NULL, 0) == RUDP_ERR_INVALID_ARG);
    assert(rudp_unpack_datagram(buf, 5, &parsed_hdr, NULL, 0) == RUDP_ERR_INVALID_ARG);

    // B. Bundled Datagram (count == 3: 1 header + 3 records = 28 bytes)
    rudp_datagram_header_s bundle_hdr = {
        .ack = 50,
        .ack_channel = 1,
        .count = 3
    };
    assert(rudp_pack_datagram_header(&bundle_hdr, buf, sizeof(buf)) == 4);

    rudp_record_s recs_in[3];
    for (int i = 0; i < 3; i++) {
        recs_in[i].channel_id = (uint8_t)i;
        recs_in[i].flags = (i == 0) ? RUDP_RECORD_FLAG_RELIABLE : RUDP_RECORD_FLAG_UNRELIABLE;
        recs_in[i].seq_num = (uint16_t)(100 + i);
        recs_in[i].payload.type = (uint8_t)(10 + i);
        recs_in[i].payload.flags = 0;
        recs_in[i].payload.value = (uint16_t)(1000 + i);
        assert(rudp_pack_record(&recs_in[i], buf + 4 + i * 8, sizeof(buf) - (4 + i * 8)) == 8);
    }
    size_t total_datagram_len = 4 + 3 * 8; // 28 bytes

    // Buffer length bounds checks
    rudp_record_s recs_out[3];
    assert(rudp_unpack_datagram(buf, total_datagram_len - 1, &parsed_hdr, recs_out, 3) == RUDP_ERR_INVALID_ARG);
    assert(rudp_unpack_datagram(buf, total_datagram_len + 1, &parsed_hdr, recs_out, 3) == RUDP_ERR_INVALID_ARG);
    assert(rudp_unpack_datagram(buf, total_datagram_len, &parsed_hdr, recs_out, 2) == RUDP_ERR_INVALID_ARG); // max_records too small

    // Successful unpacking of all bundled records
    assert(rudp_unpack_datagram(buf, total_datagram_len, &parsed_hdr, recs_out, 3) == RUDP_OK);
    assert(parsed_hdr.count == 3);
    for (int i = 0; i < 3; i++) {
        assert(recs_out[i].channel_id == (uint8_t)i);
        assert(recs_out[i].flags == recs_in[i].flags);
        assert(recs_out[i].seq_num == (uint16_t)(100 + i));
        assert(recs_out[i].payload.type == (uint8_t)(10 + i));
        assert(recs_out[i].payload.value == (uint16_t)(1000 + i));
    }

    printf("[OK] Datagram Bundling & Bounded Validation (Exact wire length check: in_len == 4 + count * 8 verified)\n");
}

// --- 13. Unreliable Fast Bypass & 16-bit Anti-Rollback Test ---
void test_unreliable_fast_bypass_and_anti_rollback(void) {
    rudp_session_s tx_session;
    rudp_session_s rx_session;
    assert(rudp_session_init(&tx_session) == RUDP_OK);
    assert(rudp_session_init(&rx_session) == RUDP_OK);

    // Channel 1: Unreliable channel
    assert(rudp_session_config_channel(&tx_session, 1, RUDP_CHANNEL_FLAG_UNRELIABLE) == RUDP_OK);
    assert(rudp_session_config_channel(&rx_session, 1, RUDP_CHANNEL_FLAG_UNRELIABLE) == RUDP_OK);

    uint8_t wire_buf[64];
    tfv_packet_u pkt1 = { .type = 1, .flags = 0, .value = 111 };

    // 1. Fast Memory Bypass: Verify tx_buffer is not touched
    assert(tx_session.channels[1].ctx.head == 0);
    assert(tx_session.channels[1].ctx.tail == 0);
    assert(tx_session.channels[1].ctx.tx_buffer[0].state == RUDP_SLOT_FREE);

    int written = rudp_session_send_unreliable(&tx_session, 1, pkt1, 0, wire_buf, sizeof(wire_buf));
    assert(written == 12);

    // Sliding window buffer must STILL be completely free (zero-malloc / zero-slot bypass)
    assert(tx_session.channels[1].ctx.head == 0);
    assert(tx_session.channels[1].ctx.tail == 0);
    assert(tx_session.channels[1].ctx.tx_buffer[0].state == RUDP_SLOT_FREE);
    assert(tx_session.channels[1].next_unreliable_seq == 1);

    // Argument bounds checks
    assert(rudp_session_send_unreliable(NULL, 1, pkt1, 0, wire_buf, sizeof(wire_buf)) == RUDP_ERR_INVALID_ARG);
    assert(rudp_session_send_unreliable(&tx_session, 1, pkt1, 0, NULL, sizeof(wire_buf)) == RUDP_ERR_INVALID_ARG);
    assert(rudp_session_send_unreliable(&tx_session, RUDP_MAX_CHANNELS, pkt1, 0, wire_buf, sizeof(wire_buf)) == RUDP_ERR_INVALID_ARG);
    assert(rudp_session_send_unreliable(&tx_session, 1, pkt1, RUDP_MAX_CHANNELS, wire_buf, sizeof(wire_buf)) == RUDP_ERR_INVALID_ARG);
    assert(rudp_session_send_unreliable(&tx_session, 1, pkt1, 0, wire_buf, 11) == RUDP_ERR_INVALID_ARG);

    // 2. Anti-Rollback Sequence Filter Verification
    rudp_record_s delivered[4];

    // Packet 0 arrives: First unreliable packet -> accepted
    int del_count = rudp_session_process_datagram(&rx_session, wire_buf, (size_t)written, delivered, 4);
    assert(del_count == 1);
    assert(delivered[0].channel_id == 1);
    assert(delivered[0].seq_num == 0);
    assert(delivered[0].payload.value == 111);
    assert(rx_session.channels[1].has_unreliable_seq == 1);
    assert(rx_session.channels[1].last_unreliable_seq == 0);

    // Send Packet 1
    tfv_packet_u pkt2 = { .type = 1, .flags = 0, .value = 222 };
    written = rudp_session_send_unreliable(&tx_session, 1, pkt2, 0, wire_buf, sizeof(wire_buf));
    assert(written == 12);
    assert(tx_session.channels[1].next_unreliable_seq == 2);

    // Save copy of packet 1 bytes for duplicate testing
    uint8_t pkt1_copy[12];
    memcpy(pkt1_copy, wire_buf, 12);

    del_count = rudp_session_process_datagram(&rx_session, wire_buf, (size_t)written, delivered, 4);
    assert(del_count == 1);
    assert(delivered[0].seq_num == 1);
    assert(delivered[0].payload.value == 222);
    assert(rx_session.channels[1].last_unreliable_seq == 1);

    // Duplicate test: Re-send packet 1 -> must be dropped
    del_count = rudp_session_process_datagram(&rx_session, pkt1_copy, 12, delivered, 4);
    assert(del_count == 0);
    assert(rx_session.channels[1].last_unreliable_seq == 1);

    // Late arrival test: Craft packet 0 (older) -> must be dropped
    rudp_datagram_header_s hdr_old = { .ack = 0, .ack_channel = 0, .count = 1 };
    rudp_record_s rec_old = { .channel_id = 1, .flags = RUDP_RECORD_FLAG_UNRELIABLE, .seq_num = 0, .payload = pkt1 };
    rudp_pack_datagram_header(&hdr_old, wire_buf, sizeof(wire_buf));
    rudp_pack_record(&rec_old, wire_buf + 4, sizeof(wire_buf) - 4);
    del_count = rudp_session_process_datagram(&rx_session, wire_buf, 12, delivered, 4);
    assert(del_count == 0);
    assert(rx_session.channels[1].last_unreliable_seq == 1);

    // 3. RFC 1982 16-bit Sequence Rollover Test (65535 -> 0)
    rx_session.channels[1].last_unreliable_seq = 65535;

    // Packet 0 arrives after 65535: distance = (0 - 65535) = 1 (< 0x8000U) -> accepted!
    rudp_record_s rec_rollover = { .channel_id = 1, .flags = RUDP_RECORD_FLAG_UNRELIABLE, .seq_num = 0, .payload = pkt1 };
    rudp_pack_record(&rec_rollover, wire_buf + 4, sizeof(wire_buf) - 4);
    del_count = rudp_session_process_datagram(&rx_session, wire_buf, 12, delivered, 4);
    assert(del_count == 1);
    assert(delivered[0].seq_num == 0);
    assert(rx_session.channels[1].last_unreliable_seq == 0);

    // Stale 65535 arrives when last seen is 0: distance = (65535 - 0) = 65535 (>= 0x8000U) -> dropped!
    rec_rollover.seq_num = 65535;
    rudp_pack_record(&rec_rollover, wire_buf + 4, sizeof(wire_buf) - 4);
    del_count = rudp_session_process_datagram(&rx_session, wire_buf, 12, delivered, 4);
    assert(del_count == 0);
    assert(rx_session.channels[1].last_unreliable_seq == 0);

    printf("[OK] Unreliable Fast Bypass & 16-bit Anti-Rollback (Zero-malloc bypass & RFC 1982 filter validated)\n");
}

// --- 14. Multi-Channel Datagram Session Routing Test ---
void test_multi_channel_datagram_session(void) {
    rudp_session_s peer_a;
    rudp_session_s peer_b;
    assert(rudp_session_init(&peer_a) == RUDP_OK);
    assert(rudp_session_init(&peer_b) == RUDP_OK);

    // Channel 0: Reliable
    assert(rudp_session_config_channel(&peer_a, 0, RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED) == RUDP_OK);
    assert(rudp_session_config_channel(&peer_b, 0, RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED) == RUDP_OK);

    // Channel 1: Unreliable
    assert(rudp_session_config_channel(&peer_a, 1, RUDP_CHANNEL_FLAG_UNRELIABLE) == RUDP_OK);
    assert(rudp_session_config_channel(&peer_b, 1, RUDP_CHANNEL_FLAG_UNRELIABLE) == RUDP_OK);

    // Step 1: Peer A sends reliable message on Channel 0
    tfv_packet_u rel_pkt = { .type = 10, .flags = 0, .value = 1000 };
    assert(rudp_send(&peer_a.channels[0].ctx, rel_pkt, 1000) == RUDP_OK);
    assert(peer_a.channels[0].ctx.tx_buffer[0].state == RUDP_SLOT_IN_FLIGHT);

    // Step 2: Peer A bundles Reliable frame (chan 0) + Unreliable movement (chan 1) in one datagram
    uint8_t datagram[64];
    rudp_datagram_header_s d_hdr = {
        .ack = 0, // Cumulative ACK for peer_b's channel 0
        .ack_channel = 0,
        .count = 2
    };
    rudp_pack_datagram_header(&d_hdr, datagram, sizeof(datagram));

    rudp_record_s rec0 = {
        .channel_id = 0,
        .flags = RUDP_RECORD_FLAG_RELIABLE,
        .seq_num = 0,
        .payload = rel_pkt
    };
    rudp_pack_record(&rec0, datagram + 4, sizeof(datagram) - 4);

    tfv_packet_u unrec_pkt = { .type = 20, .flags = 0, .value = 2000 };
    rudp_record_s rec1 = {
        .channel_id = 1,
        .flags = RUDP_RECORD_FLAG_UNRELIABLE,
        .seq_num = peer_a.channels[1].next_unreliable_seq++,
        .payload = unrec_pkt
    };
    rudp_pack_record(&rec1, datagram + 12, sizeof(datagram) - 12);

    // Step 3: Peer B processes bundled datagram (20 bytes: 4B header + 2 * 8B records)
    rudp_record_s delivered[4];
    int count = rudp_session_process_datagram(&peer_b, datagram, 20, delivered, 4);
    assert(count == 2);

    // Check Channel 0 delivery (reliable in-order)
    assert(delivered[0].channel_id == 0);
    assert(delivered[0].flags == RUDP_RECORD_FLAG_RELIABLE);
    assert(delivered[0].seq_num == 0);
    assert(delivered[0].payload.value == 1000);
    assert(peer_b.channels[0].ctx.expected_seq_num == 1); // Channel 0 expected sequence advanced

    // Check Channel 1 delivery (unreliable)
    assert(delivered[1].channel_id == 1);
    assert(delivered[1].flags == RUDP_RECORD_FLAG_UNRELIABLE);
    assert(delivered[1].seq_num == 0);
    assert(delivered[1].payload.value == 2000);
    assert(peer_b.channels[1].last_unreliable_seq == 0);

    // Step 4: Peer B sends Standalone ACK datagram back to Peer A for Channel 0 (ack = 1, count = 0, 4 bytes)
    uint8_t ack_datagram[16];
    rudp_datagram_header_s ack_hdr = {
        .ack = peer_b.channels[0].ctx.expected_seq_num, // ack = 1 (N+1)
        .ack_channel = 0,
        .count = 0 // Pure Standalone ACK!
    };
    int ack_len = rudp_pack_datagram_header(&ack_hdr, ack_datagram, sizeof(ack_datagram));
    assert(ack_len == 4);

    // Step 5: Peer A receives Standalone ACK datagram
    assert(peer_a.channels[0].ctx.tx_buffer[0].state == RUDP_SLOT_IN_FLIGHT); // In-flight before ACK
    count = rudp_session_process_datagram(&peer_a, ack_datagram, 4, NULL, 0);
    assert(count == 0); // No payload records delivered
    assert(peer_a.channels[0].ctx.tx_buffer[0].state == RUDP_SLOT_FREE); // Slot freed by ACK!
    assert(peer_a.channels[0].ctx.tail == 1); // Window advanced!

    printf("[OK] Multi-Channel Datagram Session (Bundling, Dispatch, Standalone ACK & Free Window verified)\n");
}

int main(void) {
    printf("--- MEMORY SIZE TESTS ---\n");
    printf("Header Size  : %zu bytes\n", sizeof(rudp_header_s));
    printf("Frame Size   : %zu bytes\n", sizeof(rudp_frame_s));
    printf("Slot Size    : %zu bytes\n", sizeof(rudp_slot_s));
    printf("Datagram Hdr : %zu bytes\n", sizeof(rudp_datagram_header_s));
    printf("Record Size  : %zu bytes\n", sizeof(rudp_record_s));
    printf("Context Size : %zu bytes\n", sizeof(rudp_context_s));
    printf("Channel Size : %zu bytes\n", sizeof(rudp_channel_s));
    printf("Session Size : %zu bytes (at RUDP_MAX_CHANNELS=%u)\n\n", sizeof(rudp_session_s), RUDP_MAX_CHANNELS);

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
    test_multi_channel_session();
    test_datagram_wire_serialization();
    test_datagram_bundling_and_validation();
    test_unreliable_fast_bypass_and_anti_rollback();
    test_multi_channel_datagram_session();

    printf("\n>>> ALL TESTS PASSED SUCCESSFULLY! <<<\n");

    return 0;
}
