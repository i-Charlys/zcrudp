#include "protocol_profiles.h"
#include "protocol_rudp.h"
#include "rudp_nextgen.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_abi_sizes(void) {
  assert(sizeof(rudp_datagram_header_s) == 4U);
  assert(sizeof(rudp_record_s) == 8U);
  assert(sizeof(rudp_context_s) == 1040U);
  assert(sizeof(rudp_channel_s) == 1340U);
  assert(sizeof(rudp_session_s) == 5364U);
}

static void test_pr_sctp_forward_seq(void) {
  rudp_session_s tx, rx;
  assert(rudp_session_init(&tx) == RUDP_OK);
  assert(rudp_session_init(&rx) == RUDP_OK);

  uint8_t flags = (uint8_t)(RUDP_CHANNEL_FLAG_RELIABLE |
                            RUDP_CHANNEL_FLAG_ORDERED |
                            RUDP_CHANNEL_FLAG_PR_SCTP);
  assert(rudp_session_config_channel(&tx, 0U, flags) == RUDP_OK);
  assert(rudp_session_config_channel(&rx, 0U, flags) == RUDP_OK);

  /* Queue seq 0 at t=100, and seq 1, 2 at t=190 */
  tfv_packet_u p0 = {.type = 1U, .flags = 0U, .value = 100U};
  tfv_packet_u p1 = {.type = 1U, .flags = 0U, .value = 101U};
  tfv_packet_u p2 = {.type = 1U, .flags = 0U, .value = 102U};
  assert(rudp_session_send_reliable(&tx, 0U, p0, 100U) == RUDP_OK);
  assert(rudp_session_send_reliable(&tx, 0U, p1, 190U) == RUDP_OK);
  assert(rudp_session_send_reliable(&tx, 0U, p2, 190U) == RUDP_OK);

  /* Transmit seq 0, 1, 2 on wire, but simulate seq 0 lost and seq 1,2 arriving */
  uint8_t d0[12], d1[12], d2[12];
  assert(rudp_session_build_datagram(&tx, 0U, d0, sizeof(d0), 100U, 500U) == 12);
  assert(rudp_session_build_datagram(&tx, 0U, d1, sizeof(d1), 190U, 500U) == 12);
  assert(rudp_session_build_datagram(&tx, 0U, d2, sizeof(d2), 190U, 500U) == 12);

  rudp_record_s out[8];
  /* Delivering seq 1 and seq 2 out-of-order on ordered channel buffers them (HoL) */
  assert(rudp_session_process_datagram(&rx, d1, sizeof(d1), out, 8U) == 0);
  assert(rudp_session_process_datagram(&rx, d2, sizeof(d2), out, 8U) == 0);
  assert(rx.channels[0].ctx.expected_seq_num == 0U);

  /* At now=200 with max_age_ms=50, seq 0 (age 100) expires while seq 1,2 (age 10) stay */
  int expired = rudp_session_expire_reliable(&tx, 0U, 200U, 50U, 5U);
  assert(expired == 1);
  assert(tx.channels[0].ctx.tx_buffer[0].state == RUDP_SLOT_FWD_SEQ);

  /* Build datagram carrying FORWARD-SEQ for seq 0 */
  uint8_t fwd_wire[64];
  int fwd_len = rudp_session_build_datagram(&tx, 0U, fwd_wire, sizeof(fwd_wire), 200U, 50U);
  assert(fwd_len >= 12);

  rudp_datagram_header_s hdr;
  assert(rudp_unpack_datagram_header(fwd_wire, (size_t)fwd_len, &hdr) == RUDP_OK);
  int found_fwd = 0;
  for (uint8_t i = 0U; i < hdr.count; ++i) {
    rudp_record_s r;
    assert(rudp_unpack_record(fwd_wire + 4U + (size_t)i * 8U, 8U, &r) == RUDP_OK);
    if (r.flags & RUDP_RECORD_FLAG_FWD_SEQ) {
      found_fwd = 1;
      assert(r.seq_num == 1U);
    }
  }
  assert(found_fwd == 1);

  /* Deliver FORWARD-SEQ to receiver -> unblocks seq 1 and seq 2 in order */
  int delivered = rudp_session_process_datagram(&rx, fwd_wire, (size_t)fwd_len, out, 8U);
  assert(delivered == 2);
  assert(out[0].payload.value == 101U);
  assert(out[1].payload.value == 102U);
  assert(rx.channels[0].ctx.expected_seq_num == 3U);

  /* Duplicate or stale/out-of-window FORWARD-SEQ must not advance or crash */
  assert(rudp_session_process_datagram(&rx, fwd_wire, (size_t)fwd_len, out, 8U) == 0);
  assert(rx.channels[0].ctx.expected_seq_num == 3U);

  uint8_t stale_fwd[12];
  rudp_datagram_header_s shdr = {.ack = 0U, .ack_channel = 0U, .count = 1U};
  rudp_record_s srec = {.channel_id = 0U,
                        .flags = RUDP_RECORD_FLAG_FWD_SEQ,
                        .seq_num = 5000U,
                        .payload = {.type = RUDP_FWD_SEQ_REASON_MANUAL, .flags = 0U, .value = 0U}};
  assert(rudp_pack_datagram_header(&shdr, stale_fwd, sizeof(stale_fwd)) == 4);
  assert(rudp_pack_record(&srec, stale_fwd + 4, 8U) == 8);
  assert(rudp_session_process_datagram(&rx, stale_fwd, sizeof(stale_fwd), out, 8U) == 0);
  assert(rx.channels[0].ctx.expected_seq_num == 3U);

  /* Test retry expiration + manual abandon on an isolated session */
  rudp_session_s tx_retx;
  assert(rudp_session_init(&tx_retx) == RUDP_OK);
  assert(rudp_session_config_channel(&tx_retx, 1U, flags) == RUDP_OK);
  assert(rudp_session_send_reliable(&tx_retx, 1U, p0, 1000U) == RUDP_OK);
  assert(rudp_session_send_reliable(&tx_retx, 1U, p1, 1000U) == RUDP_OK);
  uint8_t tmp[64];
  assert(rudp_session_build_datagram(&tx_retx, 1U, tmp, sizeof(tmp), 1000U, 10U) > 0);
  assert(rudp_session_build_datagram(&tx_retx, 1U, tmp, sizeof(tmp), 1020U, 10U) > 0);
  assert(rudp_session_build_datagram(&tx_retx, 1U, tmp, sizeof(tmp), 1060U, 10U) > 0);
  assert(rudp_session_expire_reliable(&tx_retx, 1U, 1060U, 5000U, 2U) == 2);
  assert(rudp_session_send_reliable(&tx_retx, 1U, p2, 1060U) == RUDP_OK);
  assert(rudp_session_build_datagram(&tx_retx, 1U, tmp, sizeof(tmp), 1060U, 500U) > 0);
  int abandoned = rudp_session_abandon_reliable(&tx_retx, 1U, 3U, RUDP_FWD_SEQ_REASON_MANUAL);
  assert(abandoned == 1);
  assert(tx_retx.channels[1].ctx.tx_buffer[2].state == RUDP_SLOT_FWD_SEQ);
}

static void test_xtp_sreq_and_fastnak(void) {
  rudp_session_s tx, rx;
  assert(rudp_session_init(&tx) == RUDP_OK);
  assert(rudp_session_init(&rx) == RUDP_OK);

  uint8_t xtp_flags = (uint8_t)(RUDP_CHANNEL_FLAG_RELIABLE |
                                RUDP_CHANNEL_FLAG_ORDERED |
                                RUDP_CHANNEL_FLAG_XTP_SREQ);
  assert(rudp_session_config_channel(&tx, 0U, xtp_flags) == RUDP_OK);
  assert(rudp_session_config_channel(
             &rx, 0U, (uint8_t)(xtp_flags | RUDP_CHANNEL_FLAG_FASTNAK)) == RUDP_OK);

  for (uint16_t i = 0U; i < 8U; ++i) {
    tfv_packet_u p = {.type = 2U, .flags = 0U, .value = (uint16_t)(200U + i)};
    assert(rudp_session_send_reliable(&tx, 0U, p, 10U) == RUDP_OK);
  }

  /* Emit 1 record per datagram (12 bytes): first 7 should NOT set SREQ/DREQ, 8th MUST */
  uint8_t pkts[8][12];
  for (int i = 0; i < 8; ++i) {
    assert(rudp_session_build_datagram(&tx, 0U, pkts[i], 12U, 10U, 100U) == 12);
    rudp_record_s r;
    assert(rudp_unpack_record(pkts[i] + 4, 8U, &r) == RUDP_OK);
    if (i < 7) {
      assert((r.flags & (RUDP_RECORD_FLAG_SREQ | RUDP_RECORD_FLAG_DREQ)) == 0U);
    } else {
      assert((r.flags & RUDP_RECORD_FLAG_SREQ) != 0U);
    }
  }

  /* Receiving packets 0..6 in order under XTP_SREQ suppresses eager ack_pending */
  rudp_record_s out[8];
  for (int i = 0; i < 7; ++i) {
    assert(rudp_session_process_datagram(&rx, pkts[i], 12U, out, 8U) == 1);
    assert(out[0].payload.value == (uint16_t)(200U + i));
    assert(rx.channels[0].ack_pending == 0U);
  }
  /* Receiving end-of-burst packet 7 (SREQ) triggers ack_pending == 1 */
  assert(rudp_session_process_datagram(&rx, pkts[7], 12U, out, 8U) == 1);
  assert(out[0].payload.value == 207U);
  assert(rx.channels[0].ack_pending == 1U);

  /* FASTNAK: gap on expected seq 8 when seq 9 arrives sets ack_pending immediately */
  rx.channels[0].ack_pending = 0U;
  uint8_t gap_pkt[12];
  rudp_datagram_header_s gh = {.ack = 0U, .ack_channel = 0U, .count = 1U};
  rudp_record_s gr = {.channel_id = 0U,
                      .flags = RUDP_RECORD_FLAG_RELIABLE,
                      .seq_num = 9U,
                      .payload = {.type = 2U, .flags = 0U, .value = 209U}};
  assert(rudp_pack_datagram_header(&gh, gap_pkt, sizeof(gap_pkt)) == 4);
  assert(rudp_pack_record(&gr, gap_pkt + 4, 8U) == 8);
  assert(rudp_session_process_datagram(&rx, gap_pkt, sizeof(gap_pkt), out, 8U) == 0);
  assert(rx.channels[0].ack_pending == 1U);
}

static void test_dccp_ack_vector_and_ecn(void) {
  rudp_ackvec_s vec;
  assert(rudp_ackvec_init(&vec, 65500U) == RUDP_OK);

  /* Test push_run across > 63 run split and 16-bit wraparound */
  assert(rudp_ackvec_push_run(&vec, RUDP_ACKVEC_RECV, 140U) == RUDP_OK);
  assert(rudp_ackvec_push_run(&vec, RUDP_ACKVEC_LOST, 2U) == RUDP_OK);
  assert(rudp_ackvec_push_run(&vec, RUDP_ACKVEC_RECV_CE, 5U) == RUDP_OK);
  assert(rudp_ackvec_push_run(&vec, RUDP_ACKVEC_LOST_DROPPED, 1U) == RUDP_OK);

  uint16_t next_idx = 0U;
  rudp_record_s rec;
  assert(rudp_ackvec_encode_record(&vec, 0U, 0U, &rec, &next_idx) == RUDP_OK);
  assert((rec.flags & (RUDP_RECORD_FLAG_ACK | RUDP_RECORD_FLAG_ACK_VEC)) ==
         (RUDP_RECORD_FLAG_ACK | RUDP_RECORD_FLAG_ACK_VEC));

  rudp_ackvec_summary_s sum;
  assert(rudp_ackvec_decode_record(&rec, &sum) == RUDP_OK);
  assert(sum.start_seq == 65500U);

  /* Decode remaining chunks to verify full run coverage including ECN-CE */
  uint32_t total_recv = sum.acked_count;
  uint32_t total_ce = sum.ecn_ce_count;
  uint32_t total_lost = sum.lost_count;
  uint32_t total_drop = sum.dropped_count;
  while (next_idx < vec.run_count) {
    assert(rudp_ackvec_encode_record(&vec, 0U, next_idx, &rec, &next_idx) == RUDP_OK);
    assert(rudp_ackvec_decode_record(&rec, &sum) == RUDP_OK);
    total_recv += sum.acked_count;
    total_ce += sum.ecn_ce_count;
    total_lost += sum.lost_count;
    total_drop += sum.dropped_count;
  }
  assert(total_recv == 140U || total_recv == 145U);
  assert(total_lost == 2U);
  assert(total_ce == 5U);
  assert(total_drop == 1U);

  /* Apply an Ack Vector record carrying ECN-CE to pacer and confirm rate drop */
  rudp_ackvec_s ce_vec;
  assert(rudp_ackvec_init(&ce_vec, 10U) == RUDP_OK);
  assert(rudp_ackvec_record(&ce_vec, 10U, RUDP_ACKVEC_RECV_CE) == RUDP_OK);
  assert(rudp_ackvec_record(&ce_vec, 11U, RUDP_ACKVEC_RECV_CE) == RUDP_OK);
  rudp_record_s ce_rec;
  assert(rudp_ackvec_encode_record(&ce_vec, 0U, 0U, &ce_rec, &next_idx) == RUDP_OK);

  rudp_pacer_s pacer;
  assert(rudp_pacer_init(&pacer, 1000000U, 50000U, 4000000U, 5000U, 1000ULL) == RUDP_OK);
  assert(rudp_pacer_on_owd_sample(&pacer, 5000U, 1000U, 0U) == RUDP_OK);
  uint32_t rate_before = pacer.rate_bytes_per_sec;
  assert(rudp_ackvec_apply_to_pacer(&ce_rec, &pacer, 15000U, 1200U) == RUDP_OK);
  assert(pacer.rate_bytes_per_sec < rate_before);

  /* Full session integration: Ack Vector SACK + FACK retransmit trigger */
  rudp_session_s tx;
  assert(rudp_session_init(&tx) == RUDP_OK);
  assert(rudp_session_config_channel(
             &tx, 0U,
             (uint8_t)(RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED |
                       RUDP_CHANNEL_FLAG_ACK_VEC)) == RUDP_OK);
  for (uint16_t i = 0U; i < 6U; ++i) {
    tfv_packet_u p = {.type = 3U, .flags = 0U, .value = (uint16_t)(300U + i)};
    assert(rudp_session_send_reliable(&tx, 0U, p, 100U) == RUDP_OK);
  }
  uint8_t burst[64];
  assert(rudp_session_build_datagram(&tx, 0U, burst, sizeof(burst), 100U, 500U) > 0);

  /* Peer sends ACK_VEC: seq 0 RECV, seq 1 LOST, seq 2..5 RECV */
  rudp_ackvec_s fb;
  assert(rudp_ackvec_init(&fb, 0U) == RUDP_OK);
  assert(rudp_ackvec_record(&fb, 0U, RUDP_ACKVEC_RECV) == RUDP_OK);
  assert(rudp_ackvec_record(&fb, 1U, RUDP_ACKVEC_LOST) == RUDP_OK);
  for (uint16_t s = 2U; s < 6U; ++s) {
    assert(rudp_ackvec_record(&fb, s, RUDP_ACKVEC_RECV) == RUDP_OK);
  }
  rudp_record_s fb_rec;
  assert(rudp_ackvec_encode_record(&fb, 0U, 0U, &fb_rec, &next_idx) == RUDP_OK);
  assert(next_idx == fb.run_count);

  uint8_t ack_wire[12];
  rudp_datagram_header_s ah = {.ack = 0U, .ack_channel = 0U, .count = 1U};
  assert(rudp_pack_datagram_header(&ah, ack_wire, sizeof(ack_wire)) == 4);
  assert(rudp_pack_record(&fb_rec, ack_wire + 4, 8U) == 8);

  rudp_record_s dummy[4];
  assert(rudp_session_process_datagram_ex(&tx, ack_wire, sizeof(ack_wire), dummy, 4U,
                                          true, 120U, NULL) == 0);
  /* Seq 2..5 should be marked SACKED, triggering FACK fast retransmit of seq 1 */
  assert(tx.channels[0].ctx.tx_buffer[2].state == RUDP_SLOT_SACKED);
  assert(tx.channels[0].ctx.tx_buffer[5].state == RUDP_SLOT_SACKED);

  uint8_t retx[32];
  int retx_len = rudp_session_build_datagram(&tx, 0U, retx, sizeof(retx), 120U, 10000U);
  assert(retx_len == 12);
  rudp_record_s retx_rec;
  assert(rudp_unpack_record(retx + 4, 8U, &retx_rec) == RUDP_OK);
  assert(retx_rec.seq_num == 1U);
}

int main(void) {
  test_abi_sizes();
  test_pr_sctp_forward_seq();
  test_xtp_sreq_and_fastnak();
  test_dccp_ack_vector_and_ecn();
  printf("Phase 8 tests passed (ABI + PR-SCTP FWD-SEQ + XTP SREQ/FASTNAK + DCCP AckVec/ECN)\n");
  return 0;
}
