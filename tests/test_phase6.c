#include "protocol_profiles.h"
#include "protocol_rudp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int rudp_fuzz_one_input(const uint8_t *data, size_t size);

static uint32_t next_prng(uint32_t *state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

/* 1. Unreliable intra-tick bundling, prepare/cancel rollback, wakeup & hole ACK advance */
static void test_queued_unreliable_and_holes(void) {
  rudp_session_s a, b;
  assert(rudp_session_init(&a) == RUDP_OK);
  assert(rudp_session_init(&b) == RUDP_OK);

  /* Error guards */
  tfv_packet_u p0 = {.type = 1U, .flags = 0U, .value = 10U};
  assert(rudp_session_queue_unreliable(NULL, 0U, p0) == RUDP_ERR_INVALID_ARG);
  assert(rudp_session_queue_unreliable(&a, RUDP_MAX_CHANNELS, p0) ==
         RUDP_ERR_INVALID_ARG);
  a.channels[0].ctx.state = RUDP_STATE_DISCONNECTED;
  assert(rudp_session_queue_unreliable(&a, 0U, p0) == RUDP_ERR_DISCONNECTED);
  a.channels[0].ctx.state = RUDP_STATE_CONNECTED;

  assert(!rudp_session_has_pending_work(&a, 0U, 100U));
  assert(rudp_session_has_pending_work(NULL, 0U, 100U) == false);

  /* Mixed sequence on same channel: R0, U0, R1, U1 */
  assert(rudp_session_send_reliable(&a, 0U,
                                    (tfv_packet_u){.type = 1U, .value = 100U}, 10U) ==
         RUDP_OK);
  assert(rudp_session_queue_unreliable(&a, 0U,
                                       (tfv_packet_u){.type = 2U, .value = 200U}) ==
         RUDP_OK);
  assert(rudp_session_send_reliable(&a, 0U,
                                    (tfv_packet_u){.type = 1U, .value = 101U}, 10U) ==
         RUDP_OK);
  assert(rudp_session_queue_unreliable(&a, 0U,
                                       (tfv_packet_u){.type = 2U, .value = 201U}) ==
         RUDP_OK);
  assert(rudp_session_has_pending_work(&a, 10U, 100U));

  /* Snapshot before prepare + cancel must restore unreliable slots and tail identically */
  rudp_session_s a_before = a;
  uint8_t dgram[1404U];
  rudp_tx_plan_s plan;
  int prep = rudp_session_prepare_datagram(&a, 10U, 100U, dgram, sizeof(dgram), &plan);
  assert(prep == RUDP_WIRE_DATAGRAM_HEADER_SIZE + 4 * RUDP_WIRE_RECORD_SIZE);
  assert(rudp_session_cancel_datagram(&a, &plan) == RUDP_OK);
  assert(memcmp(&a, &a_before, sizeof(a)) == 0);

  /* Now commit bundled datagram: unreliable slots become RUDP_SLOT_FREE holes at 1 & 3 */
  int built =
      rudp_session_prepare_datagram(&a, 10U, 100U, dgram, sizeof(dgram), &plan);
  assert(built == RUDP_WIRE_DATAGRAM_HEADER_SIZE + 4 * RUDP_WIRE_RECORD_SIZE);
  assert(rudp_session_commit_datagram(&a, &plan) == RUDP_OK);
  assert(a.channels[0].ctx.tx_buffer[0].state == RUDP_SLOT_IN_FLIGHT);
  assert(a.channels[0].ctx.tx_buffer[1].state == RUDP_SLOT_FREE);
  assert(a.channels[0].ctx.tx_buffer[2].state == RUDP_SLOT_IN_FLIGHT);
  assert(a.channels[0].ctx.tx_buffer[3].state == RUDP_SLOT_FREE);
  assert(a.channels[0].ctx.tail == 0U);
  assert(a.channels[0].ctx.head == 4U);
  assert(!rudp_session_has_pending_work(&a, 10U, 100U));
  assert(rudp_session_has_pending_work(&a, 111U, 100U));

  /* Receiver receives all 4 bundled records in a single datagram */
  rudp_record_s del[8];
  int dcount = rudp_session_process_datagram_at(&b, dgram, (size_t)built, del, 8, 12U);
  assert(dcount == 4);
  assert(del[0].payload.value == 100U);
  assert(del[1].payload.value == 200U);
  assert(del[2].payload.value == 101U);
  assert(del[3].payload.value == 201U);

  /* Peer emits ACK (seq 2); sender's tail must skip both reliable slots and both holes */
  uint8_t ack_buf[1404U];
  int ack_len = rudp_session_build_datagram(&b, 0U, ack_buf, sizeof(ack_buf), 12U, 100U);
  assert(ack_len == RUDP_WIRE_DATAGRAM_HEADER_SIZE);
  rudp_stats_s stats = {0};
  int rx_ack = rudp_session_process_datagram_ex(&a, ack_buf, (size_t)ack_len, del, 8,
                                                true, 15U, &stats);
  assert(rx_ack == 0);
  assert(stats.rx_ack_rejected == 0U);
  assert(a.channels[0].ctx.tail == a.channels[0].ctx.head);

  /* Verify partial ACK skips hole immediately when R0 is ACKed before R1 */
  assert(rudp_session_send_reliable(&a, 0U,
                                    (tfv_packet_u){.type = 1U, .value = 300U}, 20U) ==
         RUDP_OK);
  assert(rudp_session_queue_unreliable(&a, 0U,
                                       (tfv_packet_u){.type = 2U, .value = 301U}) ==
         RUDP_OK);
  assert(rudp_session_send_reliable(&a, 0U,
                                    (tfv_packet_u){.type = 1U, .value = 302U}, 20U) ==
         RUDP_OK);
  int blen = rudp_session_build_datagram(&a, 0U, dgram, sizeof(dgram), 20U, 100U);
  assert(blen == RUDP_WIRE_DATAGRAM_HEADER_SIZE + 3 * RUDP_WIRE_RECORD_SIZE);
  /* Ack only seq 3 (R0), leaving R1 (seq 4) unacked; tail must jump from 4 across 5(FREE) to 6 */
  assert(rudp_recv_ack(&a.channels[0].ctx, 3U) == RUDP_OK);
  assert(a.channels[0].ctx.tail == 6U);
  assert(a.channels[0].ctx.tx_buffer[6U].frame.header.seq_num == 3U);

  /* Clean SACK across RUDP_SLOT_UNRELIABLE holes inside [tail, head) */
  assert(rudp_session_init(&a) == RUDP_OK);
  assert(rudp_session_init(&b) == RUDP_OK);
  assert(rudp_session_config_recovery(&a, 0U, 100U, 10U, 1000U) == RUDP_OK);
  assert(rudp_session_config_recovery(&b, 0U, 100U, 10U, 1000U) == RUDP_OK);
  assert(rudp_session_send_reliable(&a, 0U, (tfv_packet_u){.value = 1U}, 30U) ==
         RUDP_OK);
  assert(rudp_session_queue_unreliable(&a, 0U, (tfv_packet_u){.value = 2U}) == RUDP_OK);
  assert(rudp_session_send_reliable(&a, 0U, (tfv_packet_u){.value = 3U}, 30U) ==
         RUDP_OK);
  assert(rudp_session_send_reliable(&a, 0U, (tfv_packet_u){.value = 4U}, 30U) ==
         RUDP_OK);
  assert(rudp_session_send_reliable(&a, 0U, (tfv_packet_u){.value = 5U}, 30U) ==
         RUDP_OK);
  /* Set MTU to emit 1 record at a time so each packet is separate; R0 is dropped */
  assert(rudp_session_set_mtu(&a, RUDP_WIRE_DATAGRAM_HEADER_SIZE + RUDP_WIRE_RECORD_SIZE) ==
         1);
  uint8_t pkts[5][RUDP_WIRE_DATAGRAM_HEADER_SIZE + RUDP_WIRE_RECORD_SIZE];
  for (int i = 0; i < 5; ++i) {
    assert(rudp_session_build_datagram(&a, 0U, pkts[i], sizeof(pkts[i]), 30U, 100U) ==
           (int)sizeof(pkts[i]));
  }
  assert(rudp_session_set_mtu(&a, 1404U) > 1);
  for (int i = 1; i < 5; ++i) {
    assert(rudp_session_process_datagram_at(&b, pkts[i], sizeof(pkts[i]), del, 8,
                                            35U) >= 0);
  }
  ack_len = rudp_session_build_datagram(&b, 0U, ack_buf, sizeof(ack_buf), 35U, 100U);
  assert(ack_len > RUDP_WIRE_DATAGRAM_HEADER_SIZE);
  stats = (rudp_stats_s){0};
  assert(rudp_session_process_datagram_ex(&a, ack_buf, (size_t)ack_len, del, 8, true,
                                          40U, &stats) == 0);
  assert(stats.rx_ack_rejected == 0U);
  assert(stats.rx_sack_blocks == 3U);
  assert(rudp_session_has_pending_work(&a, 40U, 100U));

  /* Fill TX ring to capacity with unreliable packets to verify BUFFER_FULL */
  rudp_session_s full;
  assert(rudp_session_init(&full) == RUDP_OK);
  for (size_t i = 0; i < RUDP_WINDOW_SIZE - 1U; ++i) {
    assert(rudp_session_queue_unreliable(&full, 1U, (tfv_packet_u){.value = (uint16_t)i}) ==
           RUDP_OK);
  }
  assert(rudp_session_queue_unreliable(&full, 1U, p0) == RUDP_ERR_BUFFER_FULL);
}

/* 2. Full branch coverage of src/io.c */
static void test_io_coverage(void) {
  rudp_iovec_collection_s col = {.count = 0};
  uint8_t dummy[RUDP_IOVEC_MAX] = {0};
  for (uint8_t i = 0U; i < RUDP_IOVEC_MAX; ++i) {
    dummy[i] = (uint8_t)(i + 1U);
  }
  uint8_t out[16] = {0};

  assert(rudp_iovec_reset(NULL) == RUDP_ERR_INVALID_ARG);
  assert(rudp_iovec_add(NULL, dummy, 4U) == RUDP_ERR_INVALID_ARG);
  assert(rudp_iovec_add(&col, NULL, 4U) == RUDP_ERR_INVALID_ARG);

  /* Zero-length slice is allowed and ignored by flatten */
  assert(rudp_iovec_add(&col, dummy, 0U) == RUDP_OK);
  assert(col.count == 1U && col.total_len == 0U);
  assert(rudp_iovec_flatten(&col, out, sizeof(out)) == 0);

  /* Fill up to RUDP_IOVEC_MAX then trigger BUFFER_FULL */
  for (uint8_t i = 1U; i < RUDP_IOVEC_MAX; ++i) {
    assert(rudp_iovec_add(&col, &dummy[i], 1U) == RUDP_OK);
  }
  assert(rudp_iovec_add(&col, dummy, 1U) == RUDP_ERR_BUFFER_FULL);
  assert(rudp_iovec_flatten(&col, out, (size_t)(RUDP_IOVEC_MAX - 2U)) ==
         RUDP_ERR_BUFFER_FULL);
  assert(rudp_iovec_flatten(&col, out, sizeof(out)) == RUDP_IOVEC_MAX - 1);

  /* Overflow > 65535 bytes on rudp_iovec_add */
  assert(rudp_iovec_reset(&col) == RUDP_OK);
  assert(col.count == 0U && col.total_len == 0U && col.path_entropy == 0U &&
         col.dscp == 0U);
  assert(rudp_iovec_add(&col, dummy, 65535U) == RUDP_OK);
  assert(rudp_iovec_add(&col, dummy, 1U) == RUDP_ERR_BUFFER_FULL);

  /* Corrupted collection validation inside rudp_iovec_flatten */
  assert(rudp_iovec_flatten(NULL, out, sizeof(out)) == RUDP_ERR_INVALID_ARG);
  assert(rudp_iovec_flatten(&col, NULL, sizeof(out)) == RUDP_ERR_INVALID_ARG);
  col.count = RUDP_IOVEC_MAX + 1U;
  assert(rudp_iovec_flatten(&col, out, sizeof(out)) == RUDP_ERR_INVALID_ARG);
  col.count = 1U;
  col.iovecs[0] = (rudp_iovec_s){.base = NULL, .len = 4U};
  assert(rudp_iovec_flatten(&col, out, sizeof(out)) == RUDP_ERR_INVALID_ARG);
  col.count = 2U;
  col.iovecs[0] = (rudp_iovec_s){.base = dummy, .len = 40000U};
  col.iovecs[1] = (rudp_iovec_s){.base = dummy, .len = 40000U};
  assert(rudp_iovec_flatten(&col, out, sizeof(out)) == RUDP_ERR_INVALID_ARG);

  /* Deterministic entropy hash checks */
  uint32_t e0 = rudp_calc_entropy(0x1234U, 0U, 0U);
  uint32_t e1 = rudp_calc_entropy(0x1234U, 1U, 0U);
  uint32_t e2 = rudp_calc_entropy(0x1234U, 0U, 1U);
  assert(e0 == rudp_calc_entropy(0x1234U, 0U, 0U));
  assert(e0 != e1 && e0 != e2);
}

/* 3. Retry-limit disconnect across build/prepare datagram & iovec APIs */
static void test_retry_disconnect_all_apis(void) {
  for (int api = 0; api < 4; ++api) {
    rudp_session_s s;
    assert(rudp_session_init(&s) == RUDP_OK);
    assert(rudp_session_send_reliable(&s, 0U, (tfv_packet_u){.value = 9U}, 10U) ==
           RUDP_OK);

    uint8_t buf[1404U];
    rudp_iovec_collection_s col;
    rudp_tx_plan_s plan;
    uint32_t now = 10U;

    /* Initial send + RUDP_MAX_RETRIES retransmissions succeed */
    for (int attempt = 0; attempt <= RUDP_MAX_RETRIES; ++attempt) {
      int ret = 0;
      if (api == 0) {
        ret = rudp_session_build_datagram(&s, 0U, buf, sizeof(buf), now, 10U);
      } else if (api == 1) {
        ret = rudp_session_build_iovec(&s, 0U, 0U, buf, sizeof(buf), now, 10U, &col);
      } else if (api == 2) {
        ret = rudp_session_prepare_datagram(&s, now, 10U, buf, sizeof(buf), &plan);
        if (ret > 0) {
          assert(rudp_session_commit_datagram(&s, &plan) == RUDP_OK);
        }
      } else {
        ret = rudp_session_prepare_iovec(&s, 0U, now, 10U, buf, sizeof(buf), &col,
                                         &plan);
        if (ret > 0) {
          assert(rudp_session_commit_datagram(&s, &plan) == RUDP_OK);
        }
      }
      assert(ret == RUDP_WIRE_DATAGRAM_HEADER_SIZE + RUDP_WIRE_RECORD_SIZE);
      /* Advance past max exponential backoff (10 ms << 6 = 640 ms) each retry */
      now += 1000U;
    }

    /* Next timeout crosses RUDP_MAX_RETRIES and disconnects channel 0 */
    int disc = 0;
    if (api == 0) {
      disc = rudp_session_build_datagram(&s, 0U, buf, sizeof(buf), now, 10U);
    } else if (api == 1) {
      disc = rudp_session_build_iovec(&s, 0U, 0U, buf, sizeof(buf), now, 10U, &col);
    } else if (api == 2) {
      disc = rudp_session_prepare_datagram(&s, now, 10U, buf, sizeof(buf), &plan);
      assert(plan.valid == 0U);
    } else {
      disc = rudp_session_prepare_iovec(&s, 0U, now, 10U, buf, sizeof(buf), &col,
                                        &plan);
      assert(plan.valid == 0U);
    }
    assert(disc == RUDP_ERR_DISCONNECTED);
    assert(s.channels[0].ctx.state == RUDP_STATE_DISCONNECTED);
  }

  /* Fixed-mode timeout == 0 at identical timestamp never retransmits or disconnects. */
  {
    rudp_session_s s;
    assert(rudp_session_init(&s) == RUDP_OK);
    assert(rudp_session_send_reliable(&s, 0U, (tfv_packet_u){.value = 42U}, 10U) ==
           RUDP_OK);
    uint8_t buf[1404U];
    assert(rudp_session_build_datagram(&s, 0U, buf, sizeof(buf), 10U, 0U) ==
           RUDP_WIRE_DATAGRAM_HEADER_SIZE + RUDP_WIRE_RECORD_SIZE);
    for (int i = 0; i < 12; ++i) {
      assert(!rudp_session_has_pending_work(&s, 10U, 0U));
      int n = rudp_session_build_datagram(&s, 0U, buf, sizeof(buf), 10U, 0U);
      assert(n == RUDP_WIRE_DATAGRAM_HEADER_SIZE);
      assert(s.channels[0].ctx.state == RUDP_STATE_CONNECTED);
    }
  }

  /* Bidirectional data + piggybacked gap-ACK preserves Tri-ACK fast retransmit. */
  {
    rudp_session_s a, b;
    assert(rudp_session_init(&a) == RUDP_OK);
    assert(rudp_session_init(&b) == RUDP_OK);
    uint8_t wire[1404U];
    rudp_record_s rx_out[16U];
    uint8_t dgrams[4U][1404U];

    for (uint16_t i = 0U; i < 4U; ++i) {
      assert(rudp_session_send_reliable(
                 &a, 0U, (tfv_packet_u){.value = (uint16_t)(10U + i)}, 10U) ==
             RUDP_OK);
      int n = rudp_session_build_datagram(&a, 0U, dgrams[i], sizeof(dgrams[i]),
                                          10U, 200U);
      assert(n == RUDP_WIRE_DATAGRAM_HEADER_SIZE + RUDP_WIRE_RECORD_SIZE);
    }

    /* Drop dgrams[0]; deliver dgrams[1..3] while B also sends reverse unreliable data. */
    for (uint16_t i = 1U; i < 4U; ++i) {
      assert(rudp_session_process_datagram(
                 &b, dgrams[i],
                 RUDP_WIRE_DATAGRAM_HEADER_SIZE + RUDP_WIRE_RECORD_SIZE, rx_out,
                 16U) == 0);
      assert(rudp_session_queue_unreliable(
                 &b, 0U, (tfv_packet_u){.type = 2U, .value = i}) == RUDP_OK);
      int blen =
          rudp_session_build_datagram(&b, 0U, wire, sizeof(wire), 20U + i, 200U);
      assert(blen ==
             RUDP_WIRE_DATAGRAM_HEADER_SIZE + (2 * RUDP_WIRE_RECORD_SIZE));
      int dlen =
          rudp_session_process_datagram(&a, wire, (size_t)blen, rx_out, 16U);
      assert(dlen == 1);
      assert(rx_out[0].flags == RUDP_RECORD_FLAG_UNRELIABLE);
    }

    /* Before 200 ms RTO expires (at t=30), A fast-retransmits lost seq 0 immediately. */
    assert(rudp_session_has_pending_work(&a, 30U, 200U));
    int flen = rudp_session_build_datagram(&a, 0U, wire, sizeof(wire), 30U, 200U);
    assert(flen == RUDP_WIRE_DATAGRAM_HEADER_SIZE + RUDP_WIRE_RECORD_SIZE);
    int delivered =
        rudp_session_process_datagram(&b, wire, (size_t)flen, rx_out, 16U);
    assert(delivered == 4);
    assert(rx_out[0].payload.value == 10U && rx_out[3].payload.value == 13U);
  }
}

/* 4. Stream payload scalability (4B .. 1200B) under loss */
static void test_payload_scalability_and_wan_bdp(void) {
  static const size_t sizes[5] = {4U, 64U, 256U, 512U, 1200U};
  uint32_t rng = 0xCAFE1234U;

  for (size_t sidx = 0; sidx < 5U; ++sidx) {
    size_t len = sizes[sidx];
    uint8_t src[1200];
    uint8_t dst[1200];
    for (size_t i = 0; i < len; ++i) {
      src[i] = (uint8_t)((i * 37U + sidx * 17U) & 0xFFU);
    }
    memset(dst, 0, sizeof(dst));

    rudp_session_s a, b;
    assert(rudp_session_init(&a) == RUDP_OK);
    assert(rudp_session_init(&b) == RUDP_OK);
    assert(rudp_session_config_recovery(&a, 0U, 60U, 10U, 1000U) == RUDP_OK);
    assert(rudp_session_config_recovery(&b, 0U, 60U, 10U, 1000U) == RUDP_OK);

    rudp_stream_tx_s stx = {.data = NULL};
    rudp_stream_rx_s srx = {.data = dst, .capacity = sizeof(dst)};
    assert(rudp_stream_start(&stx, src, len) == RUDP_OK);

    int done = 0;
    uint32_t now = 0U;
    for (int tick = 0; tick < 2000 && !done; ++tick) {
      now = (uint32_t)(tick * 5);
      (void)rudp_stream_pump(&stx, &a, 0U, now);

      uint8_t wire[1404U];
      int alen = rudp_session_build_datagram(&a, 0U, wire, sizeof(wire), now, 60U);
      assert(alen >= RUDP_WIRE_DATAGRAM_HEADER_SIZE);
      if (alen > RUDP_WIRE_DATAGRAM_HEADER_SIZE && (next_prng(&rng) % 20U) == 0U) {
        /* 5% deterministic drop on A->B data */
      } else {
        rudp_record_s del[175];
        int n = rudp_session_process_datagram_at(&b, wire, (size_t)alen, del, 175, now);
        assert(n >= 0);
        do {
          for (int i = 0; i < n; ++i) {
            if (del[i].flags == RUDP_RECORD_FLAG_RELIABLE) {
              int r = rudp_stream_receive(&srx, del[i].payload);
              assert(r >= 0);
              if (r == 1) {
                assert(srx.length == len);
                assert(memcmp(dst, src, len) == 0);
                done = 1;
              }
            }
          }
          n = rudp_session_poll(&b, del, 175);
          assert(n >= 0);
        } while (n > 0);
      }

      int blen = rudp_session_build_datagram(&b, 0U, wire, sizeof(wire), now, 60U);
      assert(blen >= RUDP_WIRE_DATAGRAM_HEADER_SIZE);
      rudp_record_s ad[16];
      assert(rudp_session_process_datagram_at(&a, wire, (size_t)blen, ad, 16, now) >=
             0);
    }
    assert(done == 1);
  }

  /* WAN BDP test: 100 ms RTT (50 ms one-way), 120 Hz pacing, 240 msgs, 3% loss */
  rudp_session_s a, b;
  assert(rudp_session_init(&a) == RUDP_OK);
  assert(rudp_session_init(&b) == RUDP_OK);
  assert(rudp_session_config_recovery(&a, 0U, 100U, 20U, 2000U) == RUDP_OK);
  assert(rudp_session_config_recovery(&b, 0U, 100U, 20U, 2000U) == RUDP_OK);

  enum { WAN_CAP = 512, WAN_TOTAL = 240 };
  struct wan_pkt {
    uint32_t due;
    uint16_t len;
    uint8_t dir;
    uint8_t active;
    uint8_t bytes[1404U];
  };
  static struct wan_pkt q[WAN_CAP];
  memset(q, 0, sizeof(q));

  uint16_t sent = 0U, recvd = 0U;
  for (uint32_t now = 0U; now < 15000U && recvd < WAN_TOTAL; now += 4U) {
    while (sent < WAN_TOTAL && ((uint32_t)sent * 1000U) / 120U <= now) {
      uint16_t pending =
          (uint16_t)((a.channels[0].ctx.head - a.channels[0].ctx.tail) &
                     (RUDP_WINDOW_SIZE - 1U));
      if (pending >= RUDP_WINDOW_SIZE - 1U) {
        break;
      }
      assert(rudp_session_send_reliable(&a, 0U,
                                        (tfv_packet_u){.type = 1U, .value = sent},
                                        now) == RUDP_OK);
      sent++;
    }

    for (size_t i = 0; i < WAN_CAP; ++i) {
      if (q[i].active && q[i].due <= now) {
        rudp_session_s *dst_s = q[i].dir == 0U ? &b : &a;
        rudp_record_s del[175];
        int n = rudp_session_process_datagram_at(dst_s, q[i].bytes, q[i].len, del, 175,
                                                 now);
        assert(n >= 0);
        if (q[i].dir == 0U) {
          do {
            for (int r = 0; r < n; ++r) {
              assert(del[r].payload.value == recvd);
              recvd++;
            }
            n = rudp_session_poll(&b, del, 175);
            assert(n >= 0);
          } while (n > 0);
        }
        q[i].active = 0U;
      }
    }

    for (uint8_t dir = 0U; dir < 2U; ++dir) {
      rudp_session_s *src_s = dir == 0U ? &a : &b;
      uint8_t had_ack = src_s->channels[0].ack_pending;
      uint8_t wire[1404U];
      int n = rudp_session_build_datagram(src_s, 0U, wire, sizeof(wire), now, 100U);
      assert(n >= RUDP_WIRE_DATAGRAM_HEADER_SIZE);
      if (n > RUDP_WIRE_DATAGRAM_HEADER_SIZE || had_ack) {
        if ((next_prng(&rng) % 100U) >= 3U) {
          for (size_t i = 0; i < WAN_CAP; ++i) {
            if (!q[i].active) {
              q[i].active = 1U;
              q[i].dir = dir;
              q[i].due = now + 50U;
              q[i].len = (uint16_t)n;
              memcpy(q[i].bytes, wire, (size_t)n);
              break;
            }
          }
        }
      }
    }
  }
  assert(sent == WAN_TOTAL && recvd == WAN_TOTAL);
}

/* 5. Deterministic 5,000-iteration corpus fuzz check */
static void test_fuzz_corpus(void) {
  uint32_t rng = 0xDEADBEEFU;
  uint8_t buf[128];
  assert(rudp_fuzz_one_input(NULL, 0U) == 0);
  for (int iter = 0; iter < 5000; ++iter) {
    size_t len = (size_t)(next_prng(&rng) % sizeof(buf));
    for (size_t j = 0; j < len; ++j) {
      buf[j] = (uint8_t)(next_prng(&rng) & 0xFFU);
    }
    if (len >= 4U && (iter % 3) == 0) {
      buf[2] = (uint8_t)(buf[2] & 3U); /* Force valid channel */
      buf[3] = (uint8_t)((len - 4U) / 8U); /* Match record count */
    }
    assert(rudp_fuzz_one_input(buf, len) == 0);
  }
}

int main(void) {
  test_queued_unreliable_and_holes();
  test_io_coverage();
  test_retry_disconnect_all_apis();
  test_payload_scalability_and_wan_bdp();
  test_fuzz_corpus();
  printf("ALL PHASE 6 TESTS AND FUZZ CHECKS PASSED!\n");
  return 0;
}
