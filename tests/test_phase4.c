#include "protocol_profiles.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int inject(rudp_session_s *rx, uint8_t channel, uint16_t seq,
                  uint16_t value, rudp_record_s *out, size_t capacity) {
  uint8_t bytes[12];
  rudp_datagram_header_s header = {.ack = 0, .ack_channel = 0, .count = 1};
  rudp_record_s record = {
      .channel_id = channel, .flags = RUDP_RECORD_FLAG_RELIABLE,
      .seq_num = seq, .payload = {.value = value},
  };
  assert(rudp_pack_datagram_header(&header, bytes, sizeof bytes) == 4);
  assert(rudp_pack_record(&record, bytes + 4, 8) == 8);
  return rudp_session_process_datagram(rx, bytes, sizeof bytes, out, capacity);
}

static void reassembly(void) {
  rudp_session_s rx;
  rudp_record_s out[RUDP_WINDOW_SIZE];
  assert(rudp_session_init(&rx) == 0);
  rx.channels[0].ctx.expected_seq_num = 65534;
  assert(inject(&rx, 0, 0, 12, out, 1) == 0);
  assert(inject(&rx, 0, 0, 999, out, 1) == 0); /* First copy wins. */
  assert(inject(&rx, 0, 65535, 11, out, 1) == 0);
  assert(inject(&rx, 1, 0, 20, out, 1) == 1); /* No cross-channel blocking. */
  assert(out[0].payload.value == 20);
  assert(inject(&rx, 0, 65534, 10, out, 1) == 1);
  assert(out[0].payload.value == 10 && rx.channels[0].ctx.expected_seq_num == 65535);
  assert(rudp_session_poll(&rx, out, 1) == 1 && out[0].payload.value == 11);
  assert(rudp_session_poll(&rx, out, 1) == 1 && out[0].payload.value == 12);
  assert(rudp_session_poll(&rx, out, 1) == 0);
  assert(inject(&rx, 0, 65535, 11, out, 1) == 0);
  assert(inject(&rx, 0, (uint16_t)(1 + RUDP_WINDOW_SIZE), 99, out, 1) == 0);
  assert(inject(&rx, 0, 2, 99, out, 1) == 0);
  assert(rudp_session_reset_channel(&rx, 0) == 0);
  assert(inject(&rx, 0, 0, 0, out, 1) == 1);
  assert(inject(&rx, 0, 1, 1, out, 1) == 1);
  assert(rudp_session_poll(&rx, out, 1) == 0); /* Reset erased buffered seq2. */
  assert(rudp_session_reset(&rx) == 0);
  for (unsigned i = RUDP_WINDOW_SIZE - 1; i; --i)
    assert(inject(&rx, 0, (uint16_t)i, (uint16_t)i, out, 1) == 0);
  assert(inject(&rx, 0, 0, 0, out, RUDP_WINDOW_SIZE) == RUDP_WINDOW_SIZE);
  for (unsigned i = 0; i < RUDP_WINDOW_SIZE; ++i) assert(out[i].payload.value == i);
  assert(rudp_session_poll(NULL, out, 1) < 0);
  assert(rudp_session_poll(&rx, NULL, 1) < 0);
}

static void priorities(void) {
  rudp_session_s tx;
  rudp_record_s record;
  uint8_t bytes[12];
  tfv_packet_u payload = {.raw = 0};
  assert(rudp_session_init(&tx) == 0);
  assert(rudp_session_set_qos(&tx, 0, 200, 0) == 0);
  assert(rudp_session_set_qos(&tx, 1, 0, 46) == 0);
  assert(rudp_session_set_qos(&tx, 1, 0, 64) < 0);
  assert(tx.channels[1].dscp == 46);
  assert(rudp_session_send_reliable(&tx, 0, payload, 0) == 0);
  assert(rudp_session_send_reliable(&tx, 1, payload, 0) == 0);
  assert(rudp_session_build_datagram(&tx, 0, bytes, 12, 0, 100) == 12);
  assert(rudp_unpack_record(bytes + 4, 8, &record) == 0 && record.channel_id == 1);
  assert(rudp_session_build_datagram(&tx, 0, bytes, 12, 0, 100) == 12);
  assert(rudp_unpack_record(bytes + 4, 8, &record) == 0 && record.channel_id == 0);
}

static void stream(size_t length, size_t capacity) {
  rudp_session_s sender, receiver;
  rudp_stream_tx_s tx = {.data = NULL};
  static uint8_t source[65535], destination[65537];
  uint8_t bytes[1400], ack[4];
  rudp_record_s out[175];
  memset(destination, 165, sizeof destination);
  for (size_t i = 0; i < sizeof source; ++i) source[i] = (uint8_t)i;
  rudp_stream_rx_s rx = {.data = destination + 1, .capacity = capacity};
  assert(rudp_session_init(&sender) == 0 && rudp_session_init(&receiver) == 0);
  assert(rudp_stream_start(&tx, source, length) == 0);
  assert(rudp_stream_start(&tx, source, length) == RUDP_ERR_BUFFER_FULL);
  unsigned complete = 0, rejected = 0;
  for (unsigned tick = 0; tick < 10000; ++tick) {
    int queued = rudp_stream_pump(&tx, &sender, 0, tick * 10);
    assert(queued >= 0);
    int len = rudp_session_build_datagram(&sender, 0, bytes, sizeof bytes, tick * 10, 100);
    assert(len >= 4);
    if (tick % 23 == 0) continue; /* Lose data, including first descriptor. */
    for (size_t a = 4, b = (size_t)len - 8; len > 12 && a < b; a += 8, b -= 8) {
      uint8_t temporary[8];
      memcpy(temporary, bytes + a, 8);
      memcpy(bytes + a, bytes + b, 8);
      memcpy(bytes + b, temporary, 8);
    } /* Feed chunks in reverse order: stream sees only ordered delivery. */
    int count = rudp_session_process_datagram(&receiver, bytes, (size_t)len, out, 175);
    assert(count >= 0);
    for (int i = 0; i < count; ++i) {
      int result = rudp_stream_receive(&rx, out[i].payload);
      if (result == 1) complete++;
      else if (result == RUDP_ERR_BUFFER_FULL) rejected++;
      else assert(result == 0);
    }
    assert(rudp_session_build_datagram(&receiver, 0, ack, 4, tick * 10, 100) == 4);
    if (tick % 7) assert(rudp_session_process_datagram(&sender, ack, 4, NULL, 0) == 0);
    if (queued && sender.channels[0].ctx.head == sender.channels[0].ctx.tail) break;
    assert(tick < 9999);
  }
  assert(!tx.active && !rx.active);
  assert(destination[0] == 165 && destination[65536] == 165);
  if (length <= capacity) {
    assert(complete == 1 && rejected == 0 && rx.length == length);
    assert(memcmp(source, destination + 1, length) == 0);
  } else {
    assert(!complete && rejected == 2);
    for (size_t i = 0; i < sizeof destination; ++i) assert(destination[i] == 165);
  }
  tfv_packet_u descriptor = {.raw = 0};
  assert(rudp_stream_receive(&rx, descriptor) < 0);
  descriptor = (tfv_packet_u){.type = 255, .flags = 211, .value = 0};
  assert(rudp_stream_receive(&rx, descriptor) == 1); /* Resync after discard. */
  assert(rudp_stream_start(&tx, NULL, 1) < 0);
  assert(rudp_stream_start(&tx, source, 65536) < 0);
  assert(rudp_stream_start(&tx, NULL, 0) == 0);
  assert(rudp_session_config_channel(&sender, 0, RUDP_CHANNEL_FLAG_UNRELIABLE) == 0);
  assert(rudp_stream_pump(&tx, &sender, 0, 0) < 0);
}

static void scalars(void) {
  rudp_scalar_tx_s tx = {.next_seq = 0};
  rudp_scalar_rx_s rx = {.last_seq = 0};
  rudp_scalar_sample_s out[2];
  uint8_t bytes[8], lost[8];
  assert(rudp_scalar_encode(&tx, 65535, true, bytes, 7) == RUDP_ERR_BUFFER_FULL);
  assert(!tx.next_seq);
  tx.next_seq = 65535;
  assert(rudp_scalar_encode(&tx, 65535, true, bytes, 8) == 8);
  assert(bytes[0] == 255 && bytes[1] == 255 && bytes[6] == 0 && bytes[7] == 0);
  assert(rudp_scalar_decode(&rx, bytes, 8, true, out, 2) == 1);
  assert(rudp_scalar_encode(&tx, 0, true, lost, 8) == 8);
  assert(rudp_scalar_encode(&tx, 1, true, bytes, 8) == 8);
  assert(rudp_scalar_decode(&rx, bytes, 8, true, out, 1) == RUDP_ERR_BUFFER_FULL);
  assert(rx.last_seq == 65535);
  assert(rudp_scalar_decode(&rx, bytes, 8, true, out, 2) == 2);
  assert(out[0].seq == 0 && out[0].value == 0 && out[1].value == 1);
  assert(rudp_scalar_decode(&rx, lost, 8, true, out, 2) == 0);
  assert(rudp_scalar_decode(&rx, bytes, 8, true, out, 2) == 0);
  bytes[7] = 2;
  assert(rudp_scalar_decode(&rx, bytes, 8, true, out, 2) < 0);
  assert(rudp_scalar_decode(&rx, bytes, 8, false, out, 2) < 0);
  assert(rudp_scalar_encode(&tx, 500, false, bytes, 4) == 4);
  assert(rudp_scalar_decode(&rx, bytes, 4, false, out, 2) == 1 && out[0].value == 500);
  tx.next_seq = (uint16_t)(rx.last_seq + 32768U);
  assert(rudp_scalar_encode(&tx, 501, false, bytes, 4) == 4);
  assert(rudp_scalar_decode(&rx, bytes, 4, false, out, 2) == 0);
  rudp_motion_tx_s motion = {.scalar = {.next_seq = 0}};
  uint8_t pair[16];
  assert(rudp_motion_encode(&motion, 0, 5, 5, pair, 15) < 0 && !motion.samples);
  assert(rudp_motion_encode(&motion, 0, 5, 5, pair, 16) == 1);
  assert(rudp_motion_encode(&motion, 1, 5, 5, pair, 16) == 1);
  assert(rudp_motion_encode(&motion, 2, 5, 5, pair, 16) == 1);
  assert(rudp_motion_encode(&motion, 100, 5, 5, pair, 16) == 2);
  assert(memcmp(pair, pair + 8, 8) == 0);
  memset(&rx, 0, sizeof rx);
  assert(rudp_scalar_decode(&rx, pair, 8, true, out, 2) == 2);
  assert(out[0].value == 2 && out[1].value == 100);
  assert(rudp_scalar_decode(&rx, pair + 8, 8, true, out, 2) == 0);
  assert(rudp_motion_encode(&motion, 200, 0, 0, pair, 16) == 1);
}

static int timed_ack(rudp_session_s *session, uint16_t seq, uint32_t now) {
  rudp_datagram_header_s header = {.ack = seq, .ack_channel = 0, .count = 0};
  uint8_t bytes[4];
  assert(rudp_pack_datagram_header(&header, bytes, 4) == 4);
  return rudp_session_process_datagram_at(session, bytes, 4, NULL, 0, now);
}

static void recovery(void) {
  rudp_session_s session;
  uint8_t bytes[1400];
  tfv_packet_u payload = {.raw = 0};
  assert(rudp_session_init(&session) == 0);
  rudp_channel_s *chan = &session.channels[0];
  assert(rudp_session_config_recovery(&session, 0, 100, 0, 2000) < 0);
  assert(rudp_session_config_recovery(&session, 0, 100, 200, 2000) < 0);
  assert(rudp_session_config_recovery(&session, 0, 100, 10, 60001) < 0);
  assert(rudp_session_config_recovery(&session, 0, 100, 10, 2000) == 0);
  assert(rudp_session_send_reliable(&session, 0, payload, 0) == 0);
  assert(rudp_session_config_recovery(&session, 0, 100, 10, 2000) == RUDP_ERR_BUFFER_FULL);
  /* Initial timer starts at actual encoding, not application queue time. */
  assert(rudp_session_build_datagram(&session, 0, bytes, sizeof bytes, 1000, 100) == 12);
  assert(timed_ack(&session, 1, 1020) == 0);
  assert(chan->srtt_scaled == 160 && chan->rttvar_scaled == 40 && chan->rto_ms == 60);
  assert(timed_ack(&session, 1, 1050) == 0 && chan->srtt_scaled == 160);
  assert(rudp_session_send_reliable(&session, 0, payload, 1100) == 0);
  assert(rudp_session_build_datagram(&session, 0, bytes, sizeof bytes, 1100, 100) == 12);
  /* Malformed datagram must not apply its ACK or sample. */
  const uint8_t malformed[5] = {0, 2, 0, 0, 0};
  assert(rudp_session_process_datagram_at(&session, malformed, 5, NULL, 0, 1120) < 0);
  assert(chan->ctx.tail == 1 && chan->rto_ms == 60);
  assert(timed_ack(&session, 3, 1120) == 0 && chan->ctx.tail == 1); /* Future ACK ignored. */
  assert(timed_ack(&session, 2, 1120) == 0);
  assert(chan->srtt_scaled == 160 && chan->rttvar_scaled == 30 && chan->rto_ms == 50);

  assert(rudp_session_send_reliable(&session, 0, payload, 1200) == 0);
  assert(rudp_session_build_datagram(&session, 0, bytes, sizeof bytes, 1200, 100) == 12);
  for (unsigned i = 0; i < 3; ++i) assert(timed_ack(&session, 2, 1210 + i) == 0);
  assert(rudp_session_build_datagram(&session, 0, bytes, sizeof bytes, 1212, 100) == 12);
  assert(chan->ctx.tx_buffer[2].retries == 1 && chan->timeout_backoffs[2] == 0);
  assert(rudp_session_build_datagram(&session, 0, bytes, sizeof bytes, 1262, 100) == 4);
  assert(rudp_session_build_datagram(&session, 0, bytes, sizeof bytes, 1263, 100) == 12);
  assert(chan->timeout_backoffs[2] == 1);
  assert(rudp_session_build_datagram(&session, 0, bytes, sizeof bytes, 1363, 100) == 4);
  assert(rudp_session_build_datagram(&session, 0, bytes, sizeof bytes, 1364, 100) == 12);
  assert(chan->timeout_backoffs[2] == 2);
  assert(timed_ack(&session, 3, 1380) == 0 && chan->rto_ms == 50); /* Karn. */
  assert(rudp_session_reset_channel(&session, 0) == 0);
  assert(!chan->srtt_scaled && chan->rto_ms == 100 && !chan->timeout_backoffs[2]);

  /* Unsigned clock wrap, one sample per RTT, and minimum clamp. */
  assert(rudp_session_config_recovery(&session, 0, 100, 10, 2000) == 0);
  assert(rudp_session_send_reliable(&session, 0, payload, UINT32_MAX - 9) == 0);
  assert(rudp_session_build_datagram(&session, 0, bytes, sizeof bytes, UINT32_MAX - 9, 0) == 12);
  assert(timed_ack(&session, 1, 10) == 0 && chan->srtt_scaled == 160);
  assert(rudp_session_reset_channel(&session, 0) == 0);
  assert(rudp_session_send_reliable(&session, 0, payload, 0) == 0);
  assert(rudp_session_build_datagram(&session, 0, bytes, sizeof bytes, 0, 0) == 12);
  assert(timed_ack(&session, 1, 0) == 0 && chan->rto_ms == 10);

  /* Fixed mode retains caller timing and cannot overflow its shifted timer. */
  assert(rudp_session_init(&session) == 0);
  assert(rudp_session_send_reliable(&session, 0, payload, 0) == 0);
  assert(rudp_session_build_datagram(&session, 0, bytes, sizeof bytes, 0, 100) == 12);
  assert(timed_ack(&session, 1, 20) == 0 && !chan->srtt_scaled);
  assert(rudp_session_send_reliable(&session, 0, payload, 0) == 0);
  assert(rudp_session_build_datagram(&session, 0, bytes, sizeof bytes, 0, 100) == 12);
  chan->ctx.tx_buffer[1].retries = 2;
  assert(rudp_session_build_datagram(&session, 0, bytes, sizeof bytes, 1000, UINT32_MAX) == 4);
  assert(rudp_session_init(&session) == 0);
  assert(rudp_session_config_recovery(&session, 0, 100, 10, 100) == 0);
  assert(rudp_session_send_reliable(&session, 0, payload, 0) == 0);
  assert(rudp_session_build_datagram(&session, 0, bytes, sizeof bytes, 0, 100) == 12);
  assert(timed_ack(&session, 1, 80) == 0 && chan->rto_ms == 100); /* Maximum clamp. */
  assert(rudp_session_send_reliable(&session, 0, payload, 80) == 0);
  assert(rudp_session_build_datagram(&session, 0, bytes, sizeof bytes, 80, 100) == 12);
  assert(timed_ack(&session, 2, 100) == 0 && chan->srtt_scaled == 640); /* Sampling cadence. */
  puts("PASS: adaptive RTT, Karn, clock wrap, ACK validation, split backoff, fixed-mode compatibility");
}

static void iovec_bounds(void) {
  static const uint8_t one[1] = {1};
  uint8_t out[8];
  rudp_iovec_collection_s c = {.count = 0};
  assert(rudp_iovec_add(&c, one, 1) == RUDP_OK);
  assert(rudp_iovec_add(&c, one, 0xFFFFFFFFu) == RUDP_ERR_BUFFER_FULL); /* uint32 wrap */
  assert(rudp_iovec_add(&c, one, 65535u) == RUDP_ERR_BUFFER_FULL);
  assert(c.count == 1 && c.total_len == 1);
  assert(rudp_iovec_flatten(&c, out, sizeof out) == 1);
  c.iovecs[0].len = 64; /* public fields edited behind the API */
  assert(rudp_iovec_flatten(&c, out, sizeof out) == RUDP_ERR_BUFFER_FULL);
  c.iovecs[0].len = 1; c.count = RUDP_IOVEC_MAX + 1;
  assert(rudp_iovec_flatten(&c, out, sizeof out) == RUDP_ERR_INVALID_ARG);
  puts("PASS: iovec overflow and malformed-collection rejection");
}

static void fast_retransmit_unsent(void) {
  rudp_session_s s;
  uint8_t ack[4], bytes[64];
  rudp_datagram_header_s h = {.ack = 0, .ack_channel = 0, .count = 0};
  assert(rudp_session_init(&s) == RUDP_OK);
  assert(rudp_session_send_reliable(&s, 0, (tfv_packet_u){.type = 1}, 0) == RUDP_OK);
  assert(rudp_pack_datagram_header(&h, ack, sizeof ack) == 4);
  for (int i = 0; i < 4; ++i) /* reference ACK + three duplicates while still unsent */
    assert(rudp_session_process_datagram(&s, ack, sizeof ack, NULL, 0) == 0);
  assert(rudp_session_build_datagram(&s, 0, bytes, sizeof bytes, 10, 1000) == 12);
  /* The initial transmission satisfies the request: no immediate duplicate. */
  assert(rudp_session_build_datagram(&s, 0, bytes, sizeof bytes, 10, 1000) == 4);
  assert(s.channels[0].ctx.tx_buffer[0].retries == 0);
  puts("PASS: fast-retransmit request on an unsent slot is cleared by its first send");
}

static void channel_backoff_inheritance_and_cap(void) {
  rudp_session_s s;
  uint8_t out[64];
  assert(rudp_session_init(&s) == RUDP_OK);
  assert(rudp_session_config_recovery(&s, 0, 100, 10, 2000) == RUDP_OK);
  assert(rudp_session_send_reliable(&s, 0, (tfv_packet_u){.type = 1, .value = 1}, 0) == RUDP_OK);
  assert(rudp_session_build_datagram(&s, 0, out, sizeof(out), 0, 100) == 12);
  /* Four consecutive RTOs push chan_backoff to 4 (16x = 1600ms). */
  assert(rudp_session_build_datagram(&s, 0, out, sizeof(out), 101, 100) == 12);
  assert(rudp_session_build_datagram(&s, 0, out, sizeof(out), 302, 100) == 12);
  assert(rudp_session_build_datagram(&s, 0, out, sizeof(out), 703, 100) == 12);
  assert(rudp_session_build_datagram(&s, 0, out, sizeof(out), 1504, 100) == 12);
  assert(s.channels[0].chan_backoff == 4U);

  /* Newly queued packet 1 inherits chan_backoff to break Karn's deadlock. */
  assert(rudp_session_send_reliable(&s, 0, (tfv_packet_u){.type = 1, .value = 2}, 1510) == RUDP_OK);
  assert(s.channels[0].timeout_backoffs[1] == 4U);
  assert(rudp_session_build_datagram(&s, 0, out, sizeof(out), 1510, 100) == 12);
  /* ACK=1 frees only retransmitted pkt 0 (Karn skips sample; chan_backoff remains 4). */
  uint8_t ack[4];
  rudp_datagram_header_s h = {.ack = 1, .ack_channel = 0, .count = 0};
  assert(rudp_pack_datagram_header(&h, ack, sizeof(ack)) == 4);
  assert(rudp_session_process_datagram_at(&s, ack, sizeof(ack), NULL, 0, 1520) == 0);
  assert((s.channels[0].chan_backoff & 0x0FU) == 4U);
  /* Subsequent ACK=2 frees clean non-retransmitted pkt 1: Karn updates RTT and resets chan_backoff. */
  h.ack = 2;
  assert(rudp_pack_datagram_header(&h, ack, sizeof(ack)) == 4);
  assert(rudp_session_process_datagram_at(&s, ack, sizeof(ack), NULL, 0, 1540) == 0);
  assert((s.channels[0].chan_backoff & 0x0FU) == 0U);

  /* Adaptive backoff shift > 6 (exceeding 64x) bounded by rto_max_ms when rto_min_ms is small. */
  assert(rudp_session_init(&s) == RUDP_OK);
  assert(rudp_session_config_recovery(&s, 0, 10, 10, 2000) == RUDP_OK);
  assert(rudp_session_send_reliable(&s, 0, (tfv_packet_u){.type = 1, .value = 9}, 0) == RUDP_OK);
  assert(rudp_session_build_datagram(&s, 0, out, sizeof(out), 0, 10) == 12);
  uint32_t t = 0;
  for (uint8_t step = 0; step < 9U; step++) {
    uint32_t wait = 10U << step;
    if (wait > 2000U) wait = 2000U;
    /* Before RTO deadline (strict > comparison): nothing sent at exact boundary t + wait. */
    assert(rudp_session_build_datagram(&s, 0, out, sizeof(out), t + wait, 10) == 4);
    t += wait + 1U;
    /* At t + wait + 1 (including >64x at step 7 => 1280ms and capped at step 8 => 2000ms): retransmit fires. */
    assert(rudp_session_build_datagram(&s, 0, out, sizeof(out), t, 10) == 12);
  }
  assert(s.channels[0].timeout_backoffs[0] == 9U);
  assert(s.channels[0].chan_backoff == 9U);
  puts("PASS: channel backoff inheritance, Karn reset, and >64x backoff capped at rto_max_ms");
}

static void motion_cold_start_and_stream_reset(void) {
  rudp_motion_tx_s m;
  memset(&m, 0, sizeof(m));
  uint8_t out[16];
  /* Constant-acceleration ramp (100, 110, 130, 160 -> v=+10,+20,+30, a=+10, true jerk=0): zero false jerk on warmup. */
  assert(rudp_motion_encode(&m, 100, 100000U, 1U, out, sizeof(out)) == 1);
  assert(rudp_motion_encode(&m, 110, 100000U, 1U, out, sizeof(out)) == 1);
  assert(rudp_motion_encode(&m, 130, 100000U, 1U, out, sizeof(out)) == 1);
  assert(rudp_motion_encode(&m, 160, 100000U, 1U, out, sizeof(out)) == 1);
  /* Actual non-zero jerk (v=+50, a=+20, j=+10) triggers duplicate emission once >= 3 prior samples exist. */
  assert(rudp_motion_encode(&m, 210, 100000U, 1U, out, sizeof(out)) == 2);

  rudp_stream_tx_s stx;
  rudp_stream_rx_s srx;
  uint8_t src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t dst[8] = {0};
  assert(rudp_stream_start(&stx, src, sizeof(src)) == 0);
  srx.data = dst;
  srx.capacity = sizeof(dst);
  srx.offset = 5;
  srx.length = 8;
  srx.active = 1;
  srx.discarding = 1;
  assert(rudp_stream_tx_reset(&stx) == RUDP_OK);
  assert(stx.data == NULL && stx.length == 0 && stx.offset == 0 && !stx.active && !stx.descriptor_sent);
  assert(rudp_stream_rx_reset(&srx) == RUDP_OK);
  assert(srx.data == dst && srx.capacity == sizeof(dst) && srx.offset == 0 && srx.length == 0 && !srx.active && !srx.discarding);
  assert(rudp_stream_tx_reset(NULL) == RUDP_ERR_INVALID_ARG);
  assert(rudp_stream_rx_reset(NULL) == RUDP_ERR_INVALID_ARG);
  puts("PASS: motion cold-start gating and stream tx/rx reset");
}

int main(void) {
  recovery();
  channel_backoff_inheritance_and_cap();
  motion_cold_start_and_stream_reset();
  iovec_bounds();
  fast_retransmit_unsent();
  reassembly(); priorities(); scalars();
  stream(0, 0); stream(1, 1025); stream(4, 1025); stream(1025, 1025); stream(1025, 4);
  stream(65535, 65535);
  puts("PASS: bounded RX, wrap, backpressure, priorities, multipart, compact, rolling, motion");
  return 0;
}
