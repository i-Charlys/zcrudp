# zcrudp Development Roadmap & Dependency Plan

This roadmap is organized in **strict dependency order**. Complete each phase before moving to the next to ensure no architectural dead-ends.

---

## 📌 Phase 1: Core Memory Layout & Fixes (Foundations)
*Prerequisite for all subsequent network and serialization work.*

- [x] **Documentation sync**: Update the ASCII diagram in `src/rudp.c` to match `rudp_header_s`.
- [x] **TFV 8/8/16 Layout Refactoring**: Refactor `tfv_packet_u` from 8/4/20 to byte-aligned `uint8_t type` (8b), `uint8_t flags` (8b), `uint16_t value` (16b) in `include/protocol_tfv.h` (eliminates bitfield ambiguity and standardizes 16-bit payload resolution).
- [x] **Window size compile check**: Add `#if (RUDP_WINDOW_SIZE & (RUDP_WINDOW_SIZE - 1)) != 0` check in `include/protocol_rudp.h` ensuring window size is strictly a power of 2.
- [x] **Initialization order**: Fix variable initialization order relative to `memset` in `rudp_init` (`src/rudp.c`).
- [x] **Unit assertions update**: Replace `printf` in `tests/test_tfv.c` with automated `assert()` checks for the new 8/8/16 layout.
- [x] **TFV test coverage**: Assert representative values up to `UINT8_MAX` and `UINT16_MAX`, and verify the complete 4-byte packet layout.

---

## 📌 Phase 2: Wire Format & Network Portability (Serialization)
*Depends on Phase 1 structures.*

- [x] **Endianness & Byte Order**: Implemented zero-dependency `rudp_htons`/`rudp_ntohs` (16-bit) and `rudp_htonl`/`rudp_ntohl` (32-bit) with compiler builtins and Big-Endian Network Byte Order support.
- [x] **Wire Pack / Unpack Functions**:
  - `rudp_pack_frame(const rudp_frame_s *frame, uint8_t *out_buf, size_t max_len)`
  - `rudp_unpack_frame(const uint8_t *in_buf, size_t in_len, rudp_frame_s *out_frame)`
- [x] **TFV endianness & wire tests**: Added automated roundtrip tests, wire byte verification, security bounds, and NULL checks in `tests/test_rudp.c` and `tests/test_tfv.c`.

---

## 📌 Phase 3: Reliability Engine & Full-Duplex (TX / RX Loop)
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

## 📌 Phase 3.5: Protocol Hardening & Audit Remediation (GitHub Issue #3)
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

## 📌 Phase 4: Multi-Resolution Architecture & Scalability
*Depends on Phase 3 working bidirectional engine.*

- [ ] **RX Out-of-Order Reassembly Buffer (Selective Repeat)**: Add a lightweight sliding RX buffer to temporarily store ahead-of-order in-window packets instead of dropping them immediately (eliminating Go-Back-N retransmission cascades).
- [ ] **3-Tier Multi-Resolution Packet Support**:
  - [x] **Tier 1 (4 bytes)**: Handle short header-only packets (`seq_num` + `ack`) for pure ACKs, heartbeats/pings, and connection signals (`rudp_pack_ack()`, `rudp_unpack_ack()`).
  - [x] **Tier 2 (8 bytes)**: Standard game frames (Header 4B + TFV 4B) with atomic modular encoders/decoders.
  - [ ] **Tier 3 (Multi-part Streaming)**: Stream large payloads (32-bit floats, text, files) via a TFV descriptor packet followed by $N$ pure 32-bit `packet.raw` frames (UTF-8 style scaling).
- [ ] **Multi-Channel Architecture & User-Configurable Profiles**: 
  - [x] Define bitwise capability flags (`RUDP_CHANNEL_FLAG_RELIABLE`, `RUDP_CHANNEL_FLAG_ORDERED`, `RUDP_CHANNEL_FLAG_ENCRYPTED`).
  - [x] Implement multi-channel session structures (`rudp_channel_s`, `rudp_session_s`) with zero dynamic allocation (`RUDP_MAX_CHANNELS = 4`, ~4 KB footprint).
  - [x] Implement session initialization and channel configuration API (`rudp_session_init`, `rudp_session_config_channel`).
  - [ ] Channel egress scheduler with priority preemption (critical channels preempt background queues before `sendto()`) and optional IP TOS/DSCP QoS socket tagging.
  - [ ] **Next-Gen Unreliable Channel Engine (The 3-Tier Trinity)**:
    - **Fast Memory Bypass**: Direct egress with zero `tx_buffer` allocation or copying, and anti-rollback sequence filtering on RX.
    - **Track A (Ultra-Dense 4B Game Datagram)**: Leverage Tier 1 (4 bytes total) for compact input/angle telemetry without RUDP header overhead, cutting bandwidth by 50%.
    - **Track B (Rolling Delta / 0ms Instant Recovery)**: Embed state $N$ alongside compact delta of state $N-1$ in Tier 2 (8 bytes) to mathematically reconstruct dropped frames on the receiver with 0ms round-trip latency.
    - **Track C (Kinematic Adaptive Redundancy)**: Egress scheduler detects motion inflection points (acceleration/jerk) and automatically emits forward-cloned duplicates ($2\times$) without waiting for ACKs.
- [ ] **1400-byte MTU Multiplexing / Bundling**: 
  - Implement batching of multiple payloads into a single standard 1400-byte UDP datagram.
  - Use implicit base-sequence indexing ($seq = base\_seq + k$) to eliminate 50% header overhead ($4\text{B header} + N \times 4\text{B payload}$ instead of repeating $8\text{B}$ per message).

---

## 📌 Phase 5: Security, Transport Intelligence & Dual Target (WireGuard & lwIP)
*Depends on Phase 4.*

- [ ] **Dual-Target Network Stacks**:
  - **Target A (Standard OS / Game Engines)**: Desktop, dedicated servers, and consoles using standard POSIX/BSD and Winsock UDP sockets.
  - **Target B (Embedded / IoT / Robotics)**: Bare-metal and FreeRTOS microcontrollers (STM32, ESP32) using the lightweight **lwIP** stack with zero-malloc static buffers.
- [ ] **WireGuard & Noise Protocol Cryptographic Layer**:
  - Outsource encryption and authentication to the mathematically audited **Noise Protocol Framework** (ChaCha20-Poly1305 + Curve25519) for channels flagged with `RUDP_CHANNEL_ENCRYPTED`.
  - On Target A: In-process lightweight Noise AEAD or native WireGuard tunnel encapsulation.
  - On Target B: Embedded integration with **`wireguard-lwip`** for encrypted bare-metal communication.
  - Eliminates the need for fragile homemade crypto, providing military-grade mutual authentication and replay defense.
- [ ] **Adaptive RTT & Dynamic Timeout (Van Jacobson Algorithm)**: Measure sample ping (in ms) on each received ACK, track smoothed RTT (SRTT) and jitter (RTTVAR) using integer EWMA bit-shifts (`>> 3`, `>> 2`), and dynamically compute elastic timeout (`RTO = SRTT + 4 * RTTVAR`).
- [ ] **Estimable / Dead-Reckoning Classification**: Categorize continuous data for local client-side physics interpolation/extrapolation on packet drop.
- [ ] **XOR-based Forward Error Correction (FEC)**: Support generating and decoding XOR parity frames ($P = A \oplus B \oplus C$) to mathematically reconstruct lost frames locally on the receiver with 0ms round-trip latency.

---

## 📌 Phase 6: Testing, Tooling & Master's Showcase
*Run in parallel / at the end to build the portfolio showcase.*

- [ ] **Extended Test Suite**: Unit tests for edge cases (wraparound rollover, out-of-order delivery, corrupted ACKs, maximum retry limits).
- [ ] **Interactive CLI Demo (`examples/demo_loss.c`)**: Runnable 2-node client/server demo with configurable simulated packet loss and latency injection.
- [ ] **Benchmarking Suite (`bench/bench_rudp.c`)**: Measure packet encoding/decoding throughput (Mpps) and latency (ns) with performance graphs for `README.md`.
- [ ] **CMake Integration (`CMakeLists.txt`)**: Modern CMake build script alongside `Makefile` for one-click integration into game engines (Raylib, SDL2, Unreal, Godot).
- [ ] **One-Command Multi-Language Bindings (Python, Node/Bun, Rust, Go, C++)**:
  - Provide a single command (e.g. `make bindings` or `pip install -e .`) to build and expose the C-ABI shared library (`librudp.so`).
  - Python binding (via `ctypes` or `cffi`) for rapid bot scripting, headless test simulation, and AI game client training.
  - Foreign Function Interface (FFI) templates for Node.js (`node-addon-api` / Bun FFI), Rust (bindgen crate), and Go (cgo).
- [x] **CI/CD & Memory Sanity**: GitHub Actions workflow with AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan) verifying 0 memory leaks and 0 undefined behaviors.
