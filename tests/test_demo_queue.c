/* Exercise the actual demo queue without requiring a socket or wall-clock waits. */
#define main demo_main
#define sendto capture_sendto
#include "../examples/demo_loss.c"
#undef main
#undef sendto
#include <assert.h>

static uint8_t observed[8];
static unsigned observed_count;
static int would_block;

ssize_t capture_sendto(int fd, const void *buf, size_t len, int flags,
                      const struct sockaddr *dest, socklen_t dest_len) {
  (void)fd; (void)flags; (void)dest; (void)dest_len;
  if (would_block) { errno = EAGAIN; return -1; }
  assert(observed_count < sizeof observed);
  observed[observed_count++] = *(const uint8_t *)buf;
  return (ssize_t)len;
}

int main(void) {
  struct sockaddr_in peer = {0};
  /* Slot 0 has been reused: physical index must not override the deadline. */
  queue[0] = (delayed_s){.bytes={3}, .len=1, .due=2, .seq=3, .used=1};
  queue[1] = (delayed_s){.bytes={2}, .len=1, .due=1, .seq=2, .used=1};
  queue[2] = (delayed_s){.bytes={1}, .len=1, .due=1, .seq=1, .used=1};
  queue[3] = (delayed_s){.bytes={4}, .len=1, .due=UINT64_MAX, .seq=4, .used=1};
  would_block = 1;
  assert(flush(-1, &peer) == 0);
  assert(observed_count == 0 && queue[2].used);
  would_block = 0;
  assert(flush(-1, &peer) == 0);
  assert(observed_count == 3);
  assert(observed[0] == 1 && observed[1] == 2 && observed[2] == 3);
  assert(!queue[0].used && !queue[1].used && !queue[2].used && queue[3].used);
  puts("PASS: deadline ordering, stable ties, future packet retained, EAGAIN preserves queue");
  return 0;
}
