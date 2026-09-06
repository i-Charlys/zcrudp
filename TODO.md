# Roadmap

Completed work and remaining tasks. Phase numbers are kept for links from the
implementation notes; tooling can progress independently of protocol changes.

---

## Phase 1: Memory layout
*Prerequisite for all subsequent network and serialization work.*

- [x] **Documentation sync**: Update the ASCII diagram in `src/rudp.c` to match `rudp_header_s`.
- [x] **TFV 8/8/16 Layout Refactoring**: Refactor `tfv_packet_u` from 8/4/20 to byte-aligned `uint8_t type` (8b), `uint8_t flags` (8b), `uint16_t value` (16b) in `include/protocol_tfv.h` (eliminates bitfield ambiguity and standardizes 16-bit payload resolution).
- [x] **Window size compile check**: Add `#if (RUDP_WINDOW_SIZE & (RUDP_WINDOW_SIZE - 1)) != 0` check in `include/protocol_rudp.h` ensuring window size is strictly a power of 2.
- [x] **Initialization order**: Fix variable initialization order relative to `memset` in `rudp_init` (`src/rudp.c`).
- [x] **Unit assertions update**: Replace `printf` in `tests/test_tfv.c` with automated `assert()` checks for the new 8/8/16 layout.
- [x] **TFV test coverage**: Assert representative values up to `UINT8_MAX` and `UINT16_MAX`, and verify the complete 4-byte packet layout.

---

## Phase 2: Serialization
*Depends on Phase 1 structures.*

- [x] **Endianness & Byte Order**: Implemented zero-dependency `rudp_htons`/`rudp_ntohs` (16-bit) and `rudp_htonl`/`rudp_ntohl` (32-bit) with compiler builtins and Big-Endian Network Byte Order support.
- [x] **Wire Pack / Unpack Functions**:
  - `rudp_pack_frame(const rudp_frame_s *frame, uint8_t *out_buf, size_t max_len)`
  - `rudp_unpack_frame(const uint8_t *in_buf, size_t in_len, rudp_frame_s *out_frame)`
- [x] **TFV endianness & wire tests**: Added automated roundtrip tests, wire byte verification, security bounds, and NULL checks in `tests/test_rudp.c` and `tests/test_tfv.c`.

---

## Phase 3: Reliability
*Depends on Phase 1 & 2.*

- [x] **ACK N+1 Convention & In-Window Check**:
  - Adopt `ACK = expected_seq` ($N+1$) convention so `(int16_t)(ack - seq) > 0` safely validates in-flight packets and prevents $t=0$ race conditions without using flag bits.
  - Reject corrupted or out-of-window ACKs in `rudp_recv_ack`.
- [x] **Reliability edge-case tests**: Cover timeout boundaries, repeated `rudp_tick()` calls, empty-window ACKs, stale ACKs, and ACK rollover around `65535 -> 0`.
- [x] **Window-size tests**: Verify `RUDP_WINDOW_SIZE=0`, `1`, non-powers of two, and a valid small window at compile time and runtime (`tests/test_window.c`).
- [x] **Reception Engine (RX)**:
  - Track `expected_seq` on the receiver side.
  - Detect and discard duplicate packets while re-emitting ACKs to calm the sender.
- [x] **Full-Duplex Context**: Combine TX sliding window and RX state within `rudp_context_s` to enable automatic ACK piggybacking on outgoing data frames.
- [x] **Fast Retransmit (Tri-ACK)**: If 3 duplicate ACKs arrive for the same sequence without tail moving, immediately retransmit the slot at `tail` without waiting for the timeout timer to expire.
- [x] **Retransmission Limit & Dead Peer**: Added per-slot retry counter (`slot->retries > RUDP_MAX_RETRIES`) and connection state machine (`RUDP_STATE_CONNECTED` / `RUDP_STATE_DISCONNECTED`) in `rudp_tick()`.

---

## Phase 3.5: Fixes from issue #3
*Comprehensive remediation of all 3 confirmed runtime defects, portability issues, API ergonomics, and doc/test drift before Phase 4.*

- [x] **1. Dead Connection & Zombie Overflow Protection**:
  - Make `rudp_tick()` inert / no-op once `ctx->state == RUDP_STATE_DISCONNECTED` (prevents `retries` uint8 overflow at 256 from resurrecting dead connections as "zombies").
  - Reject `rudp_send()` immediately when `ctx->state == RUDP_STATE_DISCONNECTED`.
  - Expose `int rudp_reset(rudp_context_s *ctx)` as the clean, explicit API to reset and reconnect a dead context.
- [x] **2. Preserve Retransmission List on Dead Peer Trigger**:
  - Implement `rudp_tick_result_s` returning both `{count, status}` so collected in-flight expired slots are preserved when dead peer is tripped.
  - Expose `rudp_get_unacked_slots()` allowing game engines to inspect and rollback all unacknowledged packets upon disconnect.
- [x] **3. Fast Retransmit (Tri-ACK) Hardening & Stale-ACK Defense**:
  - Fix startup off-by-one: Initialize `last_ack_received = 0xFFFF` (sentinel) so valid initial `ACK=0` is not counted as a duplicate.
  - Fix duplicate storm: Trigger Fast Retransmit strictly on `duplicate_ack_count == 3` (single trigger).
  - Use an explicit flag (`slot->fast_retransmit`) to guarantee immediate retransmission even near clock origin (`now <= timeout`).
  - Add window-floor check in `rudp_recv_ack()` (`(int16_t)(ack_num - tail_seq) < 0`) so reordered/stale ACKs cannot reset `duplicate_ack_count` or disarm Tri-ACK.
- [x] **4. Standalone ACK (Tier 1 - 4B) & Dynamic Retransmit Refresh**:
  - Implement `rudp_pack_ack(uint16_t ack_num, uint8_t *out_buf, size_t max_len)` and `rudp_unpack_ack(const uint8_t *in_buf, size_t in_len, uint16_t *out_ack)`.
  - Add modular helper `rudp_unpack_header(const uint8_t *in_buf, size_t in_len, rudp_header_s *out_header)` to share Big-Endian decoding logic with `rudp_unpack_frame`.
  - Fix unidirectional traffic: Allow receivers to send pure ACK control frames without requiring dummy application payload.
  - In `rudp_tick()`, dynamically refresh `slot->frame.header.ack = ctx->expected_seq_num` on every retransmission.
- [x] **5. Portability & C++ Engine Linkage**:
  - Add `extern "C"` guards in `include/protocol_rudp.h` and `include/protocol_tfv.h` for clean linkage with C++ game engines (Unreal, Godot, Raylib).
  - Pin standard: Add `-std=c11 -pedantic -Werror` to `Makefile` CFLAGS (supporting C11 anonymous structs in `tfv_packet_u`).
  - Standardize include paths: Replace `#include "../include/..."` with `#include "protocol_rudp.h"`.
  - Prefix or clean up `IS_LITTLE_ENDIAN` macro to prevent public namespace pollution.
  - Clean up dead header scaffolding: remove unused `RUDP_PACKED`, `RUDP_WIRE_DYNAMIC_SIZE -1`, and unused endian helpers.
- [x] **6. API Ergonomics & Context Encapsulation**:
  - Add accessor `const rudp_frame_s *rudp_get_slot_frame(const rudp_context_s *ctx, uint16_t slot_idx)` to allow reading expired frames without piercing context internals.
  - Symmetrize API return values (`rudp_pack_frame` vs `rudp_unpack_frame`).
- [x] **7. Test Suite Hardening & CI**:
  - Add `-UNDEBUG` to CFLAGS ensuring test assertions are never disabled under `-DNDEBUG`.
  - Fix `int main()` to `int main(void)` in `tests/test_tfv.c` and standardize `#include "protocol_rudp.h"`.
  - Add `make asan` target with AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan).
  - Add GitHub Actions CI workflow (`.github/workflows/ci.yml`) running standard and ASan test suites.
- [x] **8. Documentation, Style & Security Policy**:
  - Fix `README.md` Quick Start example to use $N+1$ convention (`ACK=1` acknowledges seq 0), `rudp_tick_result_s`, and `make asan`.
  - Re-sync `ARCHITECTURE.md` with 1036-byte context layout, 16-byte slot diagram, modular serializers, and dead peer state machine.
  - Add `.clang-format` configuration for consistent code styling across C and C++ integrations.
  - Add `SECURITY.md` stating cleartext transport notice and recommending DTLS for untrusted networks.

---

## Phase 4: Receive buffering and channel profiles
*Depends on Phase 3 working bidirectional engine.*

- [x] **RX Out-of-Order Reassembly Buffer**: Bounded payload slots + bitmap, wrap-safe ordered drain and explicit application backpressure via `rudp_session_poll()`. Selective storage with cumulative ACKs, not selective ACK signaling; see `docs/PHASE4.md`.
- [x] **Reliable paced-loss completion regression**: All five seeds of both 2,400-message `240hz-loss1` and `240hz-loss5` scenarios complete with unchanged workload, timeout, backoff and retry limit. Regression in `tests/test_comparison.py`; baseline preserved in `docs/bench/before-phase4/`.
- [x] **3-Tier Multi-Resolution Packet Support**:
  - [x] **Tier 1 (4 bytes)**: Handle short header-only packets (`seq_num` + `ack`) for pure ACKs, heartbeats/pings, and connection signals (`rudp_pack_ack()`, `rudp_unpack_ack()`).
  - [x] **Tier 2 (8 bytes)**: Standard game frames (Header 4B + TFV 4B) with atomic modular encoders/decoders.
  - [x] **Tier 3 (Multi-part Streaming)**: Optional caller-owned stream profiles, descriptor + raw four-byte chunks, up to 65,535 bytes per message on a dedicated reliable+ordered channel. Explicit endian-safe serialization, backpressure, bounded oversize discard and loss/reordering tests.
- [x] **Multi-Channel Architecture & User-Configurable Profiles**:
  - [x] Define bitwise capability flags (`RUDP_CHANNEL_FLAG_RELIABLE`, `RUDP_CHANNEL_FLAG_ORDERED`, `RUDP_CHANNEL_FLAG_ENCRYPTED`).
  - [x] Implement multi-channel session structures (`rudp_channel_s`, `rudp_session_s`) with zero dynamic allocation (`RUDP_MAX_CHANNELS = 4`, currently 5,620 B including RX and adaptive recovery state).
  - [x] Implement session initialization and channel configuration API (`rudp_session_init`, `rudp_session_config_channel`).
  - [x] Channel egress scheduler with strict priority before encoding, rotating equal-priority ties, ACK precedence, and per-channel DSCP hints. POSIX demo `--dscp` applies optional socket-wide IP TOS; mixed bundles have one traffic class. See `docs/PHASE4.md` for starvation and integration limits.
  - [x] **Unified Datagram & Intra-Tick Bundling Architecture**:
    - [x] **Step 1: Wire Format Specifications**: 4-byte datagram header (`ack`, `ack_channel`, `count`) and 8-byte message records (`channel_id`, `flags`, `seq_num`, `payload`).
    - [x] **Step 2: Strict Wire Serialization & Bounded Validation**: Endian-safe bitshifts and strict bounded length check (`in_len == 4 + count * 8`).
    - [x] **Step 3: Multi-Channel Session Routing**: Piggybacked ACK dispatch to target channel and message record distribution across independent channel contexts.
    - [x] **Step 4: Fast Memory Bypass & 16-bit Anti-Rollback**: Zero-malloc egress bypassing `tx_buffer`, and RFC 1982 circular sequence filter (`distance != 0 && distance < 0x8000U`) rejecting older or duplicate frames.
    - [x] **Step 5: Protocol Hardening & Intra-Tick Bundler**:
      - `rudp_session_send_reliable()` queues reliable slots into channel `tx_buffer`.
      - `rudp_session_build_datagram()` aggregates primary piggybacked ACK, multi-channel pending ACKs (`RUDP_RECORD_FLAG_ACK`), and in-flight reliable slots into a unified MTU packet.
      - Fixed Bug 1 (TCP RFC 5681): Passive piggybacked ACKs on datagrams with data (`count > 0`) do not count towards Tri-ACK Fast Retransmit.
      - Fixed Bug 2 (Zero Silent Loss): Two-pass atomic datagram validation and strict delivery bound check (`delivered_count < max_delivered`) preventing premature ACK generation on buffer saturation.
      - Fixed Bug 3 (RFC 793/1122): Retransmitted duplicate reliable packets re-arm `ack_pending = 1` to unblock peer sliding window.
      - C11 compile-time `_Static_assert` ABI checks on all structures.
  - [x] **Optional Unreliable Scalar Profiles** (`protocol_profiles.h`, `src/profiles.c`; explicitly bound, never length-dispatched against ACKs):
    - [x] **Track A (Compact4)**: seq16 + scalar16; type/flags belong to the out-of-band profile, not an unchanged TFV payload.
    - [x] **Track B (Rolling8)**: seq16 + current16 + previous delta16 + control16. Recovers the previous sample when the next datagram arrives, without a retransmission round trip; not zero elapsed time.
    - [x] **Track C (Adaptive Redundancy)**: Fixed-cadence scalar acceleration/jerk threshold crossings produce one or two identical Rolling8 datagrams for caller emission; no ACK wait, receiver deduplication. Benefit under correlated loss is not guaranteed or benchmarked against other libraries.
  - [x] **Egress Scheduler & MTU Packing (Intra-Tick Bundler)**: Batch intra-tick payloads and multi-channel ACKs into MTU-sized UDP datagrams without cross-tick delay (Anti-Nagle Principle).

---

## Phase 5: Recovery, security and platform support
*Depends on Phase 4.*

- [ ] **Dual-Target Network Stacks**:
  - **Target A (Standard OS / Game Engines)**: Desktop, dedicated servers, and consoles using standard POSIX/BSD and Winsock UDP sockets.
  - **Target B (Embedded / IoT / Robotics)**: Bare-metal and FreeRTOS microcontrollers (STM32, ESP32) using the lightweight **lwIP** stack with zero-malloc static buffers.
- [ ] **WireGuard & Noise Protocol Cryptographic Layer**:
  - Evaluate an existing Noise implementation for encryption and authentication; the channel flag alone provides neither.
  - On Target A: In-process lightweight Noise AEAD or native WireGuard tunnel encapsulation.
  - On Target B: Embedded integration with **`wireguard-lwip`** for encrypted bare-metal communication.
  - Specify peer authentication, key management and replay protection before exposing a secure-channel API.
- [x] **Adaptive RTT & Dynamic Timeout**: Opt-in session recovery, timestamped receive API, fixed-point SRTT/RTTVAR, conservative Karn sampling at most once per RTT, configurable base-RTO bounds, and timeout-only backoff separate from fast repairs. Unchanged wire format and TX slots. See `docs/ADAPTIVE_RECOVERY.md`; `make test-recovery-stress` covers 200 paced-loss runs.
- [ ] **Revisit recovery memory / traffic / latency tradeoffs (explicitly deferred)**: Preserve the measured adaptive-recovery baseline (5,620 B/session, +352 B versus phase 4; about +9% UDP-payload traffic at 5% loss, p99 median 57 ms versus 263 ms). Investigate packed timeout counters, optional recovery-state storage, shared RTT estimation only where path/queue semantics permit it, and selective ACK extensions that suppress redundant retransmissions. Target retaining the latency improvement while reducing added RAM and returning toward phase-4 traffic; these are experimental objectives, not guarantees. Validate with unchanged baseline workloads plus finite-rate links, bounded queues, reordering, correlated loss and application stalls before claiming a simultaneous improvement.
- [ ] **Estimable / Dead-Reckoning Classification**: Categorize continuous data for local client-side physics interpolation/extrapolation on packet drop.
- [ ] **Transport recovery roadmap (same messages and delivery guarantees as ENet / KCP-fast)**:
  - [ ] **Selective ACKs**: Specify a versioned, bounded extension exposing retained RX messages so the sender suppresses unnecessary retransmissions. Preserve sequence wrap safety, RX retention and flow-control invariants; an absent bit is not proof of loss. Measure control-byte overhead against saved retransmissions.
  - [ ] **Reordering-tolerant time-based loss detection**: Combine new delivery evidence and elapsed transmission time with a tested reordering allowance; distinguish retransmission attempts from timeout backoff. Test lost repairs, repeated/stale ACKs, clock wrap and spurious retransmissions. Do not retransmit unconditionally on the first observed gap.
  - [ ] **Tail-loss probe**: Add a bounded, rate-limited probe mechanism for application-limited traffic with no later messages to reveal a lost tail. Specify rearming, ACK-loss handling, retry limits and interaction with normal RTO; ensure probes do not create ACK loops or transmission storms.
  - [ ] **Finite-rate comparison gate**: Extend the shared simulator with serialization delay, finite bandwidth and bounded queues, followed by the real-socket work in phase 6. Preserve the historical unlimited-link baseline. Compare ENet, KCP-fast and zcrudp on identical messages/semantics, with congestion, correlated loss, reordering and application stalls; publish completion, p99, useful throughput, total traffic and memory. No prediction/omission advantage in reliable comparisons.
- [ ] **XOR-based Forward Error Correction (FEC)**: Optional parity frames ($P = A \oplus B \oplus C$) can reconstruct a single missing member once parity and all other members arrive, without a retransmission round trip, not at zero elapsed time. Account for block-formation delay, metadata, duplicate tracking and concurrent RX groups. Delta/XOR-against-reference encoding changes representation; parity adds recovery redundancy. Current Rolling8 already carries previous-sample redundancy through current+delta, so do not stack another protection layer without measuring its incremental benefit at matched traffic budgets.

---

## Phase 6: Tests and tooling

- [x] **Extended Test Suite**: Existing `tests/test_rudp.c`, `tests/test_phase4.c` and `tests/test_window.c` cover wraparound, out-of-order delivery, malformed/future ACK handling, output backpressure and retry-limit disconnection. Revalidated through Make and Release CTest; broader fuzzing and adverse-network coverage remain separate work.
- [x] **Interactive CLI Demo (`examples/demo_loss.c`)**: POSIX two-peer UDP demo with reliable/unreliable commands, configurable loss/latency/jitter, seeded simulation, bounded delay queue, automatic traffic and integration tests (`make demo`, `make test-tools`).
- [x] **Visual protocol replay**: Real-engine deterministic trace, offline HTML/SVG replay with seek/pause controls and reduced-motion support, plus captured README GIF and static poster (`make visual-trace`, `make visual-report`). Scripted losses/reordering are labeled as explanatory, not comparative performance measurements.
- [x] **Benchmarking Suite (`bench/bench_rudp.c`)**: Codec throughput (Mops/s, Mpps for single frames/records) and amortized cost (ns/op), repeated samples and reproducible CSV/SVG performance graphs in `README.md` (`make bench`, `make bench-report`).
- [x] **Competitive transport benchmark (`bench/compare_transport.c`)**: Run the actual zcrudp, ENet and KCP engines (default and fast profiles) through the same virtual datagram link. Six scenarios, five seeds, 2,400 ordered 4-byte messages per run; measured goodput, p50/p95/p99 delay, emitted bytes and host simulation cost. Pinned/checksummed dependencies, CSV, environment metadata and comparative graphs (`make compare`, `make test-compare`). Failed runs remain visible.
- [ ] **Physical network comparison**: Add a shared real-socket/proxy harness, finite link rates and queue disciplines, real end-to-end latency, CPU time and peak memory/allocation accounting. Current comparative latency/goodput are simulation metrics, not NIC benchmarks.
- [ ] **Broader workloads and transports**: Add larger payloads after bulk-message support, mixed reliable/unreliable channels and GNS/QUIC adapters with matched security and delivery semantics.
- [x] **CMake Integration (`CMakeLists.txt`)**: Static/shared core and optional profiles, CTest with active Release assertions, POSIX tool opt-in, ABI settings propagated to consumers, `add_subdirectory` targets and relocatable `find_package` installation. `make test-cmake` validates package/embedded consumers and custom configuration; Linux static/shared tested, individual game-engine integrations not certified.
- [ ] **One-Command Multi-Language Bindings (Python, Node/Bun, Rust, Go, C++)**:
  - Provide a single command (e.g. `make bindings` or `pip install -e .`) to build and expose the C-ABI shared library (`librudp.so`).
  - Python binding (via `ctypes` or `cffi`) for rapid bot scripting, headless test simulation, and AI game client training.
  - Foreign Function Interface (FFI) templates for Node.js (`node-addon-api` / Bun FFI), Rust (bindgen crate), and Go (cgo).
- [x] **CI/CD & Memory Sanity**: GitHub Actions workflow with AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan) verifying 0 memory leaks and 0 undefined behaviors.
