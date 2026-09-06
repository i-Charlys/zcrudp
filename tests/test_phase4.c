#include "protocol_profiles.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int inject(rudp_session_s *rx, uint8_t channel, uint16_t seq,
                  uint16_t value, rudp_record_s *out, size_t capacity) {
  uint8_t bytes[12];
  rudp_datagram_header_s header = {0, 0, 1};
  rudp_record_s record = {0};
  record.channel_id = channel; record.flags = RUDP_RECORD_FLAG_RELIABLE;
  record.seq_num = seq; record.payload.value = value;
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
  tfv_packet_u payload = {0};
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
  rudp_stream_tx_s tx = {0};
  static uint8_t source[65535], destination[65537];
  uint8_t bytes[1400], ack[4];
  rudp_record_s out[175];
  memset(destination, 165, sizeof destination);
  for (size_t i = 0; i < sizeof source; ++i) source[i] = (uint8_t)i;
  rudp_stream_rx_s rx = {0};
  rx.data = destination + 1; rx.capacity = capacity;
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
  tfv_packet_u descriptor = {0};
  assert(rudp_stream_receive(&rx, descriptor) < 0);
  descriptor.type = 255; descriptor.flags = 211;
  assert(rudp_stream_receive(&rx, descriptor) == 1); /* Resync after discard. */
  assert(rudp_stream_start(&tx, NULL, 1) < 0);
  assert(rudp_stream_start(&tx, source, 65536) < 0);
  assert(rudp_stream_start(&tx, NULL, 0) == 0);
  assert(rudp_session_config_channel(&sender, 0, RUDP_CHANNEL_FLAG_UNRELIABLE) == 0);
  assert(rudp_stream_pump(&tx, &sender, 0, 0) < 0);
}

static void scalars(void) {
  rudp_scalar_tx_s tx = {0};
  rudp_scalar_rx_s rx = {0};
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
  rudp_motion_tx_s motion = {0};
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
  rudp_datagram_header_s header = {seq, 0, 0};
  uint8_t bytes[4];
  assert(rudp_pack_datagram_header(&header, bytes, 4) == 4);
  return rudp_session_process_datagram_at(session, bytes, 4, NULL, 0, now);
}

static void recovery(void) {
  rudp_session_s session;
  uint8_t bytes[1400];
  tfv_packet_u payload = {0};
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
  assert(!chan->srtt_scaled && chan->rto_ms == 2000 && !chan->timeout_backoffs[2]);

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

int main(void) {
  recovery();
  reassembly(); priorities(); scalars();
  stream(0, 0); stream(1, 1025); stream(4, 1025); stream(1025, 1025); stream(1025, 4);
  stream(65535, 65535);
  puts("PASS: bounded RX, wrap, backpressure, priorities, multipart, compact, rolling, motion");
  return 0;
}
