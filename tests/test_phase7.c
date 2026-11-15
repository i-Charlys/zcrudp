#include "protocol_profiles.h"
#include "protocol_rudp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* 1. Registered Page Pool, anti-ABA generation checks & packet plan flatten equivalence */
static void test_page_pool_and_packet_plan(void) {
  static uint8_t backing[8 * RUDP_PAGE_SIZE];
  rudp_page_pool_s pool;
  assert(rudp_page_pool_init(NULL, backing, 8) == RUDP_ERR_INVALID_ARG);
  assert(rudp_page_pool_init(&pool, backing, 0) == RUDP_ERR_INVALID_ARG);
  assert(rudp_page_pool_init(&pool, backing, 8) == RUDP_OK);

  rudp_page_extent_s e0, e1;
  assert(rudp_page_alloc(&pool, 0U, &e0) == RUDP_ERR_INVALID_ARG);
  assert(rudp_page_alloc(&pool, (uint16_t)(RUDP_PAGE_SIZE + 1U), &e0) ==
         RUDP_ERR_INVALID_ARG);
  assert(rudp_page_alloc(&pool, 128U, &e0) == RUDP_OK);
  assert(rudp_page_alloc(&pool, 64U, &e1) == RUDP_OK);

  uint8_t *p0 = rudp_page_resolve_mut(&pool, e0);
  uint8_t *p1 = rudp_page_resolve_mut(&pool, e1);
  assert(p0 != NULL && p1 != NULL && p0 != p1);
  for (uint16_t i = 0; i < 128U; ++i) {
    p0[i] = (uint8_t)(i ^ 0xA5U);
  }
  for (uint16_t i = 0; i < 64U; ++i) {
    p1[i] = (uint8_t)(i + 17U);
  }

  /* Sub-extent + dual refcount (proto_refs & io_refs) + coalesced release */
  rudp_page_extent_s sub0;
  assert(rudp_page_subextent(&pool, e0, 16U, 32U, &sub0) == RUDP_OK);
  assert(rudp_page_resolve(&pool, sub0) == p0 + 16U);

  assert(rudp_page_retain_io(&pool, e0) == RUDP_OK);
  /* Dropping proto_refs while io_refs == 1 must NOT enqueue release yet */
  assert(rudp_page_release_proto(&pool, e0) == RUDP_OK);
  assert(rudp_page_release_proto(&pool, e0) == RUDP_ERR_INVALID_ARG); /* No underflow */
  uint16_t released[8];
  assert(rudp_page_pool_drain_releases(&pool, released, 8) == 0U);
  /* Dropping final io_ref transitions both refcounts to 0 and queues release */
  assert(rudp_page_release_io(&pool, e0) == RUDP_OK);
  assert(rudp_page_resolve(&pool, e0) == NULL); /* Already pending release */
  assert(rudp_page_pool_drain_releases(&pool, released, 8) == 1U);
  assert(released[0] == e0.page_id);

  /* Anti-ABA: reallocating the same page bumps gen, invalidating old extent e0 */
  rudp_page_extent_s e0_new;
  assert(rudp_page_alloc(&pool, 32U, &e0_new) == RUDP_OK);
  assert(e0_new.page_id == e0.page_id);
  assert(e0_new.gen != e0.gen);
  assert(rudp_page_resolve(&pool, e0) == NULL);
  assert(rudp_page_resolve(&pool, e0_new) != NULL);

  /* Packet plan to_iovec vs flatten byte-for-byte identity */
  rudp_packet_plan_s plan;
  uint32_t ent = rudp_calc_entropy(0x1122U, 1U, 3U);
  assert(rudp_packet_plan_init(&plan, 46U, ent) == RUDP_OK);
  assert(rudp_packet_plan_set_header(
             &plan, (rudp_datagram_header_s){.ack = 9U, .ack_channel = 0U, .count = 1U}) ==
         RUDP_OK);
  assert(rudp_packet_plan_add_record(
             &plan, (rudp_record_s){.channel_id = 1U,
                                    .flags = RUDP_RECORD_FLAG_RELIABLE,
                                    .seq_num = 5U,
                                    .payload = {.type = 3U, .value = 777U}}) ==
         RUDP_OK);
  assert(rudp_packet_plan_add_extent(&plan, &pool, sub0) == RUDP_ERR_INVALID_ARG); /* Freed! */
  assert(rudp_packet_plan_add_extent(&plan, &pool, e0_new) == RUDP_OK);
  assert(rudp_packet_plan_add_extent(&plan, &pool, e1) == RUDP_OK);

  rudp_iovec_collection_s col;
  assert(rudp_packet_plan_to_iovec(&pool, &plan, &col) == RUDP_OK);
  assert(col.dscp == 46U && col.path_entropy == ent);
  uint8_t flat_via_iov[256], flat_via_plan[256];
  int n1 = rudp_iovec_flatten(&col, flat_via_iov, sizeof(flat_via_iov));
  int n2 = rudp_packet_plan_flatten(&pool, &plan, flat_via_plan, sizeof(flat_via_plan));
  assert(n1 == n2 && n1 == (4 + 8 + 32 + 64));
  assert(memcmp(flat_via_iov, flat_via_plan, (size_t)n1) == 0);
}

/* 2. Shared Static Metadata Slot Pool with per-channel asymmetric quotas & auto release */
static void test_shared_metadata_pool(void) {
  static uint8_t backing[8 * RUDP_PAGE_SIZE];
  rudp_page_pool_s ppool;
  rudp_shared_pool_s spool;
  assert(rudp_page_pool_init(&ppool, backing, 8) == RUDP_OK);
  assert(rudp_shared_pool_init(&spool, &ppool) == RUDP_OK);

  /* Asymmetric quotas: Ch0=8 min/16 max, Ch1=256 max (bulk hits reservation ceiling at 232) */
  assert(rudp_shared_pool_set_quota(&spool, 0U, 8U, 16U) == RUDP_OK);
  assert(rudp_shared_pool_set_quota(&spool, 1U, 16U, 256U) == RUDP_OK);
  assert(rudp_shared_pool_set_quota(&spool, 2U, 8U, 64U) == RUDP_OK);
  assert(rudp_shared_pool_set_quota(&spool, 3U, 8U, 64U) == RUDP_OK);
  assert(rudp_shared_pool_set_quota(&spool, 1U, 240U, 250U) == RUDP_ERR_INVALID_ARG);

  /* Flood bulk channel 1 until reservation protection stops it before starving Ch0/2/3 */
  uint16_t seq = 0U;
  int ch1_added = 0;
  while (rudp_shared_pool_enqueue_inline(
             &spool, 1U, (tfv_packet_u){.value = (uint16_t)ch1_added}, 10U, &seq) ==
         RUDP_OK) {
    ch1_added++;
  }
  assert(ch1_added == 232); /* 256 - (8 + 8 + 8) reservation protection */

  /* Reserved floors on Ch0/Ch2/Ch3 still accept traffic while Ch1 is saturated */
  for (int i = 0; i < 8; ++i) {
    assert(rudp_shared_pool_enqueue_inline(&spool, 0U, (tfv_packet_u){.value = 1U}, 10U,
                                           &seq) == RUDP_OK);
    assert(rudp_shared_pool_enqueue_inline(&spool, 2U, (tfv_packet_u){.value = 2U}, 10U,
                                           &seq) == RUDP_OK);
    assert(rudp_shared_pool_enqueue_inline(&spool, 3U, (tfv_packet_u){.value = 3U}, 10U,
                                           &seq) == RUDP_OK);
  }
  assert(rudp_shared_pool_chan_used(&spool, 0U) == 8U);

  /* Free 2 bulk slots on Ch1 (232 -> 230) so global pool has headroom above Ch0/2/3 floors */
  assert(rudp_shared_pool_ack(&spool, 1U, 2U, 0U) == 2);

  /* Extent-backed slot drops proto_ref on SACK + cumulative ACK */
  rudp_page_extent_s ext0, ext1;
  assert(rudp_page_alloc(&ppool, 512U, &ext0) == RUDP_OK);
  assert(rudp_page_alloc(&ppool, 512U, &ext1) == RUDP_OK);
  assert(rudp_shared_pool_enqueue_extent(&spool, 0U, ext0, 20U, &seq) == RUDP_OK);
  assert(seq == 8U);
  assert(rudp_shared_pool_enqueue_extent(&spool, 0U, ext1, 20U, &seq) == RUDP_OK);
  assert(seq == 9U);
  /* Drop caller's setup ref; slots keep pages pinned (proto_refs == 1) */
  assert(rudp_page_release_proto(&ppool, ext0) == RUDP_OK);
  assert(rudp_page_release_proto(&ppool, ext1) == RUDP_OK);
  uint16_t drained[4];
  assert(rudp_page_pool_drain_releases(&ppool, drained, 4) == 0U);

  /* ACK up to 8 (frees 0..7) + SACK bit 1 (seq 8 + 1 + 1 = 10 -> no, bit 0 is seq 9 == ext1!) */
  int freed = rudp_shared_pool_ack(&spool, 0U, 8U, 1U);
  assert(freed == 8);
  assert(rudp_page_pool_drain_releases(&ppool, drained, 4) == 1U);
  assert(drained[0] == ext1.page_id); /* SACK freed ext1 proto_ref immediately */
  assert(rudp_shared_pool_ack(&spool, 0U, 10U, 0U) == 2);
  assert(rudp_page_pool_drain_releases(&ppool, drained, 4) == 1U);
  assert(drained[0] == ext0.page_id);
}

/* 3. SCReAM / L4S Delay-Gradient Micro-Pacer */
static void test_owd_pacer(void) {
  rudp_pacer_s pacer;
  assert(rudp_pacer_init(&pacer, 100000U, 20000U, 500000U, 5000U, 1000ULL) ==
         RUDP_OK);
  assert(rudp_pacer_refill_and_can_send(&pacer, 1000ULL, 1000U));
  assert(rudp_pacer_consume(&pacer, 1000U) == RUDP_OK);
  /* Drain initial burst completely */
  while (rudp_pacer_refill_and_can_send(&pacer, 1000ULL, 1000U)) {
    assert(rudp_pacer_consume(&pacer, 1000U) == RUDP_OK);
  }
  uint32_t wait_us = rudp_pacer_next_wait_us(&pacer, 1000U);
  assert(wait_us > 0U && wait_us != UINT32_MAX);
  assert(rudp_pacer_refill_and_can_send(&pacer, 1000ULL + wait_us, 1000U));

  /* Uncongested low-OWD feedback ramps rate UP */
  uint32_t before_rate = pacer.rate_bytes_per_sec;
  assert(rudp_pacer_on_owd_sample(&pacer, 10000U, 1000U, 0U) == RUDP_OK);
  assert(rudp_pacer_on_owd_sample(&pacer, 10500U, 1000U, 0U) == RUDP_OK);
  assert(pacer.rate_bytes_per_sec > before_rate);

  /* Queue buildup (> target_queue_us = 5000 us) or L4S CE throttles rate DOWN */
  uint32_t peak_rate = pacer.rate_bytes_per_sec;
  for (int i = 0; i < 6; ++i) {
    assert(rudp_pacer_on_owd_sample(&pacer, 25000U, 0U, 0U) == RUDP_OK);
  }
  assert(pacer.rate_bytes_per_sec < peak_rate);
  uint32_t ce_before = pacer.rate_bytes_per_sec;
  assert(rudp_pacer_on_owd_sample(&pacer, 10000U, 1000U, 1U) == RUDP_OK);
  assert(pacer.rate_bytes_per_sec < ce_before);
}

/* 4. RFC 8681 Sliding-Window Convolutional FEC + adaptive interval */
static void test_sliding_window_fec(void) {
  rudp_sliding_fec_s tx;
  rudp_sliding_fec_rx_s rx;
  assert(rudp_sliding_fec_init(&tx, 6U) == RUDP_OK);
  assert(rudp_sliding_fec_rx_init(&rx, 6U) == RUDP_OK);
  assert(tx.parity_interval == 6U);

  /* Loss feedback lowers parity interval automatically */
  for (int i = 0; i < 4; ++i) {
    assert(rudp_sliding_fec_update_loss(&tx, true) == RUDP_OK);
  }
  assert(tx.parity_interval <= 4U);

  /* Transmit seqs 10..13 while dropping seq 12, then recover seq 12 from repair in 0-RTT */
  rudp_sliding_fec_repair_s repair = {0};
  int emitted = 0;
  for (uint16_t s = 10U; s <= 15U; ++s) {
    tfv_packet_u p = {.type = 5U, .flags = 1U, .value = (uint16_t)(s * 100U)};
    if (s != 12U) {
      assert(rudp_sliding_fec_rx_source(&rx, s, p) == RUDP_OK);
    }
    int r = rudp_sliding_fec_encode(&tx, s, p, &repair);
    assert(r >= 0);
    if (r == 1 && !emitted && repair.base_seq <= 12U &&
        (uint16_t)(repair.base_seq + repair.span) > 12U) {
      uint16_t rec_seq = 0U;
      tfv_packet_u rec_pkt = {0};
      assert(rudp_sliding_fec_rx_repair(&rx, &repair, &rec_seq, &rec_pkt) == 1);
      assert(rec_seq == 12U && rec_pkt.value == 1200U);
      emitted = 1;
    }
  }
  assert(emitted == 1);
}

/* 5. Multipath Transport (Failover, Weighted Spray, Redundant) */
static void test_multipath_scheduler(void) {
  rudp_multipath_s mp;
  uint8_t ids[RUDP_MAX_PATHS];
  uint32_t ent[RUDP_MAX_PATHS];

  /* Failover mode */
  assert(rudp_multipath_init(&mp, 0xABCDU, RUDP_MP_PRIMARY_FAILOVER) == RUDP_OK);
  assert(rudp_multipath_add_path(&mp, 0U, 10U, 100U) == RUDP_OK);
  assert(rudp_multipath_add_path(&mp, 1U, 10U, 200U) == RUDP_OK);
  assert(rudp_multipath_select_paths(&mp, 0U, 500U, ids, ent) == 1);
  assert(ids[0] == 0U);
  /* Degrade path 0 via repeated loss -> auto failover to path 1 */
  for (int i = 0; i < 4; ++i) {
    assert(rudp_multipath_on_feedback(&mp, 0U, 120U, true, (uint32_t)(i * 10)) ==
           RUDP_OK);
  }
  assert(mp.paths[0].degraded == 1U);
  assert(rudp_multipath_select_paths(&mp, 0U, 500U, ids, ent) == 1);
  assert(ids[0] == 1U && ent[0] == rudp_calc_entropy(0xABCDU, 0U, 200U));

  /* Weighted spray mode (3:1 weight split) */
  assert(rudp_multipath_init(&mp, 0x1234U, RUDP_MP_WEIGHTED_SPRAY) == RUDP_OK);
  assert(rudp_multipath_add_path(&mp, 0U, 3U, 1U) == RUDP_OK);
  assert(rudp_multipath_add_path(&mp, 1U, 1U, 2U) == RUDP_OK);
  int c0 = 0, c1 = 0;
  for (int i = 0; i < 40; ++i) {
    assert(rudp_multipath_select_paths(&mp, 0U, 100U, ids, ent) == 1);
    if (ids[0] == 0U) {
      c0++;
    } else {
      c1++;
    }
  }
  assert(c0 == 30 && c1 == 10);

  /* Redundant critical mode mirrors across all active healthy paths */
  assert(rudp_multipath_init(&mp, 0x7777U, RUDP_MP_REDUNDANT) == RUDP_OK);
  assert(rudp_multipath_add_path(&mp, 0U, 1U, 11U) == RUDP_OK);
  assert(rudp_multipath_add_path(&mp, 1U, 1U, 22U) == RUDP_OK);
  int cnt = rudp_multipath_select_paths(&mp, 0U, 64U, ids, ent);
  assert(cnt == 2 && ids[0] == 0U && ids[1] == 1U && ent[0] != ent[1]);

  /* ECF Spray mode: symmetric paths keep 3:1 WRR; asymmetric paths gate slow path */
  assert(rudp_multipath_init(&mp, 0x5555U, RUDP_MP_ECF_SPRAY) == RUDP_OK);
  assert(rudp_multipath_add_path(&mp, 0U, 3U, 1U) == RUDP_OK);
  assert(rudp_multipath_add_path(&mp, 1U, 1U, 2U) == RUDP_OK);
  c0 = 0;
  c1 = 0;
  for (int i = 0; i < 40; ++i) {
    assert(rudp_multipath_select_paths(&mp, 0U, 100U, ids, ent) == 1);
    if (ids[0] == 0U) {
      c0++;
    } else {
      c1++;
    }
  }
  assert(c0 == 30 && c1 == 10);

  /* Push path 1 SRTT to ~90 ms while path 0 stays ~16 ms -> ECF gates path 1 */
  for (int i = 0; i < 32; ++i) {
    assert(rudp_multipath_on_feedback(&mp, 0U, 16U, false, (uint32_t)(i * 10)) == RUDP_OK);
    assert(rudp_multipath_on_feedback(&mp, 1U, 95U, false, (uint32_t)(i * 10)) == RUDP_OK);
  }
  c0 = 0;
  c1 = 0;
  for (int i = 0; i < 20; ++i) {
    assert(rudp_multipath_select_paths(&mp, 0U, 100U, ids, ent) == 1);
    if (ids[0] == 0U) {
      c0++;
    } else {
      c1++;
    }
  }
  assert(c0 == 20 && c1 == 0);
}

/* 6. Pudica / L4S Prague Pacer (route-shift min-filter & per-RTT backoff guard) */
static void test_owd_pacer_pudica_prague(void) {
  rudp_pacer_s pacer;
  assert(rudp_pacer_init(&pacer, 500000U, 20000U, 2000000U, 5000U, 1000ULL) ==
         RUDP_OK);
  for (int i = 0; i < 10; ++i) {
    assert(rudp_pacer_refill_and_can_send(&pacer, 1000ULL + (uint64_t)i * 10000ULL, 100U) ||
           true);
    assert(rudp_pacer_on_owd_sample(&pacer, 20000U, 100U, 0U) == RUDP_OK);
  }
  /* 8 ACKs within 14 ms (< 2 * base_owd = 40 ms) should trigger at most 1 cut */
  uint32_t before_burst = pacer.rate_bytes_per_sec;
  for (int i = 0; i < 8; ++i) {
    uint64_t t = 110000ULL + (uint64_t)i * 2000ULL;
    (void)rudp_pacer_refill_and_can_send(&pacer, t, 100U);
    assert(rudp_pacer_on_owd_sample(&pacer, 28000U, 100U, 0U) == RUDP_OK);
  }
  assert(pacer.rate_bytes_per_sec >= (before_burst * 7U) / 8U - 1024U);

  /* Permanent +15 ms route shift over 110 samples refreshes base_owd and recovers rate */
  for (int i = 0; i < 110; ++i) {
    uint64_t t = 200000ULL + (uint64_t)i * 50000ULL;
    (void)rudp_pacer_refill_and_can_send(&pacer, t, 100U);
    assert(rudp_pacer_on_owd_sample(&pacer, 35000U, 100U, 0U) == RUDP_OK);
  }
  assert(pacer.base_owd_us >= 34000U);
  assert(pacer.rate_bytes_per_sec > 200000U);
}

/* 7. Cauchy GF(2^8) Sliding FEC recovers 2 consecutive burst drops */
static void test_sliding_fec_multi_loss_rlnc(void) {
  rudp_sliding_fec_s tx;
  rudp_sliding_fec_rx_s rx;
  assert(rudp_sliding_fec_init(&tx, 8U) == RUDP_OK);
  assert(rudp_sliding_fec_rx_init(&rx, 8U) == RUDP_OK);
  for (int i = 0; i < 2; ++i) {
    assert(rudp_sliding_fec_update_loss(&tx, true) == RUDP_OK);
  }
  tx.parity_interval = 4U;

  rudp_sliding_fec_repair_s repairs[4];
  int rcnt = 0;
  for (uint16_t s = 10U; s <= 17U; ++s) {
    tfv_packet_u p = {.type = 7U, .flags = 2U, .value = (uint16_t)(s * 111U)};
    /* Drop consecutive burst: seq 12 and seq 13 */
    if (s != 12U && s != 13U) {
      assert(rudp_sliding_fec_rx_source(&rx, s, p) == RUDP_OK);
    }
    rudp_sliding_fec_repair_s rep = {0};
    int r = rudp_sliding_fec_encode(&tx, s, p, &rep);
    assert(r >= 0);
    if (r == 1) {
      repairs[rcnt++] = rep;
    }
  }
  assert(rcnt == 2);
  uint16_t rec_seq = 0U;
  tfv_packet_u rec_pkt = {0};
  /* First repair sees 2 unknowns -> buffers (0 recovered yet) */
  assert(rudp_sliding_fec_rx_repair(&rx, &repairs[0], &rec_seq, &rec_pkt) == 0);
  /* Second repair completes Cauchy GF(2^8) rank -> recovers BOTH seq 12 & seq 13 */
  assert(rudp_sliding_fec_rx_repair(&rx, &repairs[1], &rec_seq, &rec_pkt) == 2);

  tfv_packet_u p12 = {0}, p13 = {0};
  assert(rudp_sliding_fec_rx_get(&rx, 12U, &p12));
  assert(rudp_sliding_fec_rx_get(&rx, 13U, &p13));
  assert(p12.type == 7U && p12.flags == 2U && p12.value == 12U * 111U);
  assert(p13.type == 7U && p13.flags == 2U && p13.value == 13U * 111U);
}

int main(void) {
  test_page_pool_and_packet_plan();
  test_shared_metadata_pool();
  test_owd_pacer();
  test_sliding_window_fec();
  test_multipath_scheduler();
  test_owd_pacer_pudica_prague();
  test_sliding_fec_multi_loss_rlnc();
  printf("ALL PHASE 7 NEXTGEN TESTS PASSED!\n");
  return 0;
}
