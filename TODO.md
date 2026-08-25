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

- [ ] **ACK N+1 Convention & In-Window Check**:
  - Adopt `ACK = expected_seq` ($N+1$) convention so `(int16_t)(ack - seq) > 0` safely validates in-flight packets and prevents $t=0$ race conditions without using flag bits.
  - Reject corrupted or out-of-window ACKs in `rudp_recv_ack`.
- [ ] **Reliability edge-case tests**: Cover timeout boundaries, repeated `rudp_tick()` calls, empty-window ACKs, stale ACKs, and ACK rollover around `65535 -> 0`.
- [ ] **Window-size tests**: Verify `RUDP_WINDOW_SIZE=0`, `1`, non-powers of two, and a valid small window at compile time and runtime.
- [ ] **Reception Engine (RX)**:
  - Track `expected_seq` on the receiver side.
  - Detect and discard duplicate packets while re-emitting ACKs to calm the sender.
- [ ] **Full-Duplex Context**: Combine TX sliding window and RX state within `rudp_context_s` to enable automatic ACK piggybacking on outgoing data frames.
- [ ] **Fast Retransmit (Tri-ACK)**: If 3 duplicate ACKs arrive for the same sequence without tail moving, immediately retransmit the slot at `tail` without waiting for the timeout timer to expire.
- [ ] **Retransmission Limit & Dead Peer**: Add per-slot retry counter (`slot->retries > MAX_RETRIES`) to detect disconnected peers.

---

## 📌 Phase 4: Multi-Resolution Architecture & Scalability
*Depends on Phase 3 working bidirectional engine.*

- [ ] **3-Tier Multi-Resolution Packet Support**:
  - **Tier 1 (4 bytes)**: Handle short header-only packets (`seq_num` + `ack`) for pure ACKs, heartbeats/pings, and connection signals.
  - **Tier 2 (8 bytes)**: Standard game frames (Header 4B + TFV 4B).
  - **Tier 3 (Multi-part Streaming)**: Stream large payloads (32-bit floats, text, files) via a TFV descriptor packet followed by $N$ pure 32-bit `packet.raw` frames (UTF-8 style scaling).
- [ ] **Multi-Channel Architecture**: Support independent channel contexts (e.g. Channel 0: Unreliable Movement, Channel 1: Reliable Actions, Channel 2: Reliable Chat) to prevent Head-of-Line blocking between independent streams.
- [ ] **Unreliable Channels**: Support fire-and-forget packets that bypass `tx_buffer` storage and retransmission (ideal for high-frequency game data like positions).
- [ ] **1400-byte MTU Multiplexing / Bundling**: Implement batching of multiple 8-byte frames (up to 175 frames) into a single standard 1400-byte UDP datagram.

---

## 📌 Phase 5: Network Intelligence & Resilience (Advanced)
*Depends on Phase 4.*

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
