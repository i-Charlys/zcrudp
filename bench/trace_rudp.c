/* Deterministic real-engine trace for the visual demo, not a benchmark.
 * Scripted loss/reordering highlights behavior; no protocol logic is replaced. */
#include "protocol_rudp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define END_MS 360
#define PACKETS 256
typedef struct { uint8_t bytes[12]; int len, from, active; unsigned due; } queued_s;
typedef struct {
  unsigned pending, buffered[64], buffered_count, delivered, fresh, late, retries, drops, rto;
  uint16_t latest;
} snapshot_s;
static rudp_session_s peers[2];
static queued_s queue[PACKETS];
static snapshot_s snapshots[END_MS + 1];
static unsigned packet_count, retries, drops, delivered, fresh, late;
static uint16_t latest;

static void emit(int from, const uint8_t *bytes, int length, unsigned now) {
  assert(packet_count < PACKETS && (length == 4 || length == 12));
  rudp_record_s record = {0};
  const char *kind = "ack";
  unsigned seq = ((unsigned)bytes[0] << 8) | bytes[1], retry = 0, delay = 15;
  int lost = 0;
  if (length == 12) {
    assert(rudp_unpack_record(bytes + 4, 8, &record) == 0);
    seq = record.seq_num;
    kind = record.flags == RUDP_RECORD_FLAG_RELIABLE ? "reliable" : "unreliable";
    if (record.flags == RUDP_RECORD_FLAG_RELIABLE) {
      retry = peers[from].channels[0].ctx.tx_buffer[seq & (RUDP_WINDOW_SIZE - 1)].retries;
      if (retry) retries++;
      if (seq == 2 && !retry) lost = 1;
      if (seq == 3 && !retry) delay = 70;
    } else {
      if (seq == 4) delay = 80;
      if (seq == 9) lost = 1;
    }
  }
  if (lost) drops++;
  printf("%s{\"id\":%u,\"from\":%d,\"t\":%u,\"arrival\":%u,"
         "\"kind\":\"%s\",\"seq\":%u,\"retry\":%u,\"drop\":%s}",
         packet_count ? "," : "", packet_count, from, now, now + delay,
         kind, seq, retry, lost ? "true" : "false");
  queued_s *q = &queue[packet_count++];
  memcpy(q->bytes, bytes, (size_t)length);
  q->len = length; q->from = from; q->due = now + delay; q->active = !lost;
}

int main(void) {
  for (int i = 0; i < 2; ++i) {
    assert(rudp_session_init(&peers[i]) == 0);
    assert(rudp_session_config_channel(&peers[i], 0, RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED) == 0);
    assert(rudp_session_config_channel(&peers[i], 1, RUDP_CHANNEL_FLAG_UNRELIABLE) == 0);
    assert(rudp_session_config_recovery(&peers[i], 0, 100, 10, 2000) == 0);
  }
  printf("{\"duration\":%d,\"sessionBytes\":%zu,\"packets\":[", END_MS, sizeof(rudp_session_s));
  for (unsigned now = 0; now <= END_MS; ++now) {
    uint8_t bytes[12];
    for (unsigned i = 0; i < packet_count; ++i) {
      queued_s *q = &queue[i];
      if (!q->active || q->due > now) continue;
      q->active = 0;
      rudp_record_s records[64];
      int count = rudp_session_process_datagram_at(&peers[1-q->from], q->bytes,
                                                   (size_t)q->len, records, 64, now);
      assert(count >= 0);
      if (q->len == 12 && q->bytes[5] == RUDP_RECORD_FLAG_UNRELIABLE && !count) late++;
      for (int n = 0; n < count; ++n) {
        assert(q->from == 0);
        if (records[n].flags == RUDP_RECORD_FLAG_RELIABLE) {
          assert(records[n].seq_num == delivered && records[n].payload.value == delivered);
          delivered++;
        } else {
          assert(!fresh || records[n].seq_num > latest);
          latest = records[n].seq_num; fresh++;
        }
      }
    }
    if (now % 20 == 0 && now / 20 < 8) {
      tfv_packet_u payload = {.type = 1, .flags = 0, .value = (uint16_t)(now / 20)};
      assert(rudp_session_send_reliable(&peers[0], 0, payload, now) == 0);
    }
    if (now % 15 == 0 && now / 15 < 12) {
      tfv_packet_u payload = {.type = 2, .flags = 0, .value = (uint16_t)(now / 15)};
      assert(rudp_session_send_unreliable(&peers[0], 1, payload, 0, bytes, 12) == 12);
      emit(0, bytes, 12, now);
    }
    for (int side = 0; side < 2; ++side) {
      int pending_ack = peers[side].channels[0].ack_pending;
      int size = rudp_session_build_datagram(&peers[side], 0, bytes, 12, now, 100);
      assert(size >= 4 && peers[side].channels[0].ctx.state == RUDP_STATE_CONNECTED);
      if (size > 4 || pending_ack) emit(side, bytes, size, now);
    }
    snapshot_s *s = &snapshots[now];
    rudp_channel_s *rx = &peers[1].channels[0];
    rudp_context_s *tx = &peers[0].channels[0].ctx;
    s->pending = (tx->head - tx->tail) & (RUDP_WINDOW_SIZE - 1);
    for (unsigned offset = 0; offset < RUDP_WINDOW_SIZE; ++offset) {
      unsigned seq = (uint16_t)(rx->ctx.expected_seq_num + offset);
      unsigned slot = seq & (RUDP_WINDOW_SIZE - 1);
      if (rx->rx_present[slot / 8] & (1U << (slot & 7))) s->buffered[s->buffered_count++] = seq;
    }
    s->delivered = delivered; s->fresh = fresh; s->latest = latest;
    s->late = late; s->retries = retries; s->drops = drops; s->rto = peers[0].channels[0].rto_ms;
  }
  assert(delivered == 8 && fresh == 10 && latest == 11 && late == 1 && drops == 2);
  assert(snapshots[END_MS].pending == 0 && retries > 0);
  int saw_independence = 0;
  for (unsigned t = 1; t <= END_MS; ++t)
    if (snapshots[t].buffered_count && snapshots[t].fresh > snapshots[t-1].fresh) saw_independence = 1;
  assert(saw_independence);
  printf("],\"snapshots\":[");
  for (unsigned t = 0; t <= END_MS; ++t) {
    snapshot_s *s = &snapshots[t];
    printf("%s{\"t\":%u,\"pending\":%u,\"delivered\":%u,\"fresh\":%u,"
           "\"latest\":%u,\"late\":%u,\"retries\":%u,\"drops\":%u,\"rto\":%u,\"buffer\":[",
           t ? "," : "", t, s->pending, s->delivered, s->fresh, s->latest,
           s->late, s->retries, s->drops, s->rto);
    for (unsigned i = 0; i < s->buffered_count; ++i) printf("%s%u", i ? "," : "", s->buffered[i]);
    printf("]}");
  }
  puts("]}");
  return 0;
}
