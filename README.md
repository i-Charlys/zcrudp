# zcrudp

**Small packets. Fixed memory. Your event loop.**

Reliable and unreliable UDP messaging for tiny, frequent updates — in one C11
source file and two headers. You own the buffers, the clock and the socket.

| 0 heap allocation calls in the core | 0 external core dependencies | 12-byte single-update datagram |
| :---: | :---: | :---: |
| Caller-owned, bounded storage | Compile directly into your project | Includes a 4-byte TFV payload and a channel-specific ACK |

Built for game inputs, compact state updates and telemetry where memory ownership
and small messages are part of the design. Reliable events use bounded TX windows;
fresh unreliable updates bypass those windows and never wait for a missing sequence.

**Try it in two terminals:**

```bash
make demo
./build/demo_loss server --loss 20 --latency 40
# In another terminal:
./build/demo_loss client --loss 20 --latency 40
```

Type `r 1234` for a reliable event, `u 5678` for a fresh update, or `stats` to see
loss and recovery. [Demo options](#interactive-loss-and-latency-demo) ·
[Measure it yourself](#codec-benchmarks) · [Compare the designs](docs/COMPARISON.md)

## A small core with a measurable budget

- **1,040 bytes per context; 4,212 bytes per four-channel session** on the measured
  host ABI with default settings. Run `make test` to inspect sizes on your target.
  Each default TX ring has 64 slots and holds 63 outstanding reliable messages.
- **4-byte ACK; 12-byte single-update datagram; 132 bytes for 16 data records.**
  These are UDP payload sizes, excluding UDP/IP and link-layer overhead.
- **No heap calls, background threads or socket API in the core.** Integrate
  `src/rudp.c` and the two headers; call the protocol from your own loop.
- **Evidence you can rerun:** strict C11 tests, loss/jitter demo, codec timing,
  CSV output and generated graphs. [Raw results](docs/bench/codec.csv).

The current codec baseline and its measurement conditions are shown below.
These are in-memory operations, not network delivery rates or competitor speedups.

![Codec cost and throughput](docs/bench/codec.svg)

## Key Features

- **Zero-malloc**: All memory is managed through static or stack-allocated contexts.
- **Explicit wire format**: Unified datagrams use a 4-byte envelope and 8-byte records (channel, flags, sequence, TFV). The legacy single-context frame is 8 bytes total.
- **Cumulative ACKs**: Implements a sliding window (default 64 slots) with cumulative acknowledgment logic.
- **Retransmission**: Built-in timeout handling and retransmission tracking.
- **TFV Integration**: Uses a 32-bit Type-Flags-Value (TFV) structure for the payload.
- **Sequence Rollover**: Robust handling of 16-bit sequence number wraparound.

## Project Structure

```text
.
├── include/
│   ├── protocol_rudp.h    # Core RUDP definitions and context
│   └── protocol_tfv.h     # 32-bit TFV packet structure
├── src/
│   └── rudp.c             # RUDP implementation logic
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
settings, so injected round-trip delay is the sum of the two directions. The
5 ms event-loop polling interval adds scheduling granularity. A seed fixes the
PRNG stream; OS scheduling and retransmissions can still change the full trace.

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

For a single four-byte message, the unified zcrudp datagram occupies **12 bytes
of UDP payload**, compared with **28 bytes for a KCP PUSH segment**: 57% fewer
bytes at this layer in that specific case. ENet's reliable encoding is already
close, at 12–14 bytes under the stated assumptions. These are calculations from
wire definitions, not throughput measurements. See the
[sourced layouts and assumptions](docs/COMPARISON.md#small-message-wire-comparison).

The strongest reason to choose zcrudp is its combination of caller-owned fixed
storage, small records and separate reliable/unreliable paths. The current tests
verify that automatic unreliable updates continue when the reliable TX window
fills, that the simulation preserves deadline order, and that sequence gaps
trigger ACKs without wrapping the duplicate-ACK counter.

For general large messages, built-in security or broader networking features,
evaluate ENet, KCP, GNS or QUIC against those needs. This repository has no matched
competitor timing or peak-memory measurements yet. It also does not demonstrate
bulk KV-cache transfer or hardware-validated MCU integration. The
[comparison guide](docs/COMPARISON.md) details the scope and the experiments needed
to substantiate future speedup claims.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
