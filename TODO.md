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
- [ ] **7. Test Suite Hardening & CI**:
  - Replace bare `assert()` with a custom non-collapsible `TEST_ASSERT()` macro (or `-UNDEBUG` in CFLAGS) so tests don't vanish under `-DNDEBUG`.
  - Parameterize `tests/test_rudp.c` for any `RUDP_WINDOW_SIZE` (remove hardcoded 64, 32, 63 constants).
  - Fix `int main()` to `int main(void)` in `tests/test_tfv.c`.
  - Pull forward GitHub Actions CI workflow with AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan), and a fuzz target for `rudp_unpack_frame`.
- [ ] **8. Documentation, Style & Security Policy**:
  - Fix `README.md` Quick Start example to use $N+1$ convention (`ACK=1` acknowledges seq 0).
  - Re-sync `ARCHITECTURE.md` (operator `> 0`, 16-byte slot diagram, retries & connection state machine).
  - Align ASCII diagram in `src/rudp.c` with real struct field order (`frame | timestamp | state | retries | reserved`).
  - Resolve `STYLE_CONVENTION.md` 2-space vs 4-space contradiction and add `.clang-format`.
  - Add `SECURITY.md` (stating the absence of authentication/encryption on raw RUDP, recommending DTLS/TLS if needed).

---

## 📌 Phase 4: Multi-Resolution Architecture & Scalability
*Depends on Phase 3 working bidirectional engine.*

- [ ] **RX Out-of-Order Reassembly Buffer (Selective Repeat)**: Add a lightweight sliding RX buffer to temporarily store ahead-of-order in-window packets instead of dropping them immediately (eliminating Go-Back-N retransmission cascades).
- [ ] **3-Tier Multi-Resolution Packet Support**:
  - **Tier 1 (4 bytes)**: Handle short header-only packets (`seq_num` + `ack`) for pure ACKs, heartbeats/pings, and connection signals.
  - **Tier 2 (8 bytes)**: Standard game frames (Header 4B + TFV 4B).
  - **Tier 3 (Multi-part Streaming)**: Stream large payloads (32-bit floats, text, files) via a TFV descriptor packet followed by $N$ pure 32-bit `packet.raw` frames (UTF-8 style scaling).
- [ ] **Multi-Channel Architecture & Hardware Priority**: 
  - Support independent channel contexts (e.g. Channel 0: Unreliable Movement, Channel 1: High-Priority Reliable Actions, Channel 2: Background Reliable Chat) to eliminate Head-of-Line blocking.
  - Implement channel egress scheduler with priority preemption (critical actions preempt background queues before `sendto()`) and optional IP TOS/DSCP QoS socket tagging.
- [ ] **Unreliable Channels**: Support fire-and-forget packets that bypass `tx_buffer` storage and retransmission (ideal for high-frequency game data like positions).
- [ ] **1400-byte MTU Multiplexing / Bundling**: 
  - Implement batching of multiple payloads into a single standard 1400-byte UDP datagram.
  - Use implicit base-sequence indexing ($seq = base\_seq + k$) to eliminate 50% header overhead ($4\text{B header} + N \times 4\text{B payload}$ instead of repeating $8\text{B}$ per message).

---

## 📌 Phase 5: Network Intelligence & Resilience (Advanced)
*Depends on Phase 4.*

- [ ] **Lightweight Header Checksum (CRC16 / Internet Checksum)**: Verify 16-bit header integrity to prevent bitflips on `ack` or `seq_num` from causing silent data loss or illegal window advancement.
- [ ] **Randomized Initial Sequence Number (ISN) & Replay Defense**: Randomize `current_seq_num` at initialization/handshake to prevent delayed packets from past sessions colliding with a newly initialized context.
- [ ] **Connection Lifecycle**: Implement lightweight `CONNECT` (SYN) and `DISCONNECT` (FIN) control frames for clean session handshakes and teardowns.
- [ ] **Session / Connection ID in Handshake (Low Priority)**: Exchange a lightweight session identifier during `CONNECT` handshake to identify client sessions without bloating standard 8-byte frames.
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
- [ ] **CI/CD & Memory Sanity (Low Priority / Non-urgent)**: GitHub Actions workflow with AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan), and Valgrind (proving 0 memory leaks / 0 undefined behaviors).
