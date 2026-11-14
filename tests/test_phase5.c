#include "protocol_profiles.h"
#include "protocol_rudp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* 1. RFC 8439 §2.8.2 exact test vector + anti-replay + reflection + wipe */
static void test_rfc8439_and_envelope(void) {
  static const uint8_t rfc_key[32] = {
      0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a,
      0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
      0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f};
  static const uint8_t rfc_nonce[12] = {0x07, 0x00, 0x00, 0x00, 0x40, 0x41,
                                        0x42, 0x43, 0x44, 0x45, 0x46, 0x47};
  static const uint8_t rfc_aad[12] = {0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1,
                                      0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7};
  static const char rfc_text[] =
      "Ladies and Gentlemen of the class of '99: If I could offer you only "
      "one tip for the future, sunscreen would be it.";
  static const uint8_t expected_ct[114] = {
      0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb, 0x7b, 0x86, 0xaf, 0xbc,
      0x53, 0xef, 0x7e, 0xc2, 0xa4, 0xad, 0xed, 0x51, 0x29, 0x6e, 0x08, 0xfe,
      0xa9, 0xe2, 0xb5, 0xa7, 0x36, 0xee, 0x62, 0xd6, 0x3d, 0xbe, 0xa4, 0x5e,
      0x8c, 0xa9, 0x67, 0x12, 0x82, 0xfa, 0xfb, 0x69, 0xda, 0x92, 0x72, 0x8b,
      0x1a, 0x71, 0xde, 0x0a, 0x9e, 0x06, 0x0b, 0x29, 0x05, 0xd6, 0xa5, 0xb6,
      0x7e, 0xcd, 0x3b, 0x36, 0x92, 0xdd, 0xbd, 0x7f, 0x2d, 0x77, 0x8b, 0x8c,
      0x98, 0x03, 0xae, 0xe3, 0x28, 0x09, 0x1b, 0x58, 0xfa, 0xb3, 0x24, 0xe4,
      0xfa, 0xd6, 0x75, 0x94, 0x55, 0x85, 0x80, 0x8b, 0x48, 0x31, 0xd7, 0xbc,
      0x3f, 0xf4, 0xde, 0xf0, 0x8e, 0x4b, 0x7a, 0x9d, 0xe5, 0x76, 0xd2, 0x65,
      0x86, 0xce, 0xc6, 0x4b, 0x61, 0x16};
  static const uint8_t expected_tag[16] = {0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09,
                                           0xe2, 0x6a, 0x7e, 0x90, 0x2e, 0xcb,
                                           0xd0, 0x60, 0x06, 0x91};

  uint8_t ct[114], tag[16], pt[114];
  assert(rudp_chacha20_poly1305_encrypt(rfc_key, rfc_nonce, rfc_aad,
                                        sizeof(rfc_aad),
                                        (const uint8_t *)rfc_text, 114U, ct,
                                        tag) == RUDP_OK);
  assert(memcmp(ct, expected_ct, 114U) == 0);
  assert(memcmp(tag, expected_tag, 16U) == 0);
  assert(rudp_chacha20_poly1305_decrypt(rfc_key, rfc_nonce, rfc_aad,
                                        sizeof(rfc_aad), ct, 114U, tag,
                                        pt) == RUDP_OK);
  assert(memcmp(pt, rfc_text, 114U) == 0);

  /* Forged tag must fail without mutating output buffer */
  memset(pt, 0xA5, sizeof(pt));
  tag[0] ^= 0x01U;
  assert(rudp_chacha20_poly1305_decrypt(rfc_key, rfc_nonce, rfc_aad,
                                        sizeof(rfc_aad), ct, 114U, tag,
                                        pt) == RUDP_ERR_AUTH);
  assert(pt[0] == 0xA5U);

  /* Stateful envelope: directional subkeys, reflection defense, 64-pkt replay */
  rudp_crypto_ctx_s client, server;
  assert(rudp_crypto_init(&client, rfc_key, UINT64_C(0xCAFEBABE12345678), 1) ==
         RUDP_OK);
  assert(rudp_crypto_init(&server, rfc_key, UINT64_C(0xCAFEBABE12345678), 0) ==
         RUDP_OK);

  uint8_t pkts[80][64];
  uint8_t msg[12] = {0, 0, 0, 1, 0, 1, 0, 0, 0x42, 0, 0, 0};
  for (unsigned i = 0; i < 80U; ++i) {
    msg[8] = (uint8_t)i;
    int slen = rudp_crypto_seal(&client, rfc_aad, 4U, msg, sizeof(msg), pkts[i],
                                sizeof(pkts[i]));
    assert(slen == (int)(sizeof(msg) + RUDP_CRYPTO_OVERHEAD_BYTES));
  }

  /* Reflection attack: client trying to open its own packet fails auth */
  uint8_t out[64];
  memset(out, 0xA5, sizeof(out));
  rudp_crypto_ctx_s client_snapshot = client;
  assert(rudp_crypto_open(&client, rfc_aad, 4U, pkts[0], 36U, out,
                          sizeof(out)) == RUDP_ERR_AUTH);
  assert(memcmp(&client, &client_snapshot, sizeof(client)) == 0);
  assert(out[0] == 0xA5U);

  /* Out-of-order acceptance + anti-replay sliding window */
  assert(rudp_crypto_open(&server, rfc_aad, 4U, pkts[5], 36U, out,
                          sizeof(out)) == 12);
  assert(out[8] == 5U);
  /* Exact replay of nonce 5 must fail */
  assert(rudp_crypto_open(&server, rfc_aad, 4U, pkts[5], 36U, out,
                          sizeof(out)) == RUDP_ERR_AUTH);
  /* Older unseen nonce 0 within 64-packet window succeeds once */
  assert(rudp_crypto_open(&server, rfc_aad, 4U, pkts[0], 36U, out,
                          sizeof(out)) == 12);
  assert(out[8] == 0U);
  assert(rudp_crypto_open(&server, rfc_aad, 4U, pkts[0], 36U, out,
                          sizeof(out)) == RUDP_ERR_AUTH);

  /* Advance window to nonce 70: nonce 6 (< 70 - 63 = 7) is evicted, nonce 7 ok */
  assert(rudp_crypto_open(&server, rfc_aad, 4U, pkts[70], 36U, out,
                          sizeof(out)) == 12);
  rudp_crypto_ctx_s server_snapshot = server;
  assert(rudp_crypto_open(&server, rfc_aad, 4U, pkts[6], 36U, out,
                          sizeof(out)) == RUDP_ERR_AUTH);
  assert(memcmp(&server, &server_snapshot, sizeof(server)) == 0);

  /* Tampered ciphertext at valid unseen nonce 7 must not advance replay mask */
  pkts[7][20] ^= 0x80U;
  assert(rudp_crypto_open(&server, rfc_aad, 4U, pkts[7], 36U, out,
                          sizeof(out)) == RUDP_ERR_AUTH);
  assert(memcmp(&server, &server_snapshot, sizeof(server)) == 0);
  pkts[7][20] ^= 0x80U;
  assert(rudp_crypto_open(&server, rfc_aad, 4U, pkts[7], 36U, out,
                          sizeof(out)) == 12);

  rudp_crypto_wipe(&client);
  rudp_crypto_wipe(&server);
  for (size_t i = 0; i < sizeof(client.tx_key); ++i) {
    assert(client.tx_key[i] == 0U && server.rx_key[i] == 0U);
  }
  puts("PASS: RFC 8439 ChaCha20-Poly1305, directional keys, 64-pkt anti-replay, zero-mutation auth");
}

/* Custom hardware/OS AEAD vtable hook */
static int mock_encrypt(void *user_ctx,
                        const uint8_t key[RUDP_CRYPTO_KEY_BYTES],
                        const uint8_t nonce[RUDP_CRYPTO_NONCE_BYTES],
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *plaintext, size_t pt_len,
                        uint8_t *ciphertext,
                        uint8_t tag[RUDP_CRYPTO_TAG_BYTES]) {
  uint8_t k = (uint8_t)(*(const uint8_t *)user_ctx ^ key[0]);
  for (size_t i = 0; i < pt_len; ++i) {
    ciphertext[i] = (uint8_t)(plaintext[i] ^ k ^ nonce[11]);
  }
  memset(tag, (int)(k ^ (uint8_t)aad_len ^ nonce[11]), 16U);
  (void)aad;
  return RUDP_OK;
}

static int mock_decrypt(void *user_ctx,
                        const uint8_t key[RUDP_CRYPTO_KEY_BYTES],
                        const uint8_t nonce[RUDP_CRYPTO_NONCE_BYTES],
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *ciphertext, size_t ct_len,
                        const uint8_t tag[RUDP_CRYPTO_TAG_BYTES],
                        uint8_t *plaintext) {
  uint8_t k = (uint8_t)(*(const uint8_t *)user_ctx ^ key[0]);
  uint8_t expected = (uint8_t)(k ^ (uint8_t)aad_len ^ nonce[11]);
  uint8_t diff = 0U;
  for (size_t i = 0; i < 16U; ++i) diff |= (uint8_t)(tag[i] ^ expected);
  if (diff != 0U) return RUDP_ERR_AUTH;
  for (size_t i = 0; i < ct_len; ++i) {
    plaintext[i] = (uint8_t)(ciphertext[i] ^ k ^ nonce[11]);
  }
  (void)aad;
  return RUDP_OK;
}

static void test_custom_aead_vtable(void) {
  uint8_t secret = 0x5AU, zero_key[32] = {0};
  rudp_aead_ops_s ops = {
      .encrypt = mock_encrypt, .decrypt = mock_decrypt, .user_ctx = &secret};
  rudp_crypto_ctx_s tx, rx;
  assert(rudp_crypto_init(&tx, zero_key, 1, 1) == RUDP_OK);
  assert(rudp_crypto_init(&rx, zero_key, 1, 0) == RUDP_OK);
  assert(rudp_crypto_set_aead(&tx, &ops) == RUDP_OK);
  assert(rudp_crypto_set_aead(&rx, &ops) == RUDP_OK);
  uint8_t msg[5] = {1, 2, 3, 4, 5}, wire[32], plain[16];
  assert(rudp_crypto_seal(&tx, NULL, 0, msg, 5, wire, sizeof(wire)) == 29);
  assert(rudp_crypto_open(&rx, NULL, 0, wire, 29, plain, sizeof(plain)) == 5);
  assert(memcmp(msg, plain, 5) == 0);
  puts("PASS: pluggable AEAD vtable (rudp_aead_ops_s)");
}

/* 2. 32-bit SACK + FACK multi-loss repair in 1 RTT + Karn RTT on SACK + strict reject */
static void test_sack_fack_multiloss_and_karn(void) {
  rudp_session_s tx, rx;
  assert(rudp_session_init(&tx) == RUDP_OK);
  assert(rudp_session_init(&rx) == RUDP_OK);
  assert(rudp_session_config_recovery(&tx, 0, 200, 10, 2000) == RUDP_OK);
  assert(rudp_session_config_recovery(&rx, 0, 200, 10, 2000) == RUDP_OK);

  /* Queue 6 reliable messages: seq 0..5 at t=100 */
  for (uint16_t i = 0; i < 6U; ++i) {
    assert(rudp_session_send_reliable(
               &tx, 0, (tfv_packet_u){.type = 1, .value = (uint16_t)(100 + i)},
               100) == RUDP_OK);
  }
  uint8_t dgrams[6][16];
  for (unsigned i = 0; i < 6U; ++i) {
    assert(rudp_session_build_datagram(&tx, 0, dgrams[i], sizeof(dgrams[i]),
                                       100, 1) == 12);
  }

  /* Drop BOTH seq 0 and seq 2! Deliver 1, 3, 4, 5 at t=120 */
  rudp_record_s recs[8];
  rudp_stats_s tx_stats = {0}, rx_stats = {0};
  const unsigned delivered_idx[4] = {1, 3, 4, 5};
  for (unsigned k = 0; k < 4U; ++k) {
    unsigned idx = delivered_idx[k];
    assert(rudp_session_process_datagram_ex(&rx, dgrams[idx], 12, recs, 8, true,
                                            120, &rx_stats) == 0);
    /* Receiver emits 12-byte SACK datagram (4B header + 8B SACK record) */
    uint8_t ack_wire[32];
    int alen =
        rudp_session_build_datagram(&rx, 0, ack_wire, sizeof(ack_wire), 120, 4);
    assert(alen == 12);
    assert(rudp_session_process_datagram_ex(&tx, ack_wire, (size_t)alen, recs,
                                            8, true, 140, &tx_stats) == 0);
  }

  /* Verify Karn RTT sample was measured directly from SACKed slot (RTT = 40ms)! */
  assert(tx.channels[0].srtt_scaled == (40U << 3));
  assert(tx_stats.rx_sack_blocks == 4U);

  /* Verify SACK marked slots 1, 3, 4, 5 as SACKED and FACK marked BOTH 0 and 2 PENDING! */
  assert(tx.channels[0].ctx.tx_buffer[1].state == RUDP_SLOT_SACKED);
  assert(tx.channels[0].ctx.tx_buffer[3].state == RUDP_SLOT_SACKED);
  assert(rudp_slot_fast_state(&tx.channels[0].ctx.tx_buffer[0]) ==
         RUDP_FAST_RETRANSMIT_PENDING);
  assert(rudp_slot_fast_state(&tx.channels[0].ctx.tx_buffer[2]) ==
         RUDP_FAST_RETRANSMIT_PENDING);

  /* Forged SACK targeting a queued-but-unsent slot (seq 6, tx_count == 0) must be rejected cleanly! */
  assert(rudp_session_send_reliable(
             &tx, 0, (tfv_packet_u){.type = 1, .value = 106}, 141) == RUDP_OK);
  assert(tx.channels[0].ctx.tx_buffer[6].tx_count == 0U);
  uint8_t bad_sack[12];
  rudp_datagram_header_s bh = {.ack = 0, .ack_channel = 0, .count = 1};
  rudp_record_s br = {.channel_id = 0,
                      .flags = RUDP_RECORD_FLAG_ACK,
                      .seq_num = 0,
                      .payload = {.value = (uint16_t)(1U << 5)}}; /* offset 6 => seq 6 (tx_count==0!) */
  assert(rudp_pack_datagram_header(&bh, bad_sack, sizeof(bad_sack)) == 4);
  assert(rudp_pack_record(&br, bad_sack + 4, 8) == 8);
  assert(rudp_session_process_datagram_ex(&tx, bad_sack, 12, recs, 8, true, 141,
                                          &tx_stats) == 0);
  assert(tx_stats.rx_ack_rejected == 1U);
  assert(tx.channels[0].ctx.tx_buffer[6].state == RUDP_SLOT_IN_FLIGHT);

  /* At t=141 (long before initial 200ms RTO!), build_datagram emits seq 0, seq 2, and unsent seq 6! */
  uint8_t repair[64];
  int rlen =
      rudp_session_build_datagram(&tx, 0, repair, sizeof(repair), 141, 100);
  assert(rlen == 4 + 3 * 8); /* seq 0 (FACK), seq 2 (FACK), seq 6 (first send), never 1/3/4/5! */

  int got = rudp_session_process_datagram_ex(&rx, repair, (size_t)rlen, recs, 8,
                                             true, 160, &rx_stats);
  assert(got == 7);
  for (uint16_t i = 0; i < 7U; ++i) {
    assert(recs[i].seq_num == i && recs[i].payload.value == (uint16_t)(100 + i));
  }
  puts("PASS: 32-bit SACK, FACK 1-RTT multi-loss repair, SACK Karn RTT, strict unsent SACK rejection");
}

/* 3. Tail-Loss Probe (RFC 8985) */
static void test_tail_loss_probe(void) {
  rudp_session_s tx;
  assert(rudp_session_init(&tx) == RUDP_OK);
  assert(rudp_session_config_recovery(&tx, 0, 200, 20, 2000) == RUDP_OK);
  assert(rudp_session_send_reliable(&tx, 0, (tfv_packet_u){.type = 1, .value = 1},
                                    100) == RUDP_OK);
  uint8_t wire[32];
  assert(rudp_session_build_datagram(&tx, 0, wire, sizeof(wire), 100, 200) ==
         12);

  rudp_datagram_header_s ack0 = {.ack = 1, .ack_channel = 0, .count = 0};
  assert(rudp_pack_datagram_header(&ack0, wire, sizeof(wire)) == 4);
  assert(rudp_session_process_datagram_at(&tx, wire, 4, NULL, 0, 130) == 0);
  /* SRTT = 30ms, RTTVAR = 15ms -> RTO = 30 + 60 = 90ms; RFC 8985 PTO = 1.5*SRTT + 2 = 47ms */
  assert(tx.channels[0].rto_ms == 90U);

  /* Send single tail packet seq=1 at t=200 and lose it (no trailing packets to trigger SACK!) */
  assert(rudp_session_send_reliable(&tx, 0, (tfv_packet_u){.type = 1, .value = 99},
                                    200) == RUDP_OK);
  assert(rudp_session_build_datagram(&tx, 0, wire, sizeof(wire), 200, 200) ==
         12);

  /* At t=247 (elapsed 47 <= PTO=47), nothing fires */
  assert(rudp_session_build_datagram(&tx, 0, wire, sizeof(wire), 247, 200) <= 4);
  /* At t=248 (elapsed 48 > PTO=47, 43ms earlier than RTO=90!), TLP fires once with 0 backoff */
  assert(rudp_session_build_datagram(&tx, 0, wire, sizeof(wire), 248, 200) ==
         12);
  assert(rudp_slot_backoff(&tx.channels[0].ctx.tx_buffer[1]) == 0U);
  assert(tx.channels[0].chan_backoff == 0U);
  /* TLP does not re-fire at t=249 (waits for full RTO from 248) */
  assert(rudp_session_build_datagram(&tx, 0, wire, sizeof(wire), 249, 200) <= 4);
  puts("PASS: Tail-Loss Probe (RFC 8985) fires at 1.5*SRTT+2ms before RTO without penalizing backoff");
}

/* 4. Reliable Unordered channel (zero Head-of-Line blocking + exact-once dedup) */
static void test_reliable_unordered_zero_hol(void) {
  rudp_session_s tx, rx;
  assert(rudp_session_init(&tx) == RUDP_OK);
  assert(rudp_session_init(&rx) == RUDP_OK);
  /* RUDP_CHANNEL_FLAG_RELIABLE without RUDP_CHANNEL_FLAG_ORDERED = zero-HoL reliable unordered */
  uint8_t flags = RUDP_CHANNEL_FLAG_RELIABLE;
  assert(rudp_session_config_channel(&tx, 2, flags) == RUDP_OK);
  assert(rudp_session_config_channel(&rx, 2, flags) == RUDP_OK);
  assert(rudp_session_config_recovery(&tx, 2, 100, 10, 1000) == RUDP_OK);
  assert(rudp_session_config_recovery(&rx, 2, 100, 10, 1000) == RUDP_OK);

  for (uint16_t i = 0; i < 4U; ++i) {
    assert(rudp_session_send_reliable(
               &tx, 2, (tfv_packet_u){.type = 3, .value = (uint16_t)(500 + i)},
               10) == RUDP_OK);
  }
  uint8_t pkt[4][16];
  for (unsigned i = 0; i < 4U; ++i) {
    assert(rudp_session_build_datagram(&tx, 0, pkt[i], sizeof(pkt[i]), 10, 12) ==
           12);
  }

  /* Drop pkt[0]! Arrival of pkt[1] and pkt[2] MUST deliver IMMEDIATELY (zero HoL stall!) */
  rudp_record_s out[4];
  assert(rudp_session_process_datagram_at(&rx, pkt[1], 12, out, 4, 20) == 1);
  assert(out[0].seq_num == 1U && out[0].payload.value == 501U);
  /* Duplicate pkt[1] is suppressed (exact-once!) */
  assert(rudp_session_process_datagram_at(&rx, pkt[1], 12, out, 4, 21) == 0);

  assert(rudp_session_process_datagram_at(&rx, pkt[2], 12, out, 4, 22) == 1);
  assert(out[0].seq_num == 2U && out[0].payload.value == 502U);
  assert(rx.channels[2].ctx.expected_seq_num == 0U); /* Frontier still waiting for seq 0 */

  /* Finally pkt[0] arrives: delivers seq 0, advances frontier cleanly past 1 & 2 to 3 without re-delivering 1 & 2! */
  assert(rudp_session_process_datagram_at(&rx, pkt[0], 12, out, 4, 30) == 1);
  assert(out[0].seq_num == 0U && out[0].payload.value == 500U);
  assert(rudp_session_poll(&rx, out, 4) == 0);
  assert(rx.channels[2].ctx.expected_seq_num == 3U);
  /* Replaying pkt[1] after frontier advance is still rejected as old */
  assert(rudp_session_process_datagram_at(&rx, pkt[1], 12, out, 4, 31) == 0);
  puts("PASS: Reliable Unordered channel delivers immediately (0-HoL) with exact-once dedup");
}

/* 5. Zero-RTT XOR K=2 Forward Error Correction + prepare/cancel rollback */
static void test_fec_k2_zero_rtt_recovery(void) {
  rudp_session_s tx, rx;
  assert(rudp_session_init(&tx) == RUDP_OK);
  assert(rudp_session_init(&rx) == RUDP_OK);
  assert(rudp_session_config_channel(
             &tx, 0,
             RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED |
                 RUDP_CHANNEL_FLAG_FEC_K2) == RUDP_OK);
  assert(rudp_session_config_recovery(&tx, 0, 200, 20, 2000) == RUDP_OK);
  assert(rudp_session_config_recovery(&rx, 0, 200, 20, 2000) == RUDP_OK);

  assert(rudp_session_send_reliable(
             &tx, 0, (tfv_packet_u){.type = 7, .flags = 2, .value = 0x1234},
             50) == RUDP_OK);
  assert(rudp_session_send_reliable(
             &tx, 0, (tfv_packet_u){.type = 9, .flags = 1, .value = 0xABCD},
             50) == RUDP_OK);

  /* Datagram 1 & 2 carry data records (cap=12); parity is deferred to Datagram 3! */
  uint8_t d0[16], d1[16], d_fec[16];
  assert(rudp_session_build_datagram(&tx, 0, d0, 12, 50, 200) == 12);
  assert(rudp_session_build_datagram(&tx, 0, d1, 12, 50, 200) == 12);

  /* Test transactional cancel_datagram on FEC parity! */
  rudp_tx_plan_s plan;
  assert(rudp_session_prepare_datagram(&tx, 50, 200, d_fec, 12, &plan) == 12);
  assert(rudp_session_cancel_datagram(&tx, &plan) == RUDP_OK);
  /* Re-build parity after rollback must still emit the 12-byte parity datagram */
  assert(rudp_session_build_datagram(&tx, 0, d_fec, 12, 50, 200) == 12);

  /* Case A: Lose d0 (seq 0), deliver d1 (seq 1) + d_fec -> instant 0-RTT recovery of BOTH 0 and 1! */
  rudp_record_s out[4];
  rudp_stats_s stats = {0};
  assert(rudp_session_process_datagram_ex(&rx, d1, 12, out, 4, true, 60,
                                          &stats) == 0);
  int got = rudp_session_process_datagram_ex(&rx, d_fec, 12, out, 4, true, 60,
                                             &stats);
  assert(got == 2);
  assert(out[0].seq_num == 0U && out[0].payload.type == 7U &&
         out[0].payload.flags == 2U && out[0].payload.value == 0x1234U);
  assert(out[1].seq_num == 1U && out[1].payload.type == 9U &&
         out[1].payload.flags == 1U && out[1].payload.value == 0xABCDU);
  assert(stats.rx_fec_recoveries == 1U);

  /* Case B: Lose d1 (odd seq 3), deliver seq 2 + parity(2,3) -> instant 0-RTT recovery of seq 3! */
  assert(rudp_session_send_reliable(&tx, 0,
                                    (tfv_packet_u){.type = 2, .value = 0x5555},
                                    70) == RUDP_OK);
  assert(rudp_session_send_reliable(&tx, 0,
                                    (tfv_packet_u){.type = 4, .value = 0x7777},
                                    70) == RUDP_OK);
  assert(rudp_session_build_datagram(&tx, 0, d0, 12, 70, 200) == 12);
  assert(rudp_session_build_datagram(&tx, 0, d1, 12, 70, 200) == 12);
  assert(rudp_session_build_datagram(&tx, 0, d_fec, 12, 70, 200) == 12);

  assert(rudp_session_process_datagram_ex(&rx, d0, 12, out, 4, true, 80,
                                          &stats) == 1);
  assert(out[0].seq_num == 2U && out[0].payload.value == 0x5555U);
  assert(rudp_session_process_datagram_ex(&rx, d_fec, 12, out, 4, true, 80,
                                          &stats) == 1);
  assert(out[0].seq_num == 3U && out[0].payload.value == 0x7777U);
  assert(stats.rx_fec_recoveries == 2U);
  puts("PASS: Zero-RTT XOR K=2 FEC recovery (even & odd loss) + transactional rollback");
}

/* 6. Motion Dead-Reckoning (HOLD / LINEAR / MANEUVER) */
static void test_motion_dead_reckoning(void) {
  rudp_motion_tx_s tx = {0};
  rudp_motion_rx_s rx = {0};
  uint8_t wire[16];
  rudp_scalar_sample_s samples[2];

  /* Step 0: stationary cold start at value=1000 -> HOLD */
  assert(rudp_motion_encode(&tx, 1000, 50, 50, wire, sizeof(wire)) == 1);
  assert(rudp_motion_observe(&rx, wire, 8, 50, 50, samples, 2) == 1);
  assert(rx.regime == RUDP_DEAD_RECKON_HOLD);
  assert(rudp_motion_predict(&rx, 3, 5) == 1000U);

  /* Step 1: constant velocity +100 -> value=1100 -> LINEAR */
  assert(rudp_motion_encode(&tx, 1100, 50, 50, wire, sizeof(wire)) == 1);
  assert(rudp_motion_observe(&rx, wire, 8, 50, 50, samples, 2) == 1);
  assert(rx.regime == RUDP_DEAD_RECKON_LINEAR);
  assert(rudp_motion_predict(&rx, 1, 5) == 1200U);
  /* Horizon clamp: 10 steps ahead clamped to max_steps=2 -> 1100 + 2*100 = 1300 */
  assert(rudp_motion_predict(&rx, 10, 2) == 1300U);

  /* Step 2: high acceleration jump (+200 delta -> accel=+100 >= 50) -> MANEUVER */
  assert(rudp_motion_encode(&tx, 1300, 50, 50, wire, sizeof(wire)) == 2);
  assert(rudp_motion_observe(&rx, wire, 8, 50, 50, samples, 2) == 1);
  assert(rx.regime == RUDP_DEAD_RECKON_MANEUVER);
  /* Damped 2nd-order curve predicts ahead of pure linear (1300 + 200 = 1500) */
  assert(rudp_motion_predict(&rx, 1, 5) > 1500U);

  /* Duplicate / cloned rolling8 datagram is ignored without corrupting derivatives */
  assert(rudp_motion_observe(&rx, wire + 8, 8, 50, 50, samples, 2) == 0);
  assert(rx.last_value == 1300U);
  puts("PASS: Motion dead-reckoning (HOLD, LINEAR, damped MANEUVER, horizon clamp, clone dedup)");
}

/* 7. Scatter-gather build_iovec/prepare_iovec, DSCP propagation, path entropy, ABI compactness */
static void test_scatter_gather_seed_and_abi(void) {
  assert(sizeof(rudp_channel_s) == 1340U);
  assert(sizeof(rudp_session_s) == 5364U);

  rudp_session_s s1, s2;
  assert(rudp_session_init(&s1) == RUDP_OK);
  assert(rudp_session_init(&s2) == RUDP_OK);
  assert(rudp_session_set_seed(&s1, 0xBEEFU) == RUDP_OK);
  assert(rudp_session_set_seed(&s2, 0xCAFEU) == RUDP_OK);

  /* Channel 1 configured as high-priority (prio=0) with DSCP=46 (EF realtime) */
  assert(rudp_session_config_channel(&s1, 1,
                                     RUDP_CHANNEL_FLAG_RELIABLE |
                                         RUDP_CHANNEL_FLAG_ORDERED) == RUDP_OK);
  assert(rudp_session_config_channel(&s2, 1,
                                     RUDP_CHANNEL_FLAG_RELIABLE |
                                         RUDP_CHANNEL_FLAG_ORDERED) == RUDP_OK);
  assert(rudp_session_set_qos(&s1, 1, 0U, 46U) == RUDP_OK);
  assert(rudp_session_set_qos(&s2, 1, 0U, 46U) == RUDP_OK);

  assert(rudp_session_send_reliable(&s1, 1,
                                    (tfv_packet_u){.type = 5, .value = 777},
                                    1000) == RUDP_OK);
  assert(rudp_session_send_reliable(&s2, 1,
                                    (tfv_packet_u){.type = 5, .value = 777},
                                    1000) == RUDP_OK);

  uint8_t scratch[64], flat_iov[64], flat_direct[64];
  rudp_iovec_collection_s iov1, iov2;
  rudp_tx_plan_s plan;
  assert(rudp_session_prepare_iovec(&s1, 7U, 1000, 100, scratch,
                                    sizeof(scratch), &iov1, &plan) == 12);
  assert(iov1.dscp == 46U && iov1.count >= 1U);
  uint32_t entropy1 = iov1.path_entropy;

  /* Cancel and rebuild via build_iovec: wire output must match build_datagram byte-for-byte */
  assert(rudp_session_cancel_datagram(&s1, &plan) == RUDP_OK);
  assert(rudp_session_build_iovec(&s1, 0, 7U, scratch, sizeof(scratch), 1000,
                                  100, &iov1) == 12);
  assert(iov1.dscp == 46U && iov1.path_entropy == entropy1);
  assert(rudp_iovec_flatten(&iov1, flat_iov, sizeof(flat_iov)) == 12);

  /* Distinct session seed produces distinct multipath entropy while identical wire payload */
  assert(rudp_session_prepare_iovec(&s2, 7U, 1000, 100, flat_direct,
                                    sizeof(flat_direct), &iov2, &plan) == 12);
  assert(iov2.path_entropy != entropy1);
  assert(rudp_session_cancel_datagram(&s2, &plan) == RUDP_OK);
  assert(rudp_session_build_datagram(&s2, 0, flat_direct, sizeof(flat_direct),
                                     1000, 100) == 12);
  assert(memcmp(flat_iov, flat_direct, 12) == 0);
  puts("PASS: Scatter-gather build_iovec/prepare_iovec, DSCP=46 propagation, path_entropy, 5364B ABI");
}

/* 8. Full 63-slot extended SACK (offsets 33..63) + post-SRTT chan_backoff scoping */
static void test_extended_sack_63slot_and_post_srtt_backoff(void) {
  rudp_session_s tx, rx;
  assert(rudp_session_init(&tx) == RUDP_OK);
  assert(rudp_session_init(&rx) == RUDP_OK);
  assert(rudp_session_config_recovery(&tx, 0, 100, 10, 2000) == RUDP_OK);
  assert(rudp_session_config_recovery(&rx, 0, 100, 10, 2000) == RUDP_OK);

  uint8_t dgrams[45][16];
  for (uint16_t i = 0U; i < 45U; ++i) {
    assert(rudp_session_send_reliable(
               &tx, 0, (tfv_packet_u){.type = 1, .value = (uint16_t)(200U + i)},
               100) == RUDP_OK);
    assert(rudp_session_build_datagram(&tx, 0, dgrams[i], sizeof(dgrams[i]),
                                       100, 1) == 12);
  }

  /* Drop seq 0 and seq 36; deliver seq 1..35 and 37..40 at t=120. */
  rudp_record_s recs[8];
  rudp_stats_s tx_stats = {0}, rx_stats = {0};
  for (unsigned i = 1U; i <= 40U; ++i) {
    if (i == 36U) {
      continue;
    }
    assert(rudp_session_process_datagram_ex(&rx, dgrams[i], 12, recs, 8, true,
                                            120, &rx_stats) == 0);
  }

  /* Expect 20-byte ACK: 4B header + 8B primary SACK (seq 0) + 8B extended SACK (seq 32). */
  uint8_t ack_wire[32];
  int alen = rudp_session_build_datagram(&rx, 0, ack_wire, sizeof(ack_wire), 120, 100);
  assert(alen == 20);
  assert(rudp_session_process_datagram_ex(&tx, ack_wire, (size_t)alen, recs, 8,
                                          true, 140, &tx_stats) == 0);

  for (unsigned i = 33U; i <= 40U; ++i) {
    if (i == 36U) {
      assert(tx.channels[0].ctx.tx_buffer[i].state == RUDP_SLOT_IN_FLIGHT);
    } else {
      assert(tx.channels[0].ctx.tx_buffer[i].state == RUDP_SLOT_SACKED);
    }
  }
  assert(rudp_slot_fast_state(&tx.channels[0].ctx.tx_buffer[0]) ==
         RUDP_FAST_RETRANSMIT_PENDING);
  assert(rudp_slot_fast_state(&tx.channels[0].ctx.tx_buffer[36]) ==
         RUDP_FAST_RETRANSMIT_PENDING);

  uint8_t repair[64];
  assert(rudp_session_build_datagram(&tx, 0, repair, sizeof(repair), 140, 100) == 20);

  /* Post-SRTT chan_backoff scoping: once srtt_scaled > 0, fresh slots do not inherit
   * backoff from an earlier slot timeout, and cumulative advance clears chan_backoff. */
  rudp_session_s s2;
  uint8_t out[32], ack_pkt[4];
  assert(rudp_session_init(&s2) == RUDP_OK);
  assert(rudp_session_config_recovery(&s2, 0, 100, 10, 2000) == RUDP_OK);
  assert(rudp_session_send_reliable(&s2, 0, (tfv_packet_u){.type = 1, .value = 1}, 0) ==
         RUDP_OK);
  assert(rudp_session_build_datagram(&s2, 0, out, sizeof(out), 0, 100) == 12);

  rudp_datagram_header_s h = {.ack = 1, .ack_channel = 0, .count = 0};
  assert(rudp_pack_datagram_header(&h, ack_pkt, sizeof(ack_pkt)) == 4);
  assert(rudp_session_process_datagram_at(&s2, ack_pkt, sizeof(ack_pkt), NULL, 0, 50) ==
         0);
  assert(s2.channels[0].srtt_scaled == (50U << 3));
  assert(s2.channels[0].rto_ms == 150U);

  assert(rudp_session_send_reliable(&s2, 0, (tfv_packet_u){.type = 1, .value = 2},
                                    100) == RUDP_OK);
  assert(rudp_session_build_datagram(&s2, 0, out, sizeof(out), 100, 100) == 12);
  assert(rudp_session_send_reliable(&s2, 0, (tfv_packet_u){.type = 1, .value = 3},
                                    150) == RUDP_OK);
  assert(rudp_session_build_datagram(&s2, 0, out, sizeof(out), 150, 100) == 12);
  /* Keep >=3 unacked packets above seq 2 so TLP stays inactive until t=301. */
  for (uint16_t k = 0U; k < 3U; ++k) {
    assert(rudp_session_send_reliable(
               &s2, 0, (tfv_packet_u){.type = 1, .value = (uint16_t)(10U + k)},
               230) == RUDP_OK);
    assert(rudp_session_build_datagram(&s2, 0, out, sizeof(out), 230, 100) == 12);
  }

  /* seq 1 times out at t=251 (elapsed 151 > 150). */
  assert(rudp_session_build_datagram(&s2, 0, out, sizeof(out), 251, 100) == 12);
  assert((s2.channels[0].chan_backoff & 0x0FU) == 1U);

  /* New seq 6 and un-retransmitted seq 2 keep backoff=0; seq 2 times out at t=301 (1x RTO). */
  assert(rudp_session_send_reliable(&s2, 0, (tfv_packet_u){.type = 1, .value = 4},
                                    260) == RUDP_OK);
  assert(rudp_slot_backoff(&s2.channels[0].ctx.tx_buffer[6]) == 0U);
  assert(rudp_session_build_datagram(&s2, 0, out, sizeof(out), 260, 100) == 12);
  assert(rudp_slot_backoff(&s2.channels[0].ctx.tx_buffer[6]) == 0U);
  assert(rudp_session_build_datagram(&s2, 0, out, sizeof(out), 300, 100) == 4);
  assert(rudp_session_build_datagram(&s2, 0, out, sizeof(out), 301, 100) == 12);

  /* Cumulative ACK advancing past retransmitted seq 1..2 clears chan_backoff post-SRTT. */
  h.ack = 3;
  assert(rudp_pack_datagram_header(&h, ack_pkt, sizeof(ack_pkt)) == 4);
  assert(rudp_session_process_datagram_at(&s2, ack_pkt, sizeof(ack_pkt), NULL, 0, 320) ==
         0);
  assert((s2.channels[0].chan_backoff & 0x0FU) == 0U);

  puts("PASS: Extended 63-slot SACK + FACK repair and post-SRTT chan_backoff isolation");
}

int main(void) {
  test_rfc8439_and_envelope();
  test_custom_aead_vtable();
  test_sack_fack_multiloss_and_karn();
  test_tail_loss_probe();
  test_reliable_unordered_zero_hol();
  test_fec_k2_zero_rtt_recovery();
  test_motion_dead_reckoning();
  test_scatter_gather_seed_and_abi();
  test_extended_sack_63slot_and_post_srtt_backoff();
  return 0;
}
