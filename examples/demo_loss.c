#define _POSIX_C_SOURCE 200809L
#include "protocol_rudp.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* POSIX host example; protocol state and simulated network queue are bounded.
 * Both peers have explicit addresses: no discovery or authentication is implied. */
#define QUEUE_SIZE 512
#define MAX_DGRAM_LEN 64
typedef struct {
  uint8_t bytes[MAX_DGRAM_LEN];
  size_t len;
  uint64_t due;
  int used;
} delayed_s;
static delayed_s queue[QUEUE_SIZE];
static rudp_session_s session;
static uint32_t rng;
static unsigned loss, latency, jitter;
static unsigned dropped, sent, received, delivered, retried, overflow;
static volatile sig_atomic_t running = 1;

static void stop(int sig) { (void)sig; running = 0; }

static uint64_t now_ms(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) { perror("clock_gettime"); exit(1); }
  return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static uint32_t random_next(void) {
  rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
  return rng;
}

static int number(const char *s, unsigned max, unsigned *out) {
  char *end;
  errno = 0;
  unsigned long v = strtoul(s, &end, 10);
  if (errno || !*s || *s == '-' || *end || v > max) return 0;
  *out = (unsigned)v;
  return 1;
}

/* Injection applies once to every outgoing datagram, including ACKs/retries.
 * Jitter is additive [0,jitter] ms and can cause reordering. */
static void enqueue(const uint8_t *bytes, size_t len) {
  if (len > sizeof(queue[0].bytes)) {
    overflow++;
    fprintf(stderr, "DROP datagram exceeds queue slot capacity (%zu > %zu)\n", len, sizeof(queue[0].bytes));
    return;
  }
  if (random_next() % 100 < loss) {
    dropped++;
    printf("DROP simulated len=%zu\n", len);
    return;
  }
  for (unsigned i = 0; i < QUEUE_SIZE; i++) {
    if (!queue[i].used) {
      memcpy(queue[i].bytes, bytes, len);
      queue[i].len = len;
      queue[i].due = now_ms() + latency + random_next() % (jitter + 1);
      queue[i].used = 1;
      return;
    }
  }
  overflow++;
  fprintf(stderr, "DROP simulation queue full\n");
}

static int flush(int fd, const struct sockaddr_in *peer) {
  uint64_t now = now_ms();
  for (unsigned i = 0; i < QUEUE_SIZE; i++) {
    if (!queue[i].used || queue[i].due > now) continue;
    ssize_t n = sendto(fd, queue[i].bytes, queue[i].len, 0,
                       (const struct sockaddr *)peer, sizeof(*peer));
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
    if (n != (ssize_t)queue[i].len) { perror("sendto"); return -1; }
    sent++;
    queue[i].used = 0;
  }
  return 0;
}

static void emit_reliable(uint16_t slot) {
  const rudp_frame_s *f = rudp_get_slot_frame(&session.channels[0].ctx, slot);
  uint8_t bytes[12];
  rudp_datagram_header_s h = {session.channels[0].ctx.expected_seq_num, 0, 1};
  rudp_record_s r = {0, RUDP_RECORD_FLAG_RELIABLE, f->header.seq_num, f->packet};
  rudp_pack_datagram_header(&h, bytes, sizeof bytes);
  rudp_pack_record(&r, bytes + 4, sizeof bytes - 4);
  enqueue(bytes, sizeof bytes);
}

static int send_value(int reliable, unsigned value) {
  tfv_packet_u p = {.type = 1, .flags = 0, .value = (uint16_t)value};
  if (reliable) {
    uint16_t slot = session.channels[0].ctx.head;
    int ret = rudp_session_send_reliable(&session, 0, p, (uint32_t)now_ms());
    if (ret != RUDP_OK) { fprintf(stderr, "Reliable send rejected: %d\n", ret); return -1; }
    emit_reliable(slot);
  } else {
    uint8_t bytes[12];
    int n = rudp_session_send_unreliable(&session, 1, p, 0, bytes, sizeof bytes);
    if (n < 0) return -1;
    enqueue(bytes, (size_t)n);
  }
  printf("TX %s value=%u\n", reliable ? "reliable" : "unreliable", value);
  return 0;
}

static void stats(void) {
  printf("STATS sent=%u simulated_loss=%u queue_overflow=%u received=%u delivered=%u retries=%u pending=%u\n",
         sent, dropped, overflow, received, delivered, retried,
         (unsigned)((session.channels[0].ctx.head - session.channels[0].ctx.tail) & (RUDP_WINDOW_SIZE - 1)));
}

static void command(char *line) {
  unsigned value;
  if (!strcmp(line, "q")) running = 0;
  else if (!strcmp(line, "stats")) stats();
  else if ((line[0] == 'r' || line[0] == 'u') && line[1] == ' ' && number(line + 2, 65535, &value))
    (void)send_value(line[0] == 'r', value);
  else puts("Commands: r <0..65535>, u <0..65535>, stats, q");
}

static void usage(const char *name) {
  printf("Usage: %s server|client [options]\n"
         "  --bind IPv4 --peer IPv4 (default 127.0.0.1)\n"
         "  --port N --peer-port N (server 9000/9001, client 9001/9000)\n"
         "  --loss 0..100 --latency MS --jitter MS --seed N\n"
         "  --timeout MS (default 500, measured from enqueue)\n"
         "  --count N (send N reliable AND N unreliable values automatically)\n"
         "  --interval MS (default 100) --duration MS (0 = until q/Ctrl-C)\n"
         "Each peer injects loss/delay on its own outgoing traffic.\n", name);
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--help")) { usage(argv[0]); return 0; }
  if (argc < 2 || (strcmp(argv[1], "server") && strcmp(argv[1], "client"))) { usage(argv[0]); return 2; }
  int server = !strcmp(argv[1], "server");
  unsigned port = server ? 9000 : 9001, peer_port = server ? 9001 : 9000;
  unsigned seed = server ? 1 : 2, timeout = 500, count = 0, interval = 100, duration = 0;
  const char *bind_ip = "127.0.0.1", *peer_ip = "127.0.0.1";
  for (int i = 2; i < argc; i++) {
    const char *key = argv[i];
    if (!strcmp(key, "--help")) { usage(argv[0]); return 0; }
    if (++i == argc) { usage(argv[0]); return 2; }
    if (!strcmp(key, "--bind")) bind_ip = argv[i];
    else if (!strcmp(key, "--peer")) peer_ip = argv[i];
    else {
      unsigned *target = NULL, max = 3600000;
      if (!strcmp(key, "--port")) { target = &port; max = 65535; }
      else if (!strcmp(key, "--peer-port")) { target = &peer_port; max = 65535; }
      else if (!strcmp(key, "--seed")) { target = &seed; max = UINT32_MAX; }
      else if (!strcmp(key, "--loss")) { target = &loss; max = 100; }
      else if (!strcmp(key, "--latency")) target = &latency;
      else if (!strcmp(key, "--jitter")) target = &jitter;
      else if (!strcmp(key, "--timeout")) target = &timeout;
      else if (!strcmp(key, "--count")) { target = &count; max = 65535; }
      else if (!strcmp(key, "--interval")) target = &interval;
      else if (!strcmp(key, "--duration")) target = &duration;
      if (!target || !number(argv[i], max, target)) { fprintf(stderr, "Invalid option: %s %s\n", key, argv[i]); return 2; }
    }
  }
  if (!port || !peer_port || !seed || !interval || !timeout) { fprintf(stderr, "Ports, seed, interval and timeout must be positive\n"); return 2; }
  rng = seed;
  struct sockaddr_in local = {0}, peer = {0};
  local.sin_family = peer.sin_family = AF_INET;
  local.sin_port = htons((uint16_t)port); peer.sin_port = htons((uint16_t)peer_port);
  if (inet_pton(AF_INET, bind_ip, &local.sin_addr) != 1 || inet_pton(AF_INET, peer_ip, &peer.sin_addr) != 1) {
    fprintf(stderr, "Expected numeric IPv4 addresses\n"); return 2;
  }
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) { perror("socket"); return 1; }
  if (bind(fd, (struct sockaddr *)&local, sizeof local) < 0) { perror("bind"); close(fd); return 1; }
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) { perror("fcntl"); close(fd); return 1; }
  rudp_session_init(&session);
  rudp_session_config_channel(&session, 0, RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED);
  rudp_session_config_channel(&session, 1, RUDP_CHANNEL_FLAG_UNRELIABLE);
  signal(SIGINT, stop); signal(SIGTERM, stop);
  setvbuf(stdout, NULL, _IOLBF, 0);
  printf("READY %s %s:%u -> %s:%u loss=%u%% latency=%ums jitter=0..%ums seed=%u\n",
         argv[1], bind_ip, port, peer_ip, peer_port, loss, latency, jitter, seed);
  puts("Commands: r <value>, u <value>, stats, q");
  uint64_t start = now_ms(), next = start;
  unsigned generated = 0;
  int status = 0;
  struct pollfd fds[2] = {{fd, POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
  char line[128]; size_t used = 0; int too_long = 0;
  while (running && (!duration || now_ms() - start < duration)) {
    if (generated < count && now_ms() >= next) {
      if (send_value(1, generated) == 0) { (void)send_value(0, generated); generated++; }
      next = now_ms() + interval;
    }
    uint16_t indices[RUDP_WINDOW_SIZE];
    rudp_tick_result_s tick = rudp_tick(&session.channels[0].ctx, (uint32_t)now_ms(), timeout, indices, RUDP_WINDOW_SIZE);
    if (tick.status != RUDP_OK) { fprintf(stderr, "Reliable channel disconnected (retry limit)\n"); status = 1; break; }
    for (int i = 0; i < tick.count; i++) { emit_reliable(indices[i]); retried++; }
    if (flush(fd, &peer) < 0) { status = 1; break; }
    int ready = poll(fds, 2, 5);
    if (ready < 0) { if (errno == EINTR) continue; perror("poll"); status = 1; break; }
    if (fds[0].revents & POLLIN) {
      uint8_t bytes[2048]; struct sockaddr_in from; socklen_t from_len = sizeof from;
      ssize_t n = recvfrom(fd, bytes, sizeof bytes, 0, (struct sockaddr *)&from, &from_len);
      if (n < 0) { if (errno == EINTR) continue; perror("recvfrom"); status = 1; break; }
      if (from.sin_addr.s_addr != peer.sin_addr.s_addr || from.sin_port != peer.sin_port) continue;
      rudp_record_s records[255];
      int got = rudp_session_process_datagram(&session, bytes, (size_t)n, records, 255);
      received++;
      if (got < 0) fprintf(stderr, "Rejected datagram: %d\n", got);
      else for (int i = 0; i < got; i++) {
        printf("RX %s seq=%u value=%u\n", records[i].channel_id == 0 ? "reliable" : "unreliable",
               records[i].seq_num, records[i].payload.value);
        delivered++;
      }
      if (session.channels[0].ack_pending) {
        rudp_datagram_header_s ack = {session.channels[0].ctx.expected_seq_num, 0, 0};
        rudp_pack_datagram_header(&ack, bytes, sizeof bytes);
        enqueue(bytes, 4);
        session.channels[0].ack_pending = 0;
      }
    }
    if (fds[1].revents & (POLLIN | POLLHUP)) {
      char input[128]; ssize_t n = read(STDIN_FILENO, input, sizeof input);
      if (n == 0) fds[1].fd = -1; /* EOF leaves the network peer running. */
      else if (n < 0 && errno != EINTR) fds[1].fd = -1;
      else for (ssize_t i = 0; i < n; i++) {
        if (input[i] == '\n') {
          line[used] = '\0';
          if (!too_long) command(line); else fprintf(stderr, "Command too long\n");
          used = 0; too_long = 0;
        } else if (input[i] != '\r') {
          if (used + 1 < sizeof line) line[used++] = input[i]; else too_long = 1;
        }
      }
    }
  }
  stats();
  unsigned queued = 0;
  for (unsigned i = 0; i < QUEUE_SIZE; i++) queued += queue[i].used != 0;
  printf("EXIT generated=%u queued_on_exit=%u\n", generated, queued);
  close(fd);
  return status;
}
