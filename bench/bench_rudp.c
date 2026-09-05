#define _POSIX_C_SOURCE 200809L
#include "protocol_rudp.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

#define SAMPLES 7
#define CASES 5
static volatile uint64_t sink;
static const char *names[CASES] = {
  "frame_encode_8B", "frame_decode_8B", "record_encode_8B",
  "record_decode_8B", "datagram_decode_16_records"
};
typedef struct { double low, median, high; } result_s;

static uint64_t clock_ns(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock_gettime"); exit(1); }
  return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

/* Separate translation units, no LTO: codec calls remain in the measured loop.
 * Inputs vary and outputs feed an observable checksum. Timer read only at batch
 * boundaries; results include loop/checksum overhead (no baseline subtraction). */
static double measure(unsigned kind, uint64_t iterations) {
  uint8_t bytes[132] = {0};
  rudp_frame_s frame = {{123, 456}, {.type=1, .flags=2, .value=789}};
  rudp_record_s record = {1, 0, 123, {.type=1, .flags=2, .value=789}};
  rudp_datagram_header_s header = {456, 0, 16};
  rudp_record_s records[16];
  if (kind < 2) rudp_pack_frame(&frame, bytes, sizeof bytes);
  else if (kind < 4) rudp_pack_record(&record, bytes, sizeof bytes);
  else {
    rudp_pack_datagram_header(&header, bytes, sizeof bytes);
    for (unsigned i = 0; i < 16; i++) rudp_pack_record(&record, bytes + 4 + 8*i, 8);
  }
  uint64_t checksum = 0, start = clock_ns();
  /* Dispatch outside the hot loop. */
  switch (kind) {
    case 0:
      for (uint64_t i = 0; i < iterations; i++) {
        frame.packet.value = (uint16_t)i;
        int ret = rudp_pack_frame(&frame, bytes, 8);
        checksum += (unsigned)ret + bytes[7];
      }
      break;
    case 1:
      for (uint64_t i = 0; i < iterations; i++) {
        bytes[7] = (uint8_t)i;
        int ret = rudp_unpack_frame(bytes, 8, &frame);
        checksum += (unsigned)ret + frame.packet.value;
      }
      break;
    case 2:
      for (uint64_t i = 0; i < iterations; i++) {
        record.payload.value = (uint16_t)i;
        int ret = rudp_pack_record(&record, bytes, 8);
        checksum += (unsigned)ret + bytes[7];
      }
      break;
    case 3:
      for (uint64_t i = 0; i < iterations; i++) {
        bytes[7] = (uint8_t)i;
        int ret = rudp_unpack_record(bytes, 8, &record);
        checksum += (unsigned)ret + record.payload.value;
      }
      break;
    default:
      for (uint64_t i = 0; i < iterations; i++) {
        bytes[131] = (uint8_t)i;
        int ret = rudp_unpack_datagram(bytes, sizeof bytes, &header, records, 16);
        checksum += (unsigned)ret + records[15].payload.value;
      }
      break;
  }
  uint64_t elapsed = clock_ns() - start;
  sink += checksum;
  return (double)elapsed / (double)iterations;
}

static int compare(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

static int csv_report(const char *path, const result_s *results, uint64_t iterations) {
  FILE *f = fopen(path, "w");
  if (!f) { perror(path); return -1; }
  fprintf(f, "operation,iterations,samples,min_ns_per_op,median_ns_per_op,max_ns_per_op,Mops_per_s,Mrecords_per_s\n");
  for (unsigned k = 0; k < CASES; k++) {
    double rate = 1000.0 / results[k].median;
    fprintf(f, "%s,%" PRIu64 ",%d,%.3f,%.3f,%.3f,%.3f,%.3f\n", names[k], iterations,
            SAMPLES, results[k].low, results[k].median, results[k].high, rate, rate*(k == 4 ? 16 : 1));
  }
  int failed = ferror(f);
  if (fclose(f)) failed = 1;
  return failed ? -1 : 0;
}

/* Standalone vector plots generated directly from the measured medians. */
static int svg_report(const char *path, const result_s *r, uint64_t iterations) {
  FILE *f = fopen(path, "w");
  if (!f) { perror(path); return -1; }
  double max_ns = 0, max_rate = 0;
  for (unsigned k = 0; k < CASES; k++) {
    if (r[k].median > max_ns) max_ns = r[k].median;
    if (1000/r[k].median > max_rate) max_rate = 1000/r[k].median;
  }
  fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1000\" height=\"570\" viewBox=\"0 0 1000 570\" role=\"img\" aria-labelledby=\"title desc\">\n"
             "<title id=\"title\">zcrudp codec benchmark</title><desc id=\"desc\">Median amortized cost and throughput; one operation on the final row decodes sixteen records.</desc>\n"
             "<rect width=\"1000\" height=\"570\" fill=\"#f8fafc\"/><g font-family=\"monospace\" fill=\"#0f172a\">\n"
             "<text x=\"25\" y=\"32\" font-size=\"22\">zcrudp: in-memory codecs (not network latency)</text>\n"
             "<text x=\"25\" y=\"57\" font-size=\"13\">%d batches x %" PRIu64 " operations; median; warm cache; CLOCK_MONOTONIC</text>\n", SAMPLES, iterations);
  for (unsigned panel = 0; panel < 2; panel++) {
    unsigned top = 95 + panel*225;
    fprintf(f, "<text x=\"25\" y=\"%u\" font-size=\"16\">%s</text>\n", top,
            panel ? "Throughput: million operations/s (higher is better)" : "Amortized cost: ns/operation (lower is better)");
    for (unsigned k = 0; k < CASES; k++) {
      double value = panel ? 1000/r[k].median : r[k].median;
      double width = 500*value/(panel ? max_rate : max_ns);
      unsigned y = top + 18 + k*35;
      fprintf(f, "<text x=\"25\" y=\"%u\" font-size=\"13\">%s</text><rect x=\"300\" y=\"%u\" width=\"%.2f\" height=\"22\" fill=\"%s\"/><text x=\"%.2f\" y=\"%u\" font-size=\"13\">%.2f</text>\n",
              y+16, names[k], y, width, panel ? "#0f766e" : "#2563eb", 310+width, y+16, value);
    }
  }
  fprintf(f, "<text x=\"25\" y=\"550\" font-size=\"12\">One operation = one 8B frame/record, or one 132B datagram (16 records) on the last row.</text></g></svg>\n");
  int failed = ferror(f);
  if (fclose(f)) failed = 1;
  return failed ? -1 : 0;
}

int main(int argc, char **argv) {
  uint64_t iterations = 1000000;
  const char *csv = NULL, *svg = NULL;
  for (int i = 1; i < argc; i++) {
    const char *key = argv[i];
    if (!strcmp(key, "--help")) {
      puts("bench_rudp [--iterations 1000..100000000] [--csv PATH] [--svg PATH]"); return 0;
    }
    if (++i == argc) { fprintf(stderr, "Missing value\n"); return 2; }
    if (!strcmp(key, "--csv")) csv = argv[i];
    else if (!strcmp(key, "--svg")) svg = argv[i];
    else if (!strcmp(key, "--iterations")) {
      char *end; errno = 0;
      iterations = strtoull(argv[i], &end, 10);
      if (errno || *end || iterations < 1000 || iterations > 100000000 || argv[i][0] == '-') return 2;
    } else { fprintf(stderr, "Unknown option: %s\n", key); return 2; }
  }
  struct utsname host;
  if (uname(&host) == 0) printf("Host: %s %s %s\n", host.sysname, host.release, host.machine);
#ifdef __VERSION__
  printf("Compiler: %s\n", __VERSION__);
#endif
  printf("Timer: CLOCK_MONOTONIC; %d samples; %" PRIu64 " iterations/sample; 10000 warmup operations/case\n", SAMPLES, iterations);
  puts("Latency = amortized ns/op, including loop/checksum; throughput = Mops/s (Mpps for frame/record rows). No sockets or allocation in measured loops.");
  result_s results[CASES];
  for (unsigned k = 0; k < CASES; k++) {
    double samples[SAMPLES];
    (void)measure(k, 10000);
    for (unsigned s = 0; s < SAMPLES; s++) samples[s] = measure(k, iterations);
    qsort(samples, SAMPLES, sizeof samples[0], compare);
    results[k] = (result_s){samples[0], samples[SAMPLES/2], samples[SAMPLES-1]};
    if (results[k].median <= 0) { fprintf(stderr, "Insufficient timer resolution\n"); return 1; }
    printf("%-28s %9.3f ns/op [%.3f, %.3f] %9.3f Mops/s\n", names[k], results[k].median,
           results[k].low, results[k].high, 1000/results[k].median);
  }
  printf("Checksum: %" PRIu64 "\n", sink);
  if (csv && csv_report(csv, results, iterations)) return 1;
  if (svg && svg_report(svg, results, iterations)) return 1;
  return 0;
}
