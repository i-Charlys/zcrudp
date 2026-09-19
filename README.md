# zcrudp

A caller-owned, fixed-storage UDP message library in pure C.
zcrudp is a C11 library for reliable and unreliable UDP messaging, intended for
small game updates and embedded telemetry. It uses caller-owned buffers, with no
heap allocation or external dependencies. The application provides the clock,
socket and event loop.

## Why zcrudp?

An ordered reliable channel waits for a missing message before delivering later
messages on that channel. Separate channels let an unreliable update continue
while a reliable channel waits for retransmission.

The library provides:
- **Unreliable updates:** They bypass the reliable TX window and are serialized
  into a caller-provided UDP buffer. Older sequence numbers are discarded.
- **Reliable messages:** Each channel has its own TX window and bounded RX buffer.
  Ordered delivery is attempted until the retry limit is reached; applications
  must handle disconnection and coordinate resets with their peers.
- **Caller-owned storage:** The core does not call `malloc`. A default four-channel
  session occupies 5,620 bytes on the tested ABI, including storage reserved for
  channels configured as unreliable.

[Interactive web replay](docs/visual/index.html) ·
[Transport benchmarks](#transport-benchmarks-vs-enet-enet-zpl-and-kcp) ·
[Codec benchmarks](#codec-benchmarks) ·
[Interactive CLI demo](#interactive-loss-and-latency-demo)

## Key Features
**Try it in two terminals:**

```bash
make demo
./build/demo_loss server --loss 20 --latency 40
# In another terminal:
./build/demo_loss client --loss 20 --latency 40
```

Type `r 1234` for a reliable event, `u 5678` for a fresh update, or `stats` to see
loss and recovery. [Demo options](#interactive-loss-and-latency-demo) ·
[Measure it yourself](#codec-benchmarks) · [Compare throughput and latency](#transport-benchmarks-vs-enet-enet-zpl-and-kcp)

## Memory and wire format

There are two wire formats. The original single-context format is a 4-byte ACK
or an 8-byte frame (`rudp_header_s` + one 4-byte TFV). The session format is a
4-byte datagram header followed by zero or more 8-byte records. A record carries
a channel ID, flags, sequence number and TFV; a session is the local in-memory
state for all channels, not a network record. These formats require an explicit
choice by the application; do not infer one from UDP length alone.

`tx_buffer` stores reliable messages awaiting ACKs. A session channel's
`rx_buffer` stores reliable messages received ahead of a missing sequence until
they can be delivered in order. Unreliable records bypass both windows.

- **1,040 bytes per context; 5,620 bytes per four-channel session** on the measured
  host ABI with default settings. Run `make test` to inspect sizes on your target.
  Each default TX ring has 64 slots and holds 63 outstanding reliable messages.
  Sessions now retain out-of-order reliable payloads in a fixed RX window.
- **4-byte ACK; 12-byte single-update datagram; 132 bytes for 16 data records.**
  These are UDP payload sizes, excluding UDP/IP and link-layer overhead.
- **No heap calls, background threads or socket API in the core.** Integrate
  `src/rudp.c` and the two headers; call the protocol from your own loop.
- **Optional profiles:** bounded multipart messages, compact4/rolling8 scalar
  telemetry and adaptive duplication. Add `src/profiles.c` when needed. Session
  priorities and recovery state remain in the core and its fixed session size.
  [Wire formats and tradeoffs](docs/PHASE4.md).

## Transport benchmarks vs ENet, ENet (zpl), and KCP

The comparative runner executes zcrudp, ENet (lsalzman), ENet-zpl (zpl-c), and KCP (default and fast profiles):
2,400 reliable ordered four-byte messages, nine scenarios, five seeds — **225 recorded runs**.

![Delivered throughput comparison](docs/bench/comparison/throughput.svg)

![Tail latency by scenario, focused on zcrudp and ENet variants](docs/bench/comparison/latency-p99-detail.svg)

The focused p99 chart uses a separate linear scale in each scenario so large KCP
delays do not compress the differences among zcrudp and the ENet variants.
KCP-fast values are printed in each panel; the [full five-engine chart](docs/bench/comparison/latency-p99.svg)
and [CSV](docs/bench/comparison/results.csv) preserve the complete comparison.

![Datagram wire cost comparison](docs/bench/comparison/wire-cost.svg)

The [240 Hz wire-cost chart](docs/bench/comparison/wire-cost-paced.svg) also
shows the clean 250 ms ping case where zcrudp sends more UDP payload bytes than ENet.

![Initial timeout sensitivity at 250 ms ping](docs/bench/comparison/rto-sensitivity.svg)

This follow-up measures zcrudp with 100 ms and 300 ms initial retransmission
timeouts on the same virtual 250 ms ping path. At 300 ms, clean traffic drops
from 32.0 to 16.0 UDP payload bytes per delivered message with unchanged p99
latency. Under 2% loss, its p99 rises from 379 to 1,429 ms. The 100 ms profile
remains the published comparison; [the sensitivity data](docs/bench/comparison/rto-sensitivity.csv)
shows the tradeoff rather than replacing it.

These are **virtual-link transport measurements**: common delay/loss/jitter and
1 ms service cadence, not physical-NIC benchmarks. The no-loss saturation cases
show zcrudp matching ENet and ENet-zpl goodput at the common 63-message admission limit.
In high-density saturation bursts, zcrudp aggregates records tightly to emit fewer
UDP-payload bytes (8.1 B vs 18.2 B per message). In the clean 250 ms ping case,
the benchmark's 100 ms initial retransmission timeout expires before an ACK can
return. This causes unnecessary retransmissions: zcrudp emits 32.0 B/message
versus 24.1 B/message for ENet. The 100 ms setting is part of the benchmark
profile, not a suitable initial timeout for every network path.

With adaptive recovery enabled, the median of the five per-run p99 values shows:
- **5% loss (10 ms delay, 5 ms jitter)**: 57 ms for zcrudp, compared with 167 ms for ENet, 378 ms for ENet-zpl, and 69 ms for KCP-fast.
- **High jitter (20 ms delay, 2% loss, 40 ms jitter)**: 189 ms for zcrudp, compared with 338 ms for ENet and 203 ms for ENet-zpl, with 22% less datagram wire traffic than ENet.
- **KCP-fast under high jitter**: 159 ms p99, ahead of zcrudp's 189 ms in that scenario.
- **High latency with loss (250 ms ping / 125 ms one-way, 2% loss, 10 ms jitter)**: 379 ms for zcrudp, compared with 803 ms for ENet (-53%) and 673 ms for ENet-zpl (-44%). In the clean 250 ms ping case, zcrudp, ENet, and ENet-zpl all deliver at the physical 125 ms one-way baseline.

It costs 352 B of session state and delivers significantly lower tail latency under packet loss.
These results depend on the workload and transport settings, including KCP's
selected profile. See [recovery settings and tradeoffs](docs/ADAPTIVE_RECOVERY.md).

[p50 latency](docs/bench/comparison/latency-p50.svg) ·
[Host execution cost](docs/bench/comparison/host-cost.svg) ·
[Raw CSV](docs/bench/comparison/results.csv) ·
[Methodology and exact settings](docs/bench/comparison/README.md)

```bash
make compare COMPARE_PYTHON='uv run --with matplotlib python'
make test-compare  # Small deterministic comparisons; no plotting package needed
```

Competitor sources are pinned and checksummed, downloaded only for this optional
target. The core still has no external dependency. Already have Matplotlib?
Run `make compare` directly with Python 3.12+.

## CMake integration

The Makefile remains available. CMake 3.21+ can build static (default) or shared
libraries, with no downloaded dependencies:

```bash
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release -DZCRUDP_BUILD_TOOLS=ON
cmake --build build/cmake --parallel
ctest --test-dir build/cmake --output-on-failure
# Optional: cmake --install build/cmake --prefix /your/install/prefix
```

Embed with `add_subdirectory(path/to/zcrudp)` and link `zcrudp::core` or
`zcrudp::profiles`. Installed packages support `find_package(zcrudp CONFIG REQUIRED)`
with the same target names. Profiles link the core transitively. Tests default
to off when embedded; tools are always opt-in and currently require POSIX.

Options: `BUILD_SHARED_LIBS`, `ZCRUDP_BUILD_PROFILES`, `ZCRUDP_BUILD_TESTS`,
`ZCRUDP_BUILD_TOOLS`, `ZCRUDP_WARNINGS_AS_ERRORS`. ABI settings
`ZCRUDP_WINDOW_SIZE` (default 64) and `ZCRUDP_MAX_CHANNELS` (default 4) propagate
to consumers through targets; do not override their corresponding C macros
independently. Non-default ABIs run the generic window tests, not the existing
default-specific suite. Assertions remain enabled in Release tests.

`make test-cmake` verifies installation/relocation, embedding and configuration.
Linux static/shared builds are tested; this does not certify every compiler or
game engine. Packaging follows CMake's
[import/export model](https://cmake.org/cmake/help/latest/guide/importing-exporting/index.html).

## Protocol features

- **Zero-malloc**: All memory is managed through static or stack-allocated contexts.
- **Fixed-frame RUDP**: Optimized for 32-bit architectures with a total frame size of 8 bytes (4-byte header + 4-byte payload).
- **Explicit wire format**: Unified datagrams use a 4-byte envelope and 8-byte records (channel, flags, sequence, TFV). The legacy single-context frame is 8 bytes total.
- **Cumulative ACKs**: Implements a sliding window (default 64 slots) with cumulative acknowledgment logic.
- **Retransmission**: Built-in timeout handling and retransmission tracking.
- **TFV Integration**: Uses a 32-bit Type-Flags-Value (TFV) structure for the payload.
- **Sequence Rollover**: Robust handling of 16-bit sequence number wraparound.

## Project Structure

```text
.
├── include/
│   ├── protocol_rudp.h    # Core context, session and wire definitions
│   ├── protocol_tfv.h     # 32-bit TFV packet structure
│   └── protocol_profiles.h # Optional stream and scalar profiles
├── src/
│   ├── rudp.c             # Core transport logic
│   └── profiles.c         # Optional profile logic
├── examples/demo_loss.c  # Interactive POSIX UDP peers with simulated loss/delay
├── bench/bench_rudp.c    # In-memory codec benchmarks and CSV/SVG reports
├── docs/bench/           # Recorded benchmark results
└── tests/
    ├── test_rudp.c        # RUDP engine and extreme case tests
    └── test_tfv.c         # TFV structure validation
```

## Quick Start / Usage

### Basic Initialization and Sending

```c
#include "protocol_rudp.h"

rudp_context_s ctx;
rudp_init(&ctx);

tfv_packet_u packet;
packet.type = 1;
packet.flags = 0;
packet.value = 123;

// Send a packet at current timestamp (1000ms)
rudp_send(&ctx, packet, 1000);
```

### Handling ACKs and Retransmissions

```c
// Receive an ACK under N+1 convention: ACK=1 acknowledges packet 0
rudp_recv_ack(&ctx, 1);

// Check for timed-out packets (100ms timeout)
uint16_t expired_slots[64];
rudp_tick_result_s res = rudp_tick(&ctx, 1200, 100, expired_slots, 64);

if (res.status == RUDP_OK && res.count > 0) {
    for (int i = 0; i < res.count; i++) {
        const rudp_frame_s *frame = rudp_get_slot_frame(&ctx, expired_slots[i]);
        // Resend frame via sendto()
    }
}
```

## Building & Testing

The project includes a `Makefile` for compilation, strict C11 compliance, and sanitizers.

### Run all tests
```bash
make test
```

The Makefile uses the system C compiler (`cc`) by default; Zig is not required.
You can choose another compiler explicitly, for example `make CC=clang test` or
`make CC='zig cc' test` if Zig is installed. GitHub CI runs the test suite with
both Zig and GCC.

### Run Memory & Undefined Behavior Sanitizers (ASan & UBSan)
```bash
make asan
```

### Build individual tests
```bash
make test_rudp
make test_tfv
make test_window
```

### Manual compilation (alternative)
If you don't have `make`, you can compile manually (ensure the `build/` directory exists):
```bash
mkdir -p build
gcc -Iinclude -Wall -Wextra -O2 src/rudp.c tests/test_rudp.c -o build/test_rudp
./build/test_rudp
```

## Interactive loss and latency demo

Build with `make demo`. This host example uses POSIX IPv4 UDP sockets and
`CLOCK_MONOTONIC` (Linux/macOS); the core library still has no OS dependency.
Open two terminals and start the server first:

```bash
# Terminal 1: listens on 127.0.0.1:9000, sends to port 9001
./build/demo_loss server --loss 20 --latency 40 --jitter 20 --seed 1

# Terminal 2: listens on 127.0.0.1:9001, sends to port 9000
./build/demo_loss client --loss 20 --latency 40 --jitter 20 --seed 2
```

Either terminal accepts these commands:

```text
r 1234    # Send a reliable, ordered value on channel 0
u 5678    # Send an unreliable, sequenced value on channel 1
stats     # Show drops, retries, deliveries and outstanding reliable messages
q         # Exit (Ctrl-C also works)
```

Enter just the command and value, without the explanatory comment. Values are
unsigned 16-bit integers. Reliable retransmissions should eventually deliver
each value once, in order, provided the retry limit is not reached. Unreliable
values can be lost; duplicates and older sequences are discarded.

For an automatic run, keep the server above running and start:

```bash
./build/demo_loss client --count 20 --interval 100 --duration 8000 \
  --loss 20 --latency 40 --jitter 20 --seed 2
```

This sends 20 values **on each channel**. `--duration` is a hard deadline in ms;
allow time for retries after generation finishes. Exit statistics include reliable
messages still pending and delayed datagrams abandoned on exit. EOF on stdin
leaves the network loop running, allowing redirection from `/dev/null`.

Loss is an independent integer percentage applied to every outgoing datagram,
including ACKs and retransmissions. Each surviving datagram waits `--latency`
plus a uniform integer delay from zero through `--jitter` ms in a fixed 512-slot
queue. Queue saturation is reported separately. Both peers apply their own
settings, so injected round-trip delay is the sum of the two directions. While a
burst is outstanding (automatic generation still running, reliable slots unacked,
or datagrams waiting in the delay queue) the event loop polls without blocking, so
burst latency is not quantised to the OS scheduler tick; it falls back to a 5 ms
wait only when the loop is idle. A seed fixes the PRNG stream; OS scheduling and
retransmissions can still change the full trace. On Linux the intra-tick burst is
flushed with a single `sendmmsg()` call; other POSIX hosts loop on `sendto()`.

`--timeout` defaults to 500 ms and starts when the reliable message is queued,
before injected delay. Set it above the expected round-trip delay to avoid
premature retries. Retries use `rudp_tick()` and the library retry limit; exceeding
that limit exits with a nonzero status. The example serializes only new or
expired reliable slots, so ACK emission does not retransmit the whole window.

Use `--bind`, `--peer`, `--port` and `--peer-port` for two machines or alternative
ports. Addresses must be numeric IPv4. The peer address is configured explicitly;
there is no discovery, handshake, encryption or peer authentication in this demo.
Run `./build/demo_loss --help` for all options.

## Codec benchmarks

```bash
make bench                              # Run seven batches per codec
make bench BENCH_ITERATIONS=10000000    # Longer batches
make bench-report                       # Regenerate docs/bench CSV, SVG and log
```

The executable also supports independent output paths:

```bash
./build/bench_rudp --iterations 1000000 --csv build/codec.csv --svg build/codec.svg
```

The suite measures legacy 8-byte frame encoding/decoding, unified 8-byte record
encoding/decoding, and decoding a complete 132-byte datagram containing 16 records.
Each case warms up for 10,000 operations, then reports the median, minimum and
maximum batch-average cost across seven samples. Inputs vary; outputs contribute
to an observable checksum. The benchmark builds without LTO to keep codec calls
in the timed loop. No socket calls or explicit allocations occur in that loop.

**ns/op is amortized CPU-side elapsed time, not individual-call tail latency or
network latency.** It includes loop/checksum overhead. Mops/s equals `1000 / ns/op`;
for the single-frame/record cases this is Mpps of codec processing, not achievable
UDP packet rate. The final row counts whole datagrams per operation; its CSV also
reports the corresponding record throughput. Encoding and decoding are measured
separately, with hot data, and not as an end-to-end transport workload.

![Codec cost and throughput](docs/bench/codec.svg)

The checked-in example was measured on an Intel Core Ultra 9 288V, x86-64 WSL2,
with GCC 13.3.0, `-O2` and `-fno-lto`, using 1,000,000 operations per batch.
See the [raw CSV](docs/bench/codec.csv) and [environment/run log](docs/bench/environment.txt).
These host-specific observations are not performance guarantees for STM32 or
other targets. CPU frequency, background load, compiler and virtualization affect
the results. Regenerate on the target host before comparing changes; use the same
build flags and batch size. Short runs are useful for smoke tests only.

### Host-tool integration tests

```bash
make test-tools
```

Requires Python 3 (standard library only) and permission to open loopback UDP
sockets. Tests cover CLI validation, two real peers, interactive commands,
reliable recovery under simulated loss/jitter, unreliable ordering, total loss,
and CSV/SVG report validity. This target does not enforce timing-based performance
thresholds and is separate from the portable core tests.

## How it compares

zcrudp specializes in small TFV updates with caller-owned, fixed storage. It
provides independent channel state, but an ordered reliable channel can still
wait for a missing message. The core provides no handshake, authentication,
encryption, congestion control or socket management. QUIC targets a broader,
secure transport; it is not an equivalent replacement for this small-message
protocol, nor is this library a general QUIC replacement.
For a single four-byte message, the unified zcrudp datagram occupies **12 bytes
of UDP payload**, compared with **28 bytes for a KCP PUSH segment**: 57% fewer
bytes at this layer in that specific case. ENet's reliable encoding is already
close, at 12–14 bytes under the stated assumptions. These are calculations from
wire definitions, not throughput measurements. See the
[sourced layouts and assumptions](docs/COMPARISON.md#small-message-wire-comparison).

For complete technical benchmarks, packet layouts, and architectural analysis, see [docs/COMPARISON.md](docs/COMPARISON.md).
The strongest reason to choose zcrudp is its combination of caller-owned fixed
storage, small records and separate reliable/unreliable paths. The current tests
verify that automatic unreliable updates continue when the reliable TX window
fills, that the simulation preserves deadline order, and that sequence gaps
trigger ACKs without wrapping the duplicate-ACK counter.

For general large messages, built-in security or broader networking features,
evaluate ENet, KCP, GNS or QUIC against those needs. The repository includes
matched virtual-link runs for ENet and KCP. Peak-memory comparisons, physical-network
tests, bulk KV-cache transfer and hardware-validated MCU integration remain open.
The [comparison guide](docs/COMPARISON.md) separates measured transport behavior
from wire-layout calculations and architectural tradeoffs.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
