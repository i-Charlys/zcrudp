# Modules and integration

zcrudp is split into linkable C11 modules. The existing `protocol_rudp.h` and
`protocol_profiles.h` remain source-compatible facades; new code can include a
module header and link only its target. There is no runtime plugin dispatcher,
background thread, heap allocation, or mandatory batching delay.

| CMake target | Public header | Sources | Link dependencies |
| --- | --- | --- | --- |
| `zcrudp::codec` | `rudp_codec.h` | `codec.c` | configuration only |
| `zcrudp::reliability` | `rudp_reliability.h` | `reliability.c` | configuration only |
| `zcrudp::session` | `rudp_session.h` | `session.c`, `session_tx.c`, `session_rx.c`, `rx.c` | reliability, codec |
| `zcrudp::io` | `rudp_io.h` | `io.c` | configuration only |
| `zcrudp::crypto` | `rudp_crypto.h` | `crypto.c` | configuration only |
| `zcrudp::stream` | `rudp_stream.h` | `stream.c` | session |
| `zcrudp::scalar` | `rudp_scalar.h` | `scalar.c` | configuration only |
| `zcrudp::motion` | `rudp_motion.h` | `motion.c` | scalar |
| `zcrudp::nextgen` | `rudp_nextgen.h` | `nextgen.c` | session, io |

`rudp_config.h` owns common configuration and error codes. `rudp_types.h` owns
shared protocol types and ABI assertions. `rx_internal.h` is private: the session
validates a complete datagram before calling its ordered RX implementation.
All source paths in the table are relative to `src/`.

## Selecting modules

```cmake
find_package(zcrudp CONFIG REQUIRED COMPONENTS session)
target_link_libraries(my_app PRIVATE zcrudp::session)
```

`zcrudp::core` is an interface target aggregating the enabled transport modules.
`zcrudp::profiles` aggregates the enabled profiles and core for compatibility.
These facades no longer produce a monolithic `libzcrudp` / `libzcrudp_profiles`
archive or shared library. Direct binary/FFI consumers must select the relevant
module libraries; CMake consumers keep the existing facade target names.
All ABI-relevant definitions propagate to installed consumers through
`zcrudp::config`. Mixing headers and libraries built with different window,
channel or RX-storage settings is unsupported.

By default all previous functionality is enabled. A codec/scalar-only build:

```sh
cmake -S . -B build/minimal \
  -DZCRUDP_BUILD_RELIABILITY=OFF -DZCRUDP_BUILD_SESSION=OFF \
  -DZCRUDP_BUILD_IO=OFF -DZCRUDP_BUILD_CRYPTO=OFF -DZCRUDP_BUILD_STREAM=OFF -DZCRUDP_BUILD_MOTION=OFF -DZCRUDP_BUILD_NEXTGEN=OFF
cmake --build build/minimal
```

`ZCRUDP_BUILD_PROFILES=OFF` disables all profiles. Each profile also has its own
`ZCRUDP_BUILD_STREAM`, `ZCRUDP_BUILD_SCALAR`, `ZCRUDP_BUILD_MOTION` or `ZCRUDP_BUILD_NEXTGEN` switch.
CMake rejects missing dependencies rather than silently enabling them. Host tools
are opt-in, require sessions, and currently use the default inline RX storage.

For direct compilation, use `codec.c` for codecs alone, `reliability.c` for a
standalone context, or add `session.c`, `session_tx.c`, `session_rx.c`, `rx.c` for
sessions. Add `io.c` only for the vectored-I/O utilities. The Makefile builds the
full default facade. Existing manual commands compiling only `src/rudp.c` or
`src/profiles.c` must be updated; those monolithic files have been replaced.

## Optional caller-owned reliable RX storage

The default layout and behavior remain unchanged: 5,364 bytes per four-channel
session on the checked ABI (1,340 bytes per channel), with embedded RX arrays. For channels that do not
need reliable reception, select the alternative ABI:

```sh
cmake -S . -B build/external-rx -DZCRUDP_EXTERNAL_RX_STORAGE=ON
```

```c
#include "rudp_session.h"

static rudp_session_s session;
static rudp_rx_storage_s reliable_rx;

/* Check every return code in the integration. */
int setup(void) {
  int result = rudp_session_init(&session);
  if (result != RUDP_OK) return result;
  return rudp_session_attach_rx(&session, 0, &reliable_rx);
}
```

In this mode channels initially have no RX storage. Attach a distinct storage
object to every channel that will receive reliable data, including channels used
by a stream receiver. Unreliable data and ACKs need no RX storage. Reliable input
addressed to an unattached channel rejects the whole datagram before ACK or RX
mutation. This is a local storage choice, not wire negotiation.

Storage must outlive attachment and must not be shared or modified by other
execution contexts. Attach/detach requires empty TX and retained RX windows;
`NULL` detaches. Explicit reset clears retained RX and preserves attachments.
To abandon data and release storage, reset first, then detach. Calling init again
starts a new lifetime with no attachments.

At the checked 64-bit ABI, the external-storage session is 4,392 bytes, plus
264 bytes per attached RX object. One receiving channel costs 4,656 bytes in
aggregate versus 5,364 by default. With all four RX objects it costs 5,448 bytes:
external storage is useful for sparse reliable reception, not universally smaller.
Actual sizes depend on ABI and settings. TX slots and adaptive recovery metadata
remain embedded; independently sized caller-owned TX pools remain roadmap work.

## Ownership and unchanged protocol guarantees

One execution context owns a session and all its attached mutable storage. Feed
RX, timers, sends, reset and future completion events through that owner.
Independent sessions can be owned by different workers; the library introduces
no synchronization. Cross-thread queues remain integration work.

The wire bytes, sequence rules, cumulative ACK semantics, RX backpressure,
retransmission policy and retry limits are preserved. The four-byte TFV stays
inline. The existing builder still changes egress state while building; a
transactional prepare/submit/commit API and borrowed large TX buffers remain
explicit future tasks, not capabilities of this refactor.

Validation commands:

```sh
make test asan test-tools test-cmake test-compare
make test-modules test-external-rx
```

The module suite verifies independent installed consumers in static/shared
builds, a minimal build with only codec/scalar libraries, and propagation of the
external-RX ABI. The RX suite covers missing-storage atomicity, pointer ownership,
retained gaps, backpressure, reset and sequence wrap, including ASan/UBSan.
Simulated p99 and delivery comparisons are not physical-network latency guarantees.
