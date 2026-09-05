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

    // Exponential backoff: retry 1 doubles timeout to 200ms
    // At t=1202 (101ms after 1st retransmission): must NOT expire yet
    assert(rudp_tick(&ctx, 1202, 100, expired, 64).count == 0);

    // At t=1302 (201ms after 1st retransmission): expires again
    assert(rudp_tick(&ctx, 1302, 100, expired, 64).count == 1);

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

    // Simulate 10 consecutive timeouts (RUDP_MAX_RETRIES) with binary exponential backoff
    for (int retry = 1; retry <= RUDP_MAX_RETRIES; retry++) {
        uint32_t shift = (retry - 1 > 6) ? 6 : (retry - 1);
        current_time += (100 << shift) + 10; // Advance past the backed-off timeout
        rudp_tick_result_s tick_res = rudp_tick(&ctx, current_time, 100, expired, 64);
        assert(tick_res.count == 1 && tick_res.status == RUDP_OK);
        assert(ctx.tx_buffer[0].retries == retry);
        assert(ctx.state == RUDP_STATE_CONNECTED); // Still connected while retrying
    }

    // 11th timeout: Exceeds RUDP_MAX_RETRIES -> Dead peer declared!
    uint32_t shift = (RUDP_MAX_RETRIES > 6) ? 6 : RUDP_MAX_RETRIES;
    current_time += (100 << shift) + 10;
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
    // Both slots timed out past their backed-off deadlines at t = 10000ms
    rudp_tick_result_s multi_dead = rudp_tick(&ctx, 10000, 100, expired, 64);
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

// --- 15. Multi-Channel Explicit ACK Record Bundling (AI KV-Cache & Speculative Streaming) ---
void test_multi_channel_record_ack_bundling(void) {
    rudp_session_s prefill_node; // Sender of KV Chunks & Cluster Barrier
    rudp_session_s decode_node;  // Sender of Speculative Tokens & Multi-ACKs
    assert(rudp_session_init(&prefill_node) == RUDP_OK);
    assert(rudp_session_init(&decode_node) == RUDP_OK);

    // Channel 0: KV-Cache Attention Chunks (Reliable In-Order)
    assert(rudp_session_config_channel(&prefill_node, 0, RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED) == RUDP_OK);
    assert(rudp_session_config_channel(&decode_node, 0, RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED) == RUDP_OK);

    // Channel 1: Speculative Draft Tokens / Logits (Unreliable Bypass)
    assert(rudp_session_config_channel(&prefill_node, 1, RUDP_CHANNEL_FLAG_UNRELIABLE) == RUDP_OK);
    assert(rudp_session_config_channel(&decode_node, 1, RUDP_CHANNEL_FLAG_UNRELIABLE) == RUDP_OK);

    // Channel 2: Cluster Consensus / Barrier (Reliable In-Order)
    assert(rudp_session_config_channel(&prefill_node, 2, RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED) == RUDP_OK);
    assert(rudp_session_config_channel(&decode_node, 2, RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED) == RUDP_OK);

    // Step 1: Prefill node emits KV Chunk on Channel 0 AND Barrier on Channel 2
    tfv_packet_u kv_chunk = { .type = 1, .flags = 0, .value = 5555 };
    assert(rudp_send(&prefill_node.channels[0].ctx, kv_chunk, 100) == RUDP_OK);
    assert(prefill_node.channels[0].ctx.tx_buffer[0].state == RUDP_SLOT_IN_FLIGHT);

    tfv_packet_u barrier = { .type = 2, .flags = 0, .value = 9999 };
    assert(rudp_send(&prefill_node.channels[2].ctx, barrier, 100) == RUDP_OK);
    assert(prefill_node.channels[2].ctx.tx_buffer[0].state == RUDP_SLOT_IN_FLIGHT);

    // Step 2: Decode node receives both packets (via bundled datagram from prefill)
    uint8_t fwd_datagram[64];
    rudp_datagram_header_s fwd_hdr = { .ack = 0, .ack_channel = 0, .count = 2 };
    rudp_pack_datagram_header(&fwd_hdr, fwd_datagram, sizeof(fwd_datagram));

    rudp_record_s fwd_recs[2];
    fwd_recs[0].channel_id = 0;
    fwd_recs[0].flags = RUDP_RECORD_FLAG_RELIABLE;
    fwd_recs[0].seq_num = 0;
    fwd_recs[0].payload = kv_chunk;
    rudp_pack_record(&fwd_recs[0], fwd_datagram + 4, sizeof(fwd_datagram) - 4);

    fwd_recs[1].channel_id = 2;
    fwd_recs[1].flags = RUDP_RECORD_FLAG_RELIABLE;
    fwd_recs[1].seq_num = 0;
    fwd_recs[1].payload = barrier;
    rudp_pack_record(&fwd_recs[1], fwd_datagram + 12, sizeof(fwd_datagram) - 12);

    rudp_record_s delivered_to_decode[4];
    int count = rudp_session_process_datagram(&decode_node, fwd_datagram, 20, delivered_to_decode, 4);
    assert(count == 2);
    assert(decode_node.channels[0].ctx.expected_seq_num == 1);
    assert(decode_node.channels[0].ack_pending == 1); // ACK pending on Channel 0!
    assert(decode_node.channels[2].ctx.expected_seq_num == 1);
    assert(decode_node.channels[2].ack_pending == 1); // ACK pending on Channel 2!

    // Step 3: Decode node responds with 1 SINGLE DATAGRAM that simultaneously:
    // - Header ACK: confirms Channel 0 (KV-Cache, ack=1)
    // - Record 0: explicit ACK record for Channel 2 (Barrier, ack=1, flags=FLAG_ACK)
    // - Record 1: speculative draft token for Channel 1 (Unreliable, token_id=4242)
    uint8_t reply_datagram[64];
    rudp_datagram_header_s reply_hdr = {
        .ack = decode_node.channels[0].ctx.expected_seq_num, // ack = 1 for Channel 0
        .ack_channel = 0,
        .count = 2
    };
    rudp_pack_datagram_header(&reply_hdr, reply_datagram, sizeof(reply_datagram));

    // Record 0: Explicit ACK record for Channel 2
    rudp_record_s rec_ack_ch2 = {
        .channel_id = 2,
        .flags = RUDP_RECORD_FLAG_ACK,
        .seq_num = decode_node.channels[2].ctx.expected_seq_num, // ack = 1 for Channel 2
        .payload = { .raw = 0 }
    };
    rudp_pack_record(&rec_ack_ch2, reply_datagram + 4, sizeof(reply_datagram) - 4);

    // Record 1: Speculative token on Channel 1
    tfv_packet_u draft_token = { .type = 50, .flags = 0, .value = 4242 };
    rudp_record_s rec_draft = {
        .channel_id = 1,
        .flags = RUDP_RECORD_FLAG_UNRELIABLE,
        .seq_num = decode_node.channels[1].next_unreliable_seq++,
        .payload = draft_token
    };
    rudp_pack_record(&rec_draft, reply_datagram + 12, sizeof(reply_datagram) - 12);

    // Step 4: Prefill node processes the 20-byte reply datagram
    rudp_record_s delivered_to_prefill[4];
    count = rudp_session_process_datagram(&prefill_node, reply_datagram, 20, delivered_to_prefill, 4);

    // Only the draft token is delivered to application; ACK records are absorbed internally!
    assert(count == 1);
    assert(delivered_to_prefill[0].channel_id == 1);
    assert(delivered_to_prefill[0].flags == RUDP_RECORD_FLAG_UNRELIABLE);
    assert(delivered_to_prefill[0].payload.value == 4242);

    // Verify BOTH sliding windows on Prefill node were freed in a single datagram:
    assert(prefill_node.channels[0].ctx.tx_buffer[0].state == RUDP_SLOT_FREE); // Channel 0 freed!
    assert(prefill_node.channels[0].ctx.tail == 1);

    assert(prefill_node.channels[2].ctx.tx_buffer[0].state == RUDP_SLOT_FREE); // Channel 2 freed by ACK record!
    assert(prefill_node.channels[2].ctx.tail == 1);

    printf("[OK] Multi-Channel Explicit ACK Record Bundling (Simultaneous Multi-ACK & Stream Multiplexing verified)\n");
}

// --- 16. Bug 1 Verification: Unreliable Piggybacked ACKs Must Not Trigger Phantom Fast Retransmit ---
void test_bug1_unreliable_stream_no_spurious_fast_retransmit(void) {
    rudp_session_s alice, bob;
    assert(rudp_session_init(&alice) == RUDP_OK);
    assert(rudp_session_init(&bob) == RUDP_OK);

    assert(rudp_session_config_channel(&alice, 0, RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED) == RUDP_OK);
    assert(rudp_session_config_channel(&bob, 0, RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED) == RUDP_OK);
    assert(rudp_session_config_channel(&alice, 1, RUDP_CHANNEL_FLAG_UNRELIABLE) == RUDP_OK);
    assert(rudp_session_config_channel(&bob, 1, RUDP_CHANNEL_FLAG_UNRELIABLE) == RUDP_OK);

    // Alice sends packet 0 (seq = 0) on Channel 0 at t = 1000
    tfv_packet_u p0 = { .type = 1, .flags = 0, .value = 100 };
    assert(rudp_session_send_reliable(&alice, 0, p0, 1000) == RUDP_OK);

    // Bob receives packet 0
    uint8_t dgram[64];
    int len = rudp_session_build_datagram(&alice, 0, dgram, sizeof(dgram), 1000, 100);
    assert(len == 12); // 4B hdr + 8B rec
    rudp_record_s delivered[4];
    assert(rudp_session_process_datagram(&bob, dgram, (size_t)len, delivered, 4) == 1);
    assert(bob.channels[0].ctx.expected_seq_num == 1);

    // Alice sends packet 1 (seq = 1) on Channel 0 at t = 1010
    tfv_packet_u p1 = { .type = 1, .flags = 0, .value = 101 };
    assert(rudp_session_send_reliable(&alice, 0, p1, 1010) == RUDP_OK);
    assert(alice.channels[0].ctx.tx_buffer[1].state == RUDP_SLOT_IN_FLIGHT);

    // Bob acknowledges packet 0 by sending high-frequency unreliable telemetry on Channel 1 (100 packets)
    // Piggybacking ack = 1 for ack_channel = 0.
    for (int i = 0; i < 100; i++) {
        tfv_packet_u telemetry = { .type = 2, .flags = 0, .value = (uint32_t)i };
        uint8_t unrec_buf[32];
        len = rudp_session_send_unreliable(&bob, 1, telemetry, 0, unrec_buf, sizeof(unrec_buf));
        assert(len == 12);

        int rec_count = rudp_session_process_datagram(&alice, unrec_buf, (size_t)len, delivered, 4);
        assert(rec_count == 1);
        assert(delivered[0].channel_id == 1);
        assert(delivered[0].payload.value == (uint32_t)i);
    }

    // Packet 0 is freed on Alice
    assert(alice.channels[0].ctx.tx_buffer[0].state == RUDP_SLOT_FREE);
    assert(alice.channels[0].ctx.tail == 1);

    // Packet 1 is still in-flight on Alice.
    // Crucial Bug 1 verification: despite 99 redundant passive ACKs for ack = 1,
    // duplicate_ack_count MUST BE ZERO, and fast_retransmit MUST NOT BE TRIGGERED!
    assert(alice.channels[0].ctx.duplicate_ack_count == 0);
    assert(alice.channels[0].ctx.tx_buffer[1].fast_retransmit == RUDP_FAST_RETRANSMIT_OFF);

    // Now, simulate true loss detection: Bob sends 3 deliberate Standalone ACKs (count == 0)
    for (int i = 0; i < 3; i++) {
        rudp_datagram_header_s sa_hdr = {
            .ack = 1,
            .ack_channel = 0,
            .count = 0
        };
        uint8_t sa_buf[8];
        assert(rudp_pack_datagram_header(&sa_hdr, sa_buf, sizeof(sa_buf)) == 4);
        assert(rudp_session_process_datagram(&alice, sa_buf, 4, delivered, 4) == 0);
    }

    // Standalone ACKs DO increment duplicate_ack_count and trigger Tri-ACK Fast Retransmit!
    assert(alice.channels[0].ctx.duplicate_ack_count == 3);
    assert(alice.channels[0].ctx.tx_buffer[1].fast_retransmit == RUDP_FAST_RETRANSMIT_PENDING);

    printf("[OK] Bug 1 Verified: Passive piggybacked ACKs do NOT trigger phantom Fast Retransmit\n");
}

// --- 17. Bug 2 Verification: Output Buffer Saturation Prevents Silent Reliable Packet Loss ---
void test_bug2_output_buffer_saturation_no_silent_loss(void) {
    rudp_session_s rx_node;
    assert(rudp_session_init(&rx_node) == RUDP_OK);
    assert(rudp_session_config_channel(&rx_node, 0, RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED) == RUDP_OK);

    // Construct datagram with 3 reliable packets (seq 0, 1, 2)
    uint8_t dgram[64];
    rudp_datagram_header_s hdr = { .ack = 0, .ack_channel = 0, .count = 3 };
    assert(rudp_pack_datagram_header(&hdr, dgram, sizeof(dgram)) == 4);

    for (uint16_t s = 0; s < 3; s++) {
        rudp_record_s rec = {
            .channel_id = 0,
            .flags = RUDP_RECORD_FLAG_RELIABLE,
            .seq_num = s,
            .payload = { .type = 1, .flags = 0, .value = (uint32_t)(100 + s) }
        };
        assert(rudp_pack_record(&rec, dgram + 4 + (s * 8), sizeof(dgram) - (4 + s * 8)) == 8);
    }

    // Receiver application only has space for 1 record (max_delivered = 1)
    rudp_record_s delivered[1];
    int count = rudp_session_process_datagram(&rx_node, dgram, 28, delivered, 1);
    assert(count == 1);
    assert(delivered[0].seq_num == 0);
    assert(delivered[0].payload.value == 100);

    // Crucial Bug 2 verification: expected_seq_num MUST BE 1, NOT 3!
    // Since seq 1 and 2 were not delivered to application, they must NOT be acknowledged!
    assert(rx_node.channels[0].ctx.expected_seq_num == 1);
    assert(rx_node.channels[0].ack_pending == 1);

    // When the remaining packets (seq 1, 2) are retransmitted/received and buffer has space (max_delivered = 2):
    uint8_t dgram2[64];
    rudp_datagram_header_s hdr2 = { .ack = 0, .ack_channel = 0, .count = 2 };
    assert(rudp_pack_datagram_header(&hdr2, dgram2, sizeof(dgram2)) == 4);
    for (uint16_t s = 0; s < 2; s++) {
        rudp_record_s rec = {
            .channel_id = 0,
            .flags = RUDP_RECORD_FLAG_RELIABLE,
            .seq_num = (uint16_t)(1 + s),
            .payload = { .type = 1, .flags = 0, .value = (uint32_t)(101 + s) }
        };
        assert(rudp_pack_record(&rec, dgram2 + 4 + (s * 8), sizeof(dgram2) - (4 + s * 8)) == 8);
    }

    rudp_record_s delivered2[2];
    count = rudp_session_process_datagram(&rx_node, dgram2, 20, delivered2, 2);
    assert(count == 2);
    assert(delivered2[0].seq_num == 1);
    assert(delivered2[0].payload.value == 101);
    assert(delivered2[1].seq_num == 2);
    assert(delivered2[1].payload.value == 102);
    assert(rx_node.channels[0].ctx.expected_seq_num == 3);

    printf("[OK] Bug 2 Verified: Buffer saturation prevents premature ACK and silent loss\n");
}

// --- 18. Bug 3 Verification: Retransmitted Duplicate Packets Re-Arm ACK ---
void test_bug3_duplicate_reliable_packet_rearms_ack(void) {
    rudp_session_s sender, receiver;
    assert(rudp_session_init(&sender) == RUDP_OK);
    assert(rudp_session_init(&receiver) == RUDP_OK);

    assert(rudp_session_config_channel(&sender, 0, RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED) == RUDP_OK);
    assert(rudp_session_config_channel(&receiver, 0, RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED) == RUDP_OK);

    // Sender sends packet 0
    tfv_packet_u p0 = { .type = 1, .flags = 0, .value = 777 };
    assert(rudp_session_send_reliable(&sender, 0, p0, 1000) == RUDP_OK);

    // Receiver gets packet 0
    uint8_t dgram[32];
    int len = rudp_session_build_datagram(&sender, 0, dgram, sizeof(dgram), 1000, 100);
    assert(len == 12);
    rudp_record_s delivered[2];
    assert(rudp_session_process_datagram(&receiver, dgram, (size_t)len, delivered, 2) == 1);
    assert(receiver.channels[0].ctx.expected_seq_num == 1);
    assert(receiver.channels[0].ack_pending == 1);

    // Receiver creates ACK datagram to acknowledge packet 0
    uint8_t ack_dgram[32];
    len = rudp_session_build_datagram(&receiver, 0, ack_dgram, sizeof(ack_dgram), 1000, 100);
    assert(len == 4); // count == 0
    assert(receiver.channels[0].ack_pending == 0); // ACK sent, pending cleared

    // SIMULATE NETWORK DROP: The ACK datagram is lost on the network!
    // Sender never received the ACK, so sender retransmits packet 0!
    // Receiver receives duplicate packet 0 (seq 0 < expected_seq 1)
    int count = rudp_session_process_datagram(&receiver, dgram, 12, delivered, 2);
    assert(count == 0); // Duplicate payload ignored

    // Crucial Bug 3 verification: receiver MUST re-arm ack_pending to unblock sender!
    assert(receiver.channels[0].ack_pending == 1);

    // Receiver builds datagram to reply
    len = rudp_session_build_datagram(&receiver, 0, ack_dgram, sizeof(ack_dgram), 1000, 100);
    assert(len == 4);
    assert(receiver.channels[0].ack_pending == 0);

    // Sender receives the re-armed ACK and successfully frees its window!
    assert(rudp_session_process_datagram(&sender, ack_dgram, (size_t)len, delivered, 2) == 0);
    assert(sender.channels[0].ctx.tx_buffer[0].state == RUDP_SLOT_FREE);
    assert(sender.channels[0].ctx.tail == 1);

    printf("[OK] Bug 3 Verified: Duplicate reliable packet re-arms ACK to break deadlock\n");
}

// --- 19. Architectural Bundler Verification: Intra-Tick Bundler MTU Packing ---
void test_session_build_datagram_intra_tick_bundler(void) {
    rudp_session_s tx_session, rx_session;
    assert(rudp_session_init(&tx_session) == RUDP_OK);
    assert(rudp_session_init(&rx_session) == RUDP_OK);

    // Configure 3 channels
    assert(rudp_session_config_channel(&tx_session, 0, RUDP_CHANNEL_FLAG_RELIABLE) == RUDP_OK);
    assert(rudp_session_config_channel(&rx_session, 0, RUDP_CHANNEL_FLAG_RELIABLE) == RUDP_OK);
    assert(rudp_session_config_channel(&tx_session, 1, RUDP_CHANNEL_FLAG_RELIABLE) == RUDP_OK);
    assert(rudp_session_config_channel(&rx_session, 1, RUDP_CHANNEL_FLAG_RELIABLE) == RUDP_OK);
    assert(rudp_session_config_channel(&tx_session, 2, RUDP_CHANNEL_FLAG_UNRELIABLE) == RUDP_OK);
    assert(rudp_session_config_channel(&rx_session, 2, RUDP_CHANNEL_FLAG_UNRELIABLE) == RUDP_OK);

    // Queue reliable packets on Ch0 and Ch1
    tfv_packet_u p0 = { .type = 1, .flags = 0, .value = 1000 };
    tfv_packet_u p1 = { .type = 2, .flags = 0, .value = 2000 };
    assert(rudp_session_send_reliable(&tx_session, 0, p0, 500) == RUDP_OK);
    assert(rudp_session_send_reliable(&tx_session, 1, p1, 500) == RUDP_OK);

    // Set pending ACK on Ch2
    tx_session.channels[2].ack_pending = 1;
    tx_session.channels[2].ctx.expected_seq_num = 42;

    // Build unified datagram with primary_ack_channel = 0
    uint8_t out_buf[128];
    int built_len = rudp_session_build_datagram(&tx_session, 0, out_buf, sizeof(out_buf), 1000, 100);
    // Header (4B) + Ch2 ACK record (8B) + Ch0 reliable record (8B) + Ch1 reliable record (8B) = 28B
    assert(built_len == 28);
    assert(tx_session.channels[2].ack_pending == 0);
    assert(tx_session.channels[2].last_ack_sent == 42);

    rudp_datagram_header_s parsed_hdr;
    rudp_record_s parsed_recs[3];
    assert(rudp_unpack_datagram(out_buf, 28, &parsed_hdr, parsed_recs, 3) == RUDP_OK);
    assert(parsed_hdr.ack_channel == 0);
    assert(parsed_hdr.count == 3);

    assert(parsed_recs[0].channel_id == 2);
    assert(parsed_recs[0].flags == RUDP_RECORD_FLAG_ACK);
    assert(parsed_recs[0].seq_num == 42);

    assert(parsed_recs[1].channel_id == 0);
    assert(parsed_recs[1].flags == RUDP_RECORD_FLAG_RELIABLE);
    assert(parsed_recs[1].seq_num == 0);

    assert(parsed_recs[2].channel_id == 1);
    assert(parsed_recs[2].flags == RUDP_RECORD_FLAG_RELIABLE);
    assert(parsed_recs[2].seq_num == 0);

    // Test buffer capacity truncation (max_len = 12 allows header + 1 record) on timeout retransmit
    uint8_t small_buf[12];
    int small_len = rudp_session_build_datagram(&tx_session, 0, small_buf, sizeof(small_buf), 1200, 100);
    assert(small_len == 12);
    assert(rudp_unpack_datagram_header(small_buf, 12, &parsed_hdr) == RUDP_OK);
    assert(parsed_hdr.count == 1);

    // Test buffer too small (< 4 bytes)
    assert(rudp_session_build_datagram(&tx_session, 0, small_buf, 3, 1000, 100) == RUDP_ERR_BUFFER_FULL);

    // Test invalid channel ID
    assert(rudp_session_build_datagram(&tx_session, 99, out_buf, sizeof(out_buf), 1000, 100) == RUDP_ERR_INVALID_ARG);

    printf("[OK] Intra-Tick Bundler Verified: Multi-ACK and Multi-Channel Reliable In-Flight aggregation\n");
}

static void test_gap_triggered_fast_retransmit(void) {
    rudp_session_s sender, receiver;
    assert(rudp_session_init(&sender) == RUDP_OK);
    assert(rudp_session_init(&receiver) == RUDP_OK);
    assert(rudp_session_config_channel(&sender, 0, RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED) == RUDP_OK);
    assert(rudp_session_config_channel(&receiver, 0, RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED) == RUDP_OK);

    // Sender sends packets 0, 1, 2, 3, 4
    for (uint16_t i = 0; i < 5; i++) {
        tfv_packet_u p = { .type = 1, .flags = 0, .value = i };
        assert(rudp_session_send_reliable(&sender, 0, p, 1000) == RUDP_OK);
    }

    // Packet 0 is delivered
    uint8_t dgram[64];
    rudp_record_s rec_delivered[4];
    uint8_t out_p0[12];
    rudp_datagram_header_s h0 = {0, 0, 1};
    rudp_record_s r0 = {0, RUDP_RECORD_FLAG_RELIABLE, 0, {.value = 0}};
    assert(rudp_pack_datagram_header(&h0, out_p0, sizeof(out_p0)) == 4);
    assert(rudp_pack_record(&r0, out_p0 + 4, sizeof(out_p0) - 4) == 8);
    assert(rudp_session_process_datagram(&receiver, out_p0, 12, rec_delivered, 4) == 1);
    assert(receiver.channels[0].ctx.expected_seq_num == 1);
    assert(receiver.channels[0].ack_pending == 1);

    // Receiver ACKs packet 0 (ack=1)
    int len = rudp_session_build_datagram(&receiver, 0, dgram, sizeof(dgram), 1000, 100);
    assert(len == 4);
    assert(rudp_session_process_datagram(&sender, dgram, (size_t)len, rec_delivered, 4) == 0);
    assert(sender.channels[0].ctx.tail == 1); // Packet 0 freed

    // SIMULATE DROP: Packet 1 is lost on the wire!
    // Packets 2, 3, 4 arrive at receiver out-of-order (gap)
    for (uint16_t seq = 2; seq <= 4; seq++) {
        uint8_t gap_buf[12];
        rudp_datagram_header_s gh = {0, 0, 1};
        rudp_record_s gr = {0, RUDP_RECORD_FLAG_RELIABLE, seq, {.value = seq}};
        assert(rudp_pack_datagram_header(&gh, gap_buf, sizeof(gap_buf)) == 4);
        assert(rudp_pack_record(&gr, gap_buf + 4, sizeof(gap_buf) - 4) == 8);

        // Gap record rejected from immediate delivery
        assert(rudp_session_process_datagram(&receiver, gap_buf, 12, rec_delivered, 4) == 0);
        // BUT receiver MUST re-arm ack_pending to send duplicate ACK!
        assert(receiver.channels[0].ack_pending == 1);

        // Receiver sends duplicate ACK back to sender
        len = rudp_session_build_datagram(&receiver, 0, dgram, sizeof(dgram), 1000, 100);
        assert(len == 4);
        assert(rudp_session_process_datagram(&sender, dgram, (size_t)len, rec_delivered, 4) == 0);
    }

    // Sender has now received 3 duplicate ACKs (Tri-ACK) for expected_seq_num 1
    assert(sender.channels[0].ctx.duplicate_ack_count == 3);
    assert(sender.channels[0].ctx.tx_buffer[1].fast_retransmit == RUDP_FAST_RETRANSMIT_PENDING);

    // Fast Retransmit triggers immediately via rudp_tick at t=1010 without waiting for 100ms timeout!
    uint16_t expired[4];
    rudp_tick_result_s res = rudp_tick(&sender.channels[0].ctx, 1010, 100, expired, 4);
    assert(res.status == RUDP_OK && res.count == 1);
    assert(expired[0] == 1); // Slot 1 flagged and collected immediately

    printf("[OK] Bug 3 (Tri-ACK Gap): Reliable sequence gap triggers ACK re-arming and prompt Tri-ACK Fast Retransmit\n");
}

static void test_duplicate_ack_saturation(void) {
    rudp_context_s ctx;
    assert(rudp_init(&ctx) == RUDP_OK);
    tfv_packet_u p = {.type = 1, .flags = 0, .value = 123};
    assert(rudp_send(&ctx, p, 1000) == RUDP_OK);

    // Initial observation of baseline ACK=0
    assert(rudp_recv_ack(&ctx, 0) == RUDP_OK);
    assert(ctx.duplicate_ack_count == 0);

    // Send 300 duplicate ACKs (packet 0 in flight, ack 0 is duplicate)
    for (int i = 1; i <= 300; i++) {
        assert(rudp_recv_ack(&ctx, 0) == RUDP_OK);
        if (i == 3) {
            assert(ctx.duplicate_ack_count == 3);
            assert(ctx.tx_buffer[0].fast_retransmit == RUDP_FAST_RETRANSMIT_PENDING);
            // Simulate fast retransmit being handled
            ctx.tx_buffer[0].fast_retransmit = RUDP_FAST_RETRANSMIT_OFF;
        }
    }

    // Must saturate at 255 and NOT rollover to 0
    assert(ctx.duplicate_ack_count == 255);
    // Fast retransmit must not have been triggered again after count 3
    assert(ctx.tx_buffer[0].fast_retransmit == RUDP_FAST_RETRANSMIT_OFF);

    printf("[OK] Bug 4 (Saturating Duplicate ACK): duplicate_ack_count saturates at 255 without rollover\n");
}

static void test_unreliable_jump_bound_critical_2(void) {
    rudp_session_s rx_session;
    assert(rudp_session_init(&rx_session) == RUDP_OK);
    assert(rudp_session_config_channel(&rx_session, 1, RUDP_CHANNEL_FLAG_UNRELIABLE) == RUDP_OK);

    rudp_record_s delivered[4];
    uint8_t buf[12];
    rudp_datagram_header_s hdr = {0, 0, 1};

    // Packet 0 with seq = 100 arrives
    rudp_record_s r1 = {1, RUDP_RECORD_FLAG_UNRELIABLE, 100, {.value = 100}};
    assert(rudp_pack_datagram_header(&hdr, buf, sizeof(buf)) == 4);
    assert(rudp_pack_record(&r1, buf + 4, sizeof(buf) - 4) == 8);
    assert(rudp_session_process_datagram(&rx_session, buf, 12, delivered, 4) == 1);
    assert(rx_session.channels[1].last_unreliable_seq == 100);

    // Valid forward jump: seq = 600 (distance = 500 <= 512)
    rudp_record_s r2 = {1, RUDP_RECORD_FLAG_UNRELIABLE, 600, {.value = 600}};
    assert(rudp_pack_record(&r2, buf + 4, sizeof(buf) - 4) == 8);
    assert(rudp_session_process_datagram(&rx_session, buf, 12, delivered, 4) == 1);
    assert(rx_session.channels[1].last_unreliable_seq == 600);

    // Malicious forward jump: seq = 600 + 30000 (distance > 512)
    rudp_record_s r_mal = {1, RUDP_RECORD_FLAG_UNRELIABLE, 30600, {.value = 999}};
    assert(rudp_pack_record(&r_mal, buf + 4, sizeof(buf) - 4) == 8);
    assert(rudp_session_process_datagram(&rx_session, buf, 12, delivered, 4) == 0);
    assert(rx_session.channels[1].last_unreliable_seq == 600); // Unchanged!

    // Malicious 32767 jump:
    rudp_record_s r_32k = {1, RUDP_RECORD_FLAG_UNRELIABLE, (uint16_t)(600 + 32767), {.value = 999}};
    assert(rudp_pack_record(&r_32k, buf + 4, sizeof(buf) - 4) == 8);
    assert(rudp_session_process_datagram(&rx_session, buf, 12, delivered, 4) == 0);
    assert(rx_session.channels[1].last_unreliable_seq == 600); // Unchanged!

    // Legitimate packet seq = 601 is delivered promptly (channel NOT muted!)
    rudp_record_s r3 = {1, RUDP_RECORD_FLAG_UNRELIABLE, 601, {.value = 601}};
    assert(rudp_pack_record(&r3, buf + 4, sizeof(buf) - 4) == 8);
    assert(rudp_session_process_datagram(&rx_session, buf, 12, delivered, 4) == 1);
    assert(rx_session.channels[1].last_unreliable_seq == 601);

    printf("[OK] Critical 2 (Bounded Unreliable Jump): RFC 1982 jump bounded to RUDP_UNRELIABLE_MAX_AHEAD\n");
}

static void test_bundler_retransmission_gating_and_fairness(void) {
    rudp_session_s sess;
    assert(rudp_session_init(&sess) == RUDP_OK);
    for (uint8_t c = 0; c < 4; c++) {
        assert(rudp_session_config_channel(&sess, c, RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED) == RUDP_OK);
        tfv_packet_u p = {.type = 1, .flags = 0, .value = c};
        assert(rudp_session_send_reliable(&sess, c, p, 1000) == RUDP_OK);
    }

    // A. Round-Robin fairness test: datagram capacity of only 1 record
    uint8_t small_buf[12];
    rudp_datagram_header_s hdr;
    rudp_record_s rec;

    // Call 1: should pick channel 0
    int len = rudp_session_build_datagram(&sess, 0, small_buf, sizeof(small_buf), 1000, 100);
    assert(len == 12);
    assert(rudp_unpack_datagram(small_buf, 12, &hdr, &rec, 1) == RUDP_OK);
    assert(rec.channel_id == 0);

    // Call 2: round-robin cursor picks channel 1
    len = rudp_session_build_datagram(&sess, 0, small_buf, sizeof(small_buf), 1000, 100);
    assert(len == 12);
    assert(rudp_unpack_datagram(small_buf, 12, &hdr, &rec, 1) == RUDP_OK);
    assert(rec.channel_id == 1);

    // Call 3: round-robin cursor picks channel 2
    len = rudp_session_build_datagram(&sess, 0, small_buf, sizeof(small_buf), 1000, 100);
    assert(len == 12);
    assert(rudp_unpack_datagram(small_buf, 12, &hdr, &rec, 1) == RUDP_OK);
    assert(rec.channel_id == 2);

    // Call 4: round-robin cursor picks channel 3
    len = rudp_session_build_datagram(&sess, 0, small_buf, sizeof(small_buf), 1000, 100);
    assert(len == 12);
    assert(rudp_unpack_datagram(small_buf, 12, &hdr, &rec, 1) == RUDP_OK);
    assert(rec.channel_id == 3);

    // B. Retransmission Gating:
    // At t = 1010 (10ms later, timeout is 100ms), all 4 packets have been sent once and not timed out
    uint8_t big_buf[256];
    len = rudp_session_build_datagram(&sess, 0, big_buf, sizeof(big_buf), 1010, 100);
    assert(len == 4); // Only datagram header, count == 0! No spurious 60x/sec duplication!
    assert(rudp_unpack_datagram_header(big_buf, 4, &hdr) == RUDP_OK);
    assert(hdr.count == 0);

    // At t = 1150 (150ms > 100ms timeout), all 4 packets timed out and are retransmitted
    len = rudp_session_build_datagram(&sess, 0, big_buf, sizeof(big_buf), 1150, 100);
    assert(len == 4 + 4 * 8); // Header (4B) + 4 records (32B) = 36B
    assert(rudp_unpack_datagram_header(big_buf, (size_t)len, &hdr) == RUDP_OK);
    assert(hdr.count == 4);

    // C. MTU clamp: max_len = 65535 clamped to RUDP_DEFAULT_MTU (1400 bytes)
    assert(rudp_session_build_datagram(&sess, 0, big_buf, 65535, 1150, 100) <= RUDP_DEFAULT_MTU);

    printf("[OK] High 3 & 4 (Bundler Fairness & Gating): Round-robin fairness, retransmission gating, and MTU clamp verified\n");
}

static void test_legacy_recv_passive_ack(void) {
    rudp_context_s ctx;
    assert(rudp_init(&ctx) == RUDP_OK);
    tfv_packet_u p = {.type = 1, .flags = 0, .value = 42};
    assert(rudp_send(&ctx, p, 1000) == RUDP_OK);

    // Incoming data frames with piggybacked ACK=0
    rudp_frame_s incoming_frame;
    incoming_frame.header.seq_num = 0;
    incoming_frame.header.ack = 0;
    incoming_frame.packet.raw = 100;

    tfv_packet_u delivered;
    for (int i = 0; i < 5; i++) {
        int ret = rudp_recv(&ctx, &incoming_frame, &delivered);
        if (i == 0) assert(ret == 1);
        else assert(ret == 0);
    }

    // Verify passive ACK did not increment duplicate_ack_count
    assert(ctx.duplicate_ack_count == 0);
    assert(ctx.tx_buffer[0].fast_retransmit == RUDP_FAST_RETRANSMIT_OFF);

    printf("[OK] High 5 (Passive rudp_recv ACK): Data frames with piggybacked ACKs do not trigger phantom Fast Retransmit\n");
}

static void test_negative_bounds_and_mutations(void) {
    rudp_session_s sess;
    assert(rudp_session_init(&sess) == RUDP_OK);
    assert(rudp_session_config_channel(&sess, 0, RUDP_CHANNEL_FLAG_RELIABLE) == RUDP_OK);
    assert(rudp_session_config_channel(&sess, 1, RUDP_CHANNEL_FLAG_UNRELIABLE) == RUDP_OK);

    // Low 14: send_reliable rejects channel without RUDP_CHANNEL_FLAG_RELIABLE
    tfv_packet_u p = {.value = 1};
    assert(rudp_session_send_reliable(&sess, 1, p, 1000) == RUDP_ERR_INVALID_ARG);

    // M5: send_reliable rejects channel_id >= RUDP_MAX_CHANNELS
    assert(rudp_session_send_reliable(&sess, RUDP_MAX_CHANNELS, p, 1000) == RUDP_ERR_INVALID_ARG);

    // M6: reset_channel rejects channel_id >= RUDP_MAX_CHANNELS
    assert(rudp_session_reset_channel(&sess, RUDP_MAX_CHANNELS) == RUDP_ERR_INVALID_ARG);
    assert(rudp_session_reset_channel(NULL, 0) == RUDP_ERR_INVALID_ARG);

    // M2: datagram with header.ack_channel >= RUDP_MAX_CHANNELS received over wire
    uint8_t bad_ack_chan_buf[4] = {0, 0, RUDP_MAX_CHANNELS, 0};
    rudp_record_s recs[2];
    assert(rudp_session_process_datagram(&sess, bad_ack_chan_buf, 4, recs, 2) == RUDP_ERR_INVALID_ARG);

    // M1: datagram with rec.channel_id >= RUDP_MAX_CHANNELS received over wire
    uint8_t bad_rec_buf[12] = {0, 0, 0, 1, RUDP_MAX_CHANNELS, 0, 0, 0, 0, 0, 0, 0};
    assert(rudp_session_process_datagram(&sess, bad_rec_buf, 12, recs, 2) == RUDP_ERR_INVALID_ARG);

    // M3: exact datagram wire length check: in_len != 4 + count * 8
    assert(rudp_session_process_datagram(&sess, bad_rec_buf, 11, recs, 2) == RUDP_ERR_INVALID_ARG);
    assert(rudp_session_process_datagram(&sess, bad_rec_buf, 13, recs, 2) == RUDP_ERR_INVALID_ARG);

    // M4: rudp_recv_ack_ex rejects ACK older than oldest in-flight packet (tail_seq)
    rudp_context_s ctx;
    assert(rudp_init(&ctx) == RUDP_OK);
    ctx.current_seq_num = 10;
    ctx.head = 1;
    ctx.tail = 0;
    ctx.tx_buffer[0].state = RUDP_SLOT_IN_FLIGHT;
    ctx.tx_buffer[0].frame.header.seq_num = 5; // oldest in-flight is 5
    // ACK = 4 is older than 5 -> OUT_OF_WINDOW
    assert(rudp_recv_ack_ex(&ctx, 4, true) == RUDP_ERR_OUT_OF_WINDOW);
    // ACK = 5 is valid (boundary)
    assert(rudp_recv_ack_ex(&ctx, 5, true) == RUDP_OK);
    // ACK = 11 is ahead of current_seq_num 10 -> OUT_OF_WINDOW
    assert(rudp_recv_ack_ex(&ctx, 11, true) == RUDP_ERR_OUT_OF_WINDOW);

    // High 7: Liveness detection
    assert(rudp_init(&ctx) == RUDP_OK);
    rudp_touch(&ctx, 1000);
    assert(rudp_is_alive(&ctx, 1500, 1000) == true);
    assert(rudp_is_alive(&ctx, 2100, 1000) == false);
    ctx.state = RUDP_STATE_DISCONNECTED;
    assert(rudp_is_alive(&ctx, 1000, 1000) == false);

    printf("[OK] Negative Bounds & Mutations (M1-M6, Capability, Liveness): All boundary checks strictly validated\n");
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
    test_multi_channel_record_ack_bundling();
    test_bug1_unreliable_stream_no_spurious_fast_retransmit();
    test_bug2_output_buffer_saturation_no_silent_loss();
    test_bug3_duplicate_reliable_packet_rearms_ack();
    test_session_build_datagram_intra_tick_bundler();
    test_gap_triggered_fast_retransmit();
    test_duplicate_ack_saturation();
    test_unreliable_jump_bound_critical_2();
    test_bundler_retransmission_gating_and_fairness();
    test_legacy_recv_passive_ack();
    test_negative_bounds_and_mutations();

    printf("\n>>> ALL TESTS PASSED SUCCESSFULLY! <<<\n");

    return 0;
}
