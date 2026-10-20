#include "rudp_session.h"
#include "rudp_reliability.h"
#include "rudp_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#if !RUDP_EXTERNAL_RX_STORAGE
#error "This suite requires caller-owned RX storage"
#endif

static size_t data(uint8_t *bytes, uint8_t channel, uint16_t seq, uint8_t flags,
                   uint16_t ack) {
  rudp_datagram_header_s h = {.ack = ack, .ack_channel = 0, .count = 1};
  rudp_record_s r = {
      .channel_id = channel, .flags = flags, .seq_num = seq, .payload = {.value = seq}};
  assert(rudp_pack_datagram_header(&h, bytes, 12) == 4);
  assert(rudp_pack_record(&r, bytes + 4, 8) == 8);
  return 12;
}

int main(void) {
  rudp_session_s s, before;
  rudp_rx_storage_s storage;
  rudp_record_s out[2];
  uint8_t bytes[12];
  assert(rudp_session_init(&s) == RUDP_OK);
  assert(s.channels[0].rx_buffer == NULL);
  assert(rudp_session_poll(&s, out, 2) == 0);
  assert(rudp_session_config_recovery(&s, 0, 100, 20, 500) == RUDP_OK);

  /* Reliable input without backing memory must not even apply its header ACK. */
  tfv_packet_u payload = {.raw = 0};
  assert(rudp_session_send_reliable(&s, 0, payload, 0) == RUDP_OK);
  memcpy(&before, &s, sizeof(s));
  assert(rudp_session_process_datagram(&s, bytes,
                                       data(bytes, 0, 0, RUDP_RECORD_FLAG_RELIABLE, 1),
                                       out, 2) == RUDP_ERR_INVALID_ARG);
  assert(memcmp(&before, &s, sizeof(s)) == 0);
  assert(rudp_session_attach_rx(&s, 0, &storage) == RUDP_ERR_BUFFER_FULL);
  assert(rudp_recv_ack(&s.channels[0].ctx, 1) == RUDP_OK);
  assert(rudp_session_attach_rx(&s, 0, &storage) == RUDP_OK);
#if RUDP_MAX_CHANNELS > 1
  assert(rudp_session_attach_rx(&s, 1, &storage) == RUDP_ERR_INVALID_ARG);
#endif
  assert(rudp_session_attach_rx(NULL, 0, &storage) == RUDP_ERR_INVALID_ARG);
  assert(rudp_session_attach_rx(&s, RUDP_MAX_CHANNELS, &storage) ==
         RUDP_ERR_INVALID_ARG);

  /* Gaps are retained; detach cannot discard accepted reliable data. */
  assert(rudp_session_process_datagram(
             &s, bytes, data(bytes, 0, 1, RUDP_RECORD_FLAG_RELIABLE, 1), out, 1) == 0);
  assert(rudp_session_attach_rx(&s, 0, NULL) == RUDP_ERR_BUFFER_FULL);
  assert(rudp_session_config_recovery(&s, 0, 100, 20, 500) == RUDP_ERR_BUFFER_FULL);
  assert(rudp_session_process_datagram(
             &s, bytes, data(bytes, 0, 0, RUDP_RECORD_FLAG_RELIABLE, 1), out, 1) == 1);
  assert(out[0].seq_num == 0);
  assert(s.channels[0].ctx.expected_seq_num == 1);
  assert(rudp_session_poll(&s, out, 2) == 1);
  assert(out[0].seq_num == 1 && out[0].payload.value == 1);
  assert(rudp_session_process_datagram(
             &s, bytes, data(bytes, 0, 1, RUDP_RECORD_FLAG_RELIABLE, 1), out, 2) == 0);
  assert(rudp_session_attach_rx(&s, 0, NULL) == RUDP_OK);

  /* Unreliable records and ACKs remain usable without reliable RX storage. */
  assert(rudp_session_process_datagram(
             &s, bytes, data(bytes, 0, 5, RUDP_RECORD_FLAG_UNRELIABLE, 1), out, 2) ==
         1);
  assert(out[0].seq_num == 5);
  assert(rudp_pack_datagram_header(
             &(rudp_datagram_header_s){.ack = 1, .ack_channel = 0, .count = 0}, bytes,
             12) == 4);
  assert(rudp_session_process_datagram(&s, bytes, 4, NULL, 0) == 0);

  assert(rudp_session_attach_rx(&s, 0, &storage) == RUDP_OK);
  assert(rudp_session_process_datagram(
             &s, bytes, data(bytes, 0, 3, RUDP_RECORD_FLAG_RELIABLE, 1), out, 2) == 0);
  assert(rudp_session_reset(&s) == RUDP_OK);
  assert(s.channels[0].rx_buffer == storage.packets);
  for (size_t i = 0; i < sizeof(storage.present); ++i)
    assert(storage.present[i] == 0);
  /* Wraparound on caller-owned memory. */
  s.channels[0].ctx.expected_seq_num = UINT16_MAX;
  assert(rudp_session_process_datagram(
             &s, bytes, data(bytes, 0, 0, RUDP_RECORD_FLAG_RELIABLE, 0), out, 2) == 0);
  assert(rudp_session_process_datagram(
             &s, bytes, data(bytes, 0, UINT16_MAX, RUDP_RECORD_FLAG_RELIABLE, 0), out,
             2) == 2);
  assert(out[0].seq_num == UINT16_MAX && out[1].seq_num == 0);
  assert(rudp_session_attach_rx(&s, 0, NULL) == RUDP_OK);
  printf("PASS: external RX ownership, atomic rejection, gaps, backpressure, reset, "
         "wrap; session=%zu B, RX storage=%zu B\n",
         sizeof(s), sizeof(storage));
  return 0;
}
