#define _POSIX_C_SOURCE 200809L
#include "protocol_rudp.h"
#include "ikcp.h"
#include <enet/enet.h>
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Real protocol engines over a shared virtual datagram link. No OS sockets.
 * One virtual tick = 1 ms; every engine gets one service pass per endpoint/tick.
 * ENet's platform functions below replace only its clock and socket backend. */
#define CAP 8192
#define MTU 1400
#define MAX_MESSAGES 60000
typedef struct { uint32_t due; uint64_t order; unsigned len; uint8_t bytes[MTU]; } datagram_s;
static datagram_s heaps[2][CAP];
static unsigned sizes[2];
static uint32_t now, delay_ms, jitter_ms, loss_percent, rng, origin;
static uint64_t serial, wire_bytes, wire_packets, lost_packets;
static unsigned delivered, admitted, total, offered_rate;
static uint32_t born[MAX_MESSAGES], latencies[MAX_MESSAGES];
static rudp_session_s z[2];
static ikcpcb *k[2];
static ENetHost *hosts[2];
static ENetPeer *remote;
static int next_socket, connected, failed;

static uint64_t clock_ns(void) {
  struct timespec t;
  assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
  return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static uint32_t random_next(void) {
  rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng;
}
static int earlier(const datagram_s *a, const datagram_s *b) {
  return a->due < b->due || (a->due == b->due && a->order < b->order);
}
static void link_send(int from, const void *bytes, unsigned len) {
  assert(from == 0 || from == 1); assert(len <= MTU);
  wire_bytes += len; wire_packets++;
  if (random_next() % 100 < loss_percent) { lost_packets++; return; }
  int to = 1 - from;
  assert(sizes[to] < CAP); /* Never silently add simulator congestion loss. */
  datagram_s p;
  p.due = now + delay_ms + random_next() % (jitter_ms + 1);
  p.order = serial++; p.len = len; memcpy(p.bytes, bytes, len);
  unsigned i = sizes[to]++;
  while (i && earlier(&p, &heaps[to][(i-1)/2])) {
    heaps[to][i] = heaps[to][(i-1)/2]; i = (i-1)/2;
  }
  heaps[to][i] = p;
}
static unsigned link_receive(int to, void *bytes) {
  if (!sizes[to] || heaps[to][0].due > now) return 0;
  unsigned len = heaps[to][0].len;
  memcpy(bytes, heaps[to][0].bytes, len);
  datagram_s last = heaps[to][--sizes[to]];
  unsigned i = 0;
  while (2*i+1 < sizes[to]) {
    unsigned child = 2*i+1;
    if (child+1 < sizes[to] && earlier(&heaps[to][child+1], &heaps[to][child])) child++;
    if (!earlier(&heaps[to][child], &last)) break;
    heaps[to][i] = heaps[to][child]; i = child;
  }
  if (sizes[to]) heaps[to][i] = last;
  return len;
}
static void delivery(const uint8_t *bytes, size_t len) {
  assert(len == 4 && bytes[0] == 1 && bytes[1] == 0);
  unsigned id = ((unsigned)bytes[2] << 8) | bytes[3];
  assert(id == delivered && delivered < admitted); /* Exactly once, in order. */
  latencies[delivered++] = now - born[id];
}

/* ENet virtual platform: no protocol code is modified. */
int enet_initialize(void) { return 0; }
void enet_deinitialize(void) {}
enet_uint32 enet_time_get(void) { return now; }
enet_uint32 enet_host_random_seed(void) { return 42; }
ENetSocket enet_socket_create(ENetSocketType type) { (void)type; assert(next_socket < 2); return next_socket++; }
int enet_socket_bind(ENetSocket fd, const ENetAddress *a) { (void)fd; (void)a; return 0; }
int enet_socket_set_option(ENetSocket fd, ENetSocketOption opt, int value) { (void)fd; (void)opt; (void)value; return 0; }
void enet_socket_destroy(ENetSocket fd) { (void)fd; }
int enet_socket_get_address(ENetSocket fd, ENetAddress *a) {
  a->host = htonl(0x7f000001u + (unsigned)fd); a->port = (enet_uint16)(10000 + fd); return 0;
}
int enet_socket_wait(ENetSocket fd, enet_uint32 *condition, enet_uint32 timeout) {
  (void)fd; assert(timeout == 0); *condition = 0; return 0;
}
int enet_socket_send(ENetSocket fd, const ENetAddress *a, const ENetBuffer *buffers, size_t count) {
  (void)a;
  uint8_t bytes[MTU]; unsigned len = 0;
  for (size_t i = 0; i < count; i++) {
    assert(len + buffers[i].dataLength <= MTU);
    memcpy(bytes + len, buffers[i].data, buffers[i].dataLength);
    len += (unsigned)buffers[i].dataLength;
  }
  link_send(fd, bytes, len); return (int)len;
}
int enet_socket_receive(ENetSocket fd, ENetAddress *a, ENetBuffer *buffers, size_t count) {
  assert(count == 1 && buffers[0].dataLength >= MTU);
  enet_socket_get_address(1-fd, a);
  return (int)link_receive(fd, buffers[0].data);
}
static int kcp_output(const char *buf, int len, ikcpcb *pcb, void *user) {
  (void)pcb; link_send((int)(intptr_t)user, buf, (unsigned)len); return 0;
}

static void enet_service(int side, int setup) {
  ENetEvent e;
  int ret;
  while ((ret = enet_host_service(hosts[side], &e, 0)) > 0) {
    if (e.type == ENET_EVENT_TYPE_CONNECT) connected++;
    else if (e.type == ENET_EVENT_TYPE_RECEIVE) {
      assert(!setup && side == 1);
      delivery(e.packet->data, e.packet->dataLength);
      enet_packet_destroy(e.packet);
    } else if (e.type == ENET_EVENT_TYPE_DISCONNECT) {
      failed = 1;
    }
  }
  assert(ret == 0);
}

/* Engines: 0=zcrudp, 1=ENet defaults, 2=KCP defaults, 3=KCP fast profile. */
static void setup(unsigned engine) {
  sizes[0] = sizes[1] = 0; now = 1000;
  if (engine == 0) {
    for (int side = 0; side < 2; side++) {
      assert(rudp_session_init(&z[side]) == RUDP_OK);
      assert(rudp_session_config_channel(&z[side], 0, RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED) == RUDP_OK);
      assert(rudp_session_config_recovery(&z[side], 0, 100, 10, 2000) == RUDP_OK);
    }
  } else if (engine == 1) {
    next_socket = connected = 0;
    for (int side = 0; side < 2; side++) {
      ENetAddress a; enet_socket_get_address(side, &a);
      hosts[side] = enet_host_create(&a, 1, 1, 0, 0);
      assert(hosts[side]); hosts[side]->mtu = MTU; hosts[side]->randomSeed = 42;
    }
    ENetAddress a; enet_socket_get_address(1, &a);
    remote = enet_host_connect(hosts[0], &a, 1, 0); assert(remote);
    /* Exclude connection establishment; all engines start established. */
    uint32_t saved_loss = loss_percent;
    loss_percent = 0; /* Establish on this scenario's path, without handshake loss. */
    for (; now < 2000 && (connected < 2 || sizes[0] || sizes[1]); now++) {
      enet_service(0, 1); enet_service(1, 1);
    }
    assert(connected == 2 && !sizes[0] && !sizes[1]);
    loss_percent = saved_loss;
  } else {
    for (int side = 0; side < 2; side++) {
      k[side] = ikcp_create(42, (void *)(intptr_t)side); assert(k[side]);
      ikcp_setoutput(k[side], kcp_output);
      assert(ikcp_setmtu(k[side], MTU) == 0);
      if (engine == 3) ikcp_nodelay(k[side], 1, 10, 2, 1);
      ikcp_update(k[side], now);
    }
  }
  origin = now; wire_bytes = wire_packets = lost_packets = serial = 0;
  delivered = admitted = 0;
}

static unsigned pending(unsigned engine) {
  if (engine == 0) return (z[0].channels[0].ctx.head - z[0].channels[0].ctx.tail) & (RUDP_WINDOW_SIZE - 1);
  if (engine == 1) return (unsigned)(enet_list_size(&remote->sentReliableCommands) +
    enet_list_size(&remote->outgoingCommands) + enet_list_size(&remote->outgoingSendReliableCommands));
  return (unsigned)ikcp_waitsnd(k[0]);
}
static void submit(unsigned engine) {
  while (admitted < total && pending(engine) < 63) {
    uint32_t scheduled = offered_rate ? origin + (uint32_t)(((uint64_t)admitted * 1000 + offered_rate-1) / offered_rate) : now;
    if (scheduled > now) break;
    tfv_packet_u p = {.type=1, .flags=0, .value=(uint16_t)admitted};
    uint8_t bytes[4] = {1, 0, (uint8_t)(admitted >> 8), (uint8_t)admitted};
    if (engine == 0) assert(rudp_session_send_reliable(&z[0], 0, p, now) == RUDP_OK);
    else if (engine == 1) {
      ENetPacket *packet = enet_packet_create(bytes, 4, ENET_PACKET_FLAG_RELIABLE); assert(packet);
      assert(enet_peer_send(remote, 0, packet) == 0);
    } else assert(ikcp_send(k[0], (const char *)bytes, 4) == 4);
    born[admitted++] = scheduled;
  }
}
static void service(unsigned engine, int side) {
  uint8_t bytes[MTU]; unsigned len;
  if (engine == 1) { enet_service(side, 0); return; }
  while ((len = link_receive(side, bytes)) != 0) {
    if (engine == 0) {
      rudp_record_s records[175];
      int n = rudp_session_process_datagram_at(&z[side], bytes, len, records, 175, now); assert(n >= 0);
      for (int i = 0; i < n; i++) {
        assert(side == 1);
        uint8_t p[4]; rudp_pack_payload(&records[i].payload, p, 4); delivery(p, 4);
      }
    } else assert(ikcp_input(k[side], (const char *)bytes, (long)len) == 0);
  }
  if (engine == 0) {
    /* Do not emit empty ACKs unless an ACK was requested by RX. */
    int ack_pending = z[side].channels[0].ack_pending;
    int n = rudp_session_build_datagram(&z[side], 0, bytes, sizeof bytes, now, 100); assert(n >= 4);
    if (z[side].channels[0].ctx.state != RUDP_STATE_CONNECTED) { failed = 1; return; }
    if (n > 4 || ack_pending) link_send(side, bytes, (unsigned)n);
  } else {
    int n;
    while ((n = ikcp_recv(k[side], (char *)bytes, sizeof bytes)) >= 0) {
      assert(side == 1); delivery(bytes, (size_t)n);
    }
    ikcp_update(k[side], now);
    if (k[side]->state == (IUINT32)-1) failed = 1;
  }
}
static int compare_u32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return (x > y) - (x < y);
}
static uint32_t percentile(unsigned pct) {
  unsigned i = (delivered * pct + 99) / 100;
  return latencies[i ? i-1 : 0];
}

int main(int argc, char **argv) {
  if (argc != 7) {
    fprintf(stderr, "Usage: compare_transport ENGINE(0..3) COUNT RATE(0=saturated) DELAY_MS LOSS_PERCENT SEED\n"); return 2;
  }
  unsigned engine = (unsigned)strtoul(argv[1], NULL, 10);
  total = (unsigned)strtoul(argv[2], NULL, 10); offered_rate = (unsigned)strtoul(argv[3], NULL, 10);
  delay_ms = (unsigned)strtoul(argv[4], NULL, 10); loss_percent = (unsigned)strtoul(argv[5], NULL, 10);
  uint32_t seed = (uint32_t)strtoul(argv[6], NULL, 10);
  assert(engine < 4 && total > 0 && total <= MAX_MESSAGES && delay_ms > 0 && loss_percent <= 100 && seed);
  jitter_ms = loss_percent ? 5 : 0; rng = seed;
  setup(engine); rng = seed;
  uint64_t start = clock_ns();
  uint32_t end = origin;
  int all_delivered = 0;
  for (; now - origin < 120000; now++) {
    submit(engine); service(engine, 0); service(engine, 1);
    if (failed) break;
    if (!all_delivered && delivered == total) { end = now; all_delivered = 1; }
    if (all_delivered && pending(engine) == 0) break; /* Include final ACK traffic. */
  }
  uint64_t elapsed = clock_ns() - start;
  int complete = all_delivered && pending(engine) == 0 && !failed;
  if (!all_delivered) end = now;
  qsort(latencies, delivered, sizeof latencies[0], compare_u32);
  const char *names[] = {"zcrudp", "ENet", "KCP-default", "KCP-fast"};
  printf("%s,%u,%u,%u,%u,%u,%u,%u,%.3f,%u,%u,%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.3f,%d\n",
    names[engine], seed, total, offered_rate, delay_ms, loss_percent, delivered, end-origin,
    (double)delivered*1000/(end-origin), delivered ? percentile(50) : 0,
    delivered ? percentile(95) : 0, delivered ? percentile(99) : 0,
    wire_bytes, wire_packets, lost_packets, (double)elapsed/(delivered ? delivered : 1), complete);
  if (engine == 1) { enet_host_destroy(hosts[0]); enet_host_destroy(hosts[1]); }
  else if (engine >= 2) { ikcp_release(k[0]); ikcp_release(k[1]); }
  return complete ? 0 : 1;
}
