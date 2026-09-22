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

- [ ] **Couche d'I/O unifiée Scatter-Gather (Zero-Copy Universal I/O)**:
  - Une seule API d'entrée-sortie vectorielle (`rudp_iovec_s`) 100% portable et agnostique de l'OS.
  - Élimine la scission en deux stacks ("Target A vs Target B") : le cœur zcrudp reste pur, unique et sans `malloc`.
  - Sur PC (Linux, macOS, BSD, Windows) : délégation de l'envoi vectoriel à `sendmsg` (`struct iovec`) ou `WSASendTo` (`WSABUF`).
  - Sur microcontrôleurs (STM32, ESP32) et bare-metal : délégation directe aux pbufs de lwIP (`PBUF_REF`) ou aux anneaux de descripteurs DMA matériels de la puce Ethernet.
  - **Support d'entropie multi-chemins pour diversité d'interfaces** : calcul en temps constant d'un hash d'entropie de flux (`path_entropy`) exposé dans les métadonnées d'I/O, permettant à la couche socket hôte de faire tourner dynamiquement le port source UDP et de répartir les paquets sur plusieurs interfaces physiques (Wi-Fi, cellulaire, liaisons radio multiples) pour assurer la redondance et éviter le blocage sur un lien unique.
- [ ] **Couche cryptographique modulaire (Noise / WireGuard - Wrapper externe)**:
  - Intégration strictement modulaire et découplée sous forme de surcouche (wrapper externe).
  - Le cœur de zcrudp reste 100% autonome et sans dépendance externe obligatoire (pas de dépendance forcée à OpenSSL ou Libsodium).
  - Fournir des adaptateurs optionnels : Noise AEAD (ChaCha20-Poly1305) léger pour l'embarqué ou encapsulation tunnel WireGuard.
  - **Audit #1, garantie complète**: Authentifier le datagramme UDP complet avant toute mutation de l'état RUDP, y compris ACK, contrôles, données et réparations FEC. Une enveloppe doit lier un identifiant de session/nonce, maintenir une fenêtre anti-rejeu bornée et rejeter `ACK`, `HELLO`, `RESET` ou données dont le tag est invalide. Conserver un état à taille fixe fourni par l'appelant et aucune allocation dans le cœur. C'est la correction complète de l'injection d'ACK; les séquences initiales aléatoires ne fournissent qu'un durcissement contre l'injection aveugle.
- [x] **Adaptive RTT & Dynamic Timeout**: Opt-in session recovery, timestamped receive API, fixed-point SRTT/RTTVAR, conservative Karn sampling at most once per RTT, configurable base-RTO bounds, and timeout-only backoff separate from fast repairs. Unchanged wire format and TX slots. See `docs/ADAPTIVE_RECOVERY.md`; `make test-recovery-stress` covers 200 paced-loss runs.
- [ ] **Breaking core wire/recovery redesign (single format, no legacy-wire compatibility)**:
  Replace the current session-only timing policy with one bounded recovery engine used directly by `rudp_context_s` and wrapped by `rudp_session_s`. This is an intentional wire break: both endpoints must use the new format; do not add downgrade negotiation or preserve decoding of the previous format.
  - **Preserve the clean data format**: Keep full 16-bit `seq_num` and `ack` values and keep normal frames/records at 8 bytes. Do not steal sequence bits, payload bits, or TFV type values. Keep the existing 16-bit serial arithmetic and the `65535 -> 0` behavior. Encode control metadata with masks and shifts, never C bitfields.
  - **Compact homogeneous runs in session datagrams**: Avoid paying a four-byte record header for every four-byte payload when several consecutive records share a channel and kind. Keep the four-byte datagram header, then encode each run as `{base_seq16, channel_kind8, item_count8}` followed by `item_count` raw four-byte TFV payloads; sequence numbers after `base_seq` are implicit. A datagram therefore costs `4 + ack_prefix_bytes + sum(4 + 4*N_run)` instead of `4 + 8*N`. One isolated data record remains 12 bytes, two same-run records fall from 20 to 16 bytes, and ten fall from 84 to 48 bytes when no extra channel ACK is present. Permit multiple runs for mixed channels, precede them with the compact ACK prefix described below, and keep FEC source and repair runs in independent UDP loss domains. Require strict length validation, bounded run counts, wrap-safe implicit sequence expansion, and no cross-tick batching delay.
  - **Dedicated 4-byte control exchange**: Reuse the otherwise-unused `seq_num` word of standalone 4-byte ACKs. Packet length already distinguishes a control header from an 8-byte data frame. Define its low byte as a packed control byte with channel in bits 0..1, kind in bits 2..4 (`ACK`, `RTT_PROBE`, `RTT_REPLY`, `RECOVERY_ACK`, `HELLO`, `HELLO_ACK`, `RESET`), and a three-bit control epoch in bits 5..7; keep the high byte reserved and zero. In the session header, carry the same control byte in `ack_channel` when `count == 0`. Require `RUDP_MAX_CHANNELS <= 4`; data datagrams keep kind/epoch zero. Normal ACK/probe request/reply controls continue to carry the full 16-bit cumulative ACK.
  - **Compact ACK prefix for ACK-only and mixed datagrams**: Let the four-byte header acknowledge the primary channel, then place every additional pending channel ACK before the data runs as four bytes `{channel_id8, ack_flags8, ack16}` instead of the current eight-byte `rudp_record_s` with an unused TFV payload. Pack the last header byte as `ack_count` (2 bits, 0..3) plus `run_count` (6 bits, 0..63); make the builder split a datagram before the run count overflows and test the bound against worst-case channel/kind alternation. Validate exactly the ACK prefix first and then every declared run before mutating state. Four simultaneous channel ACKs cost 16 bytes (`4 + 3*4`) instead of 28 (`4 + 3*8`), while a mixed datagram pays the same compact prefix and no additional UDP/IP packet. Do not force an odd entry count or emit padding ACKs: UDP accepts arbitrary lengths, bytewise codecs do not benefit from total-length alignment, and only genuinely pending entries may count as deliberate duplicate ACKs for Tri-ACK. Reserve ACK space before scheduling data, as the current builder already does.
  - **RTT samples come only from probes**: At the first reliable transmission, emit one standalone `RTT_PROBE`; the receiver immediately returns `RTT_REPLY` with the same epoch and its current cumulative ACK. Store the local probe-send time and accept a sample only for the matching outstanding epoch. Data ACKs advance reliability but never train SRTT, so retransmission ambiguity, cumulative jumps, RX buffering, and FEC reconstruction cannot contaminate the estimator. Later probes are sent only when the estimate is stale or recovery indicates a possible path change, with at most one outstanding probe and no probe retransmission loop.
  - **Audits #1/#2/#6: restart-safe establishment and first-unreliable anchoring without permanent header cost**: Add a bounded per-channel `HELLO`/`HELLO_ACK` state machine before accepting data. For these control kinds, reinterpret the 16-bit ACK word as a caller-seeded random initial sequence number; each direction installs the peer's fresh reliable and unreliable sequence anchor and resets old TX/RX/FEC/probe state atomically. Never let the first unreliable record establish an arbitrary receive baseline: it must fall within `RUDP_UNRELIABLE_MAX_AHEAD` of the sequence negotiated for that channel. Reject normal data and ACK controls until establishment completes, rate-limit `HELLO`/`RESET`, and test simultaneous open, one-sided restart, an arbitrary first unreliable sequence, blind ACK/data injection before establishment, lost handshake controls, delayed packets from the prior epoch, and sequence wrap. This fixes restart wedging and first-record baseline poisoning, and makes blind sequence guessing harder without enlarging normal frames. A 16-bit random anchor is only probabilistic hardening (at most 65,536 possibilities), not authentication: cleartext deployments remain vulnerable to an observer or accurate injector and still require the documented DTLS/WireGuard/AEAD wrapper on untrusted networks.
  - **Core-owned adaptive state**: Move `srtt_scaled`, `rttvar_scaled`, `rto_ms`, `rto_min_ms`, `rto_max_ms`, and `last_rtt_sample` from `rudp_channel_s` into `rudp_context_s`; make session recovery call the core implementation instead of maintaining a second timer path. The expected default standalone context cost is +24 bytes (1040 -> 1064), while the session should not duplicate those fields.
  - **Recover the timeout-counter RAM**: Store the retransmission count and timeout-backoff exponent in the low/high nibbles of the existing slot retry byte. Add masked accessors and compile-time bounds (`RUDP_MAX_RETRIES <= 15`, backoff exponent <= 15); remove `timeout_backoffs[RUDP_WINDOW_SIZE]`. With a 64-slot window and four channels, target approximately 1340 bytes/channel and 5364 bytes/session before any explicit FEC decoder state. Confirm every size with `_Static_assert` rather than relying on these estimates.
  - **Exactly one early tail copy**: While the initial RTT probe is outstanding, continue sending new data and allow one copy of the oldest unacknowledged slot at a configurable 100--150 ms delay. This copy is loss exploration, not an RTT sample. Once the probe reply seeds SRTT/RTTVAR, use the adaptive timer; a separate conservative RTO remains the final recovery timer.
  - **Audit #7: automatic liveness on every valid receive path**: Update `last_rx_time` only after complete wire validation for standalone controls, core frames, session headers, and every addressed channel in a multi-channel datagram. Reuse the rate-limited RTT probe as keepalive traffic when idle. Expose session-level health without requiring applications to remember a separate `rudp_touch()` call, and test idle, unreliable-only, one-way, malformed-input, and clock-wrap cases.
  - **Observable rejection counters**: Add bounded saturating counters (or an optional caller-owned statistics sink) for stale/future ACKs, implausible unreliable jumps, malformed datagrams, out-of-window reliable records, FEC failures, probe timeouts, and rate-limited controls. Rejected unreliable jumps must remain observable instead of being indistinguishable from an idle poll; preserve `RUDP_UNRELIABLE_MAX_AHEAD` as a configurable plausibility bound.
  - **Probe and repair budget**: Share one per-context pacing budget across original data, ACKs, RTT controls, tail copies, retransmissions, and parity. At most one RTT probe and one early tail copy may be outstanding; neither can recursively arm another. New application data continues during calibration, while parity and speculative repairs must yield when the finite-rate link is already saturated. Define epoch wrap, delayed replies, reset, clock wrap, ACK loss, reordering, application stalls, and retry-limit transitions before implementation.
  - **Cross-tier conformance tests**: Feed identical traces through the standalone context and session wrapper and require identical sequence advancement, RTT samples, RTO updates, retransmission choices, and disconnect decisions. Fuzz the packed codec separately from the state machine, then run finite-bandwidth/queue benchmarks reporting completion, useful bytes, total wire bytes, p99 and worst-case latency, CPU, and exact static RAM.
- [ ] **Revisit recovery memory / traffic / latency tradeoffs (explicitly deferred)**: Preserve the measured adaptive-recovery baseline (5,620 B/session, +352 B versus phase 4; about +9% UDP-payload traffic at 5% loss, p99 median 57 ms versus 263 ms). Investigate packed timeout counters, optional recovery-state storage, shared RTT estimation only where path/queue semantics permit it, and selective ACK extensions that suppress redundant retransmissions. Target retaining the latency improvement while reducing added RAM and returning toward phase-4 traffic; these are experimental objectives, not guarantees. Validate with unchanged baseline workloads plus finite-rate links, bounded queues, reordering, correlated loss and application stalls before claiming a simultaneous improvement.
- [ ] **Close residual audit and public-API drift**:
  - Decide and enforce `RUDP_CHANNEL_FLAG_ORDERED` semantics or remove the inert flag; keep rejecting reliable sends on channels without `RUDP_CHANNEL_FLAG_RELIABLE`. Rename `RUDP_CHANNEL_FLAG_ENCRYPTED` to an explicit integration hint or remove it so no caller can mistake metadata for active encryption.
  - [x] Remove the duplicated/stale Doxygen `@return` lines, document the actual public return values and current inert hints, and synchronize `ARCHITECTURE.md` with the checked structures and state machine.
  - Move the public byte-order helper macros into internal/test scope if production code continues to serialize explicitly; retain the `RUDP_` namespace for every exported macro.
  - Add a mutation-test job for parser bounds, stale/future ACK rejection, valid reset state clearing, capability enforcement, and exact datagram lengths so the six historically surviving validation mutations cannot regress silently.
- [ ] **Estimable / Dead-Reckoning Classification**: Categorize continuous data for local client-side physics interpolation/extrapolation on packet drop.
- [ ] **Transport recovery roadmap (same messages and delivery guarantees as ENet / KCP-fast)**:
  - [ ] **Selective ACKs**: Specify a versioned, bounded extension exposing retained RX messages so the sender suppresses unnecessary retransmissions. Preserve sequence wrap safety, RX retention and flow-control invariants; an absent bit is not proof of loss. Measure control-byte overhead against saved retransmissions.
    - **Wire**: Extend the 4-byte unified datagram header (`ack`, `ack_channel`, `count`) with an optional, versioned 4- or 8-byte SACK bitmask anchored at `ack` (the cumulative ACK stays $N+1$ and authoritative). Gate it behind a capability/flag bit so the header stays 4 bytes when unused and peers that do not negotiate it ignore the trailing bytes; keep the bounded length check strict (`4 [+ mask] + count * 8`).
    - **Sender behaviour**: On the first acknowledged gap, retransmit only the missing sequence(s) immediately with no timer wait, and do not invalidate or re-send segments the RX side already retains past the hole (no go-back-N). A micro-delay or queue swap that only reorders later packets must not arm a retransmission for them.
    - **Bounds**: Mask covers strictly the current in-flight window, is wrap-safe (RFC 1982 distance), and is advisory only — the cumulative `ack` still drives window advance and Tri-ACK. Cap probe/repair rate so a flapping gap cannot create a retransmission storm.
  - [ ] **Reordering-tolerant time-based loss detection**: Combine new delivery evidence and elapsed transmission time with a tested reordering allowance; distinguish retransmission attempts from timeout backoff. Test lost repairs, repeated/stale ACKs, clock wrap and spurious retransmissions. Do not retransmit unconditionally on the first observed gap.
  - [ ] **Tail-loss probe**: Add a bounded, rate-limited probe mechanism for application-limited traffic with no later messages to reveal a lost tail. Specify rearming, ACK-loss handling, retry limits and interaction with normal RTO; ensure probes do not create ACK loops or transmission storms.
    - **Problem**: When the last packet of a salvo is lost or delayed, Tri-ACK / Fast Retransmit cannot fire — no later packet arrives to generate duplicate ACKs — so recovery falls back to the full RTO expiry in `rudp_tick()`, a passive latency spike. This is the historical weak point of ENet and plain RUDP.
    - **Solution**: When the channel is application-limited (TX queue empty, window not fully acked, no later message pending), arm a single probe at $\approx 1.5 \times \text{SRTT}$ after the last transmission. If no ACK has returned, speculatively re-send the last active slot (or a zero-payload tail segment) before the full RTO fires.
    - **Guards**: Bound to a small retry count, rate-limit to at most one probe per RTT, rearm on new data or on any ACK, and treat a probe that does uncover loss as normal recovery input without double-counting timeout backoff. A probe must not itself trigger another probe or an ACK loop.
  - [ ] **Finite-rate comparison gate**: Extend the shared simulator with serialization delay, finite bandwidth and bounded queues, followed by the real-socket work in phase 6. Preserve the historical unlimited-link baseline. Compare ENet, KCP-fast and zcrudp on identical messages/semantics, with congestion, correlated loss, reordering and application stalls; publish completion, p99, useful throughput, total traffic and memory. No prediction/omission advantage in reliable comparisons.
- [ ] **Optional XOR K=2 for rare independent loss**: Protect two reliable four-byte records $A,B$ with one parity record $P=A\oplus B$. Keep this as an opt-in profile for sustained traffic with roughly 0.1–0.2% independent loss, alongside the existing non-FEC path. In the real-engine virtual-link experiment (2,400 messages at 240 Hz, RTT 250 ms, 100 seeds), K=2/RTO250 yielded median p99 140 ms and 22.33 UDP-payload B/message at 0.1% loss, versus 220 ms and 26.40 B/message with the current RTO100; clean-path cost was 22 versus 32 B/message. At 0.5% loss its cross-seed p90 worsened, and at 1% loss or two-packet bursts the existing profile had better p99. These are workload-specific measurements from the discarded experiment and must be reproduced in the redesigned real engine before influencing defaults.
  - Keep normal data frames untouched. Session records use a `FEC_PARITY` record flag and a full 16-bit pair-base sequence. The standalone core uses a distinct optional 9-byte repair frame: one-byte repair tag, full 16-bit pair base, full 16-bit cumulative ACK, and four-byte parity. Parse it bytewise so the odd wire length never creates an unaligned C access. Data frames remain fixed at 8 bytes. `RECOVERY_ACK` may report a reconstruction for telemetry, but RTT sampling is already isolated in dedicated probes and does not depend on receiving this indication. No old-wire negotiation or downgrade path is required; both endpoints must run the same protocol format.
  - Keep $A$, $B$, and $P$ in independent UDP loss domains. Never bundle all three into one datagram: losing that datagram would erase both sources and their parity. A parity record may share a datagram only when the chosen grouping still survives the modeled datagram loss. Make this scheduler invariant testable.
  - Bound RX group state without heap allocation and handle 16-bit sequence wrap, reset, partial groups, late duplicates, in-order delivery, and parity arriving before either source. Derive group identity from the full sequence so no coefficient or separate group-ID bytes are added.
  - Decide how to close an incomplete group for an isolated message. A virtual-link copy sent after 10 ms reduced forced-loss delivery from 377 to 136 ms and the 10 Hz/5% loss p99 median from 235 to 143 ms, but raised 10 Hz clean traffic from 23.15 to 31.11 B/message. Treat this as a separate latency-priority option; a real implementation must count the extra transmission in the slot and obey Karn's RTT sampling rule.
  - Compare against the unchanged baseline on 0–2% independent loss, correlated bursts, reordering, finite-rate links, sparse/irregular traffic and one-message tails. Report completion, per-run and cross-seed p99, wire bytes, CPU and bounded RAM. Test the actual protocol and sockets before enabling by default. The exploratory branches and their simplified simulators are not production validation.

---

## Phase 6: Tests and tooling

- [x] **Extended Test Suite**: Existing `tests/test_rudp.c`, `tests/test_phase4.c` and `tests/test_window.c` cover wraparound, out-of-order delivery, malformed/future ACK handling, output backpressure and retry-limit disconnection. Revalidated through Make and Release CTest; broader fuzzing and adverse-network coverage remain separate work.
- [x] **Interactive CLI Demo (`examples/demo_loss.c`)**: POSIX two-peer UDP demo with reliable/unreliable commands, configurable loss/latency/jitter, seeded simulation, bounded delay queue, automatic traffic and integration tests (`make demo`, `make test-tools`).
  - [x] **Sub-tick poll during bursts**: The event loop polls with `timeout = 0` while a burst is outstanding (auto-generation running, reliable slots unacked, or datagrams queued) so measured latency is not quantised to the OS scheduler tick; it blocks up to 5 ms only when idle. Removes the 0..5 ms phase offset that previously measured scheduler latency rather than transport latency.
  - [x] **Vectored egress (Target A pattern)**: `flush()` sorts the due datagrams by `(due, seq)` and, on Linux, hands the whole intra-tick burst to the kernel in one `sendmmsg()` call instead of looping on `sendto()`; other POSIX hosts keep the `sendto()` loop. A blocked socket or short send stops the drain and preserves cross-tick ordering. `tests/test_demo_queue.c` hooks `sendmmsg()` accordingly.
- [x] **Visual protocol replay**: Real-engine deterministic trace, offline HTML/SVG replay with seek/pause controls and reduced-motion support, plus captured README GIF and static poster (`make visual-trace`, `make visual-report`). Scripted losses/reordering are labeled as explanatory, not comparative performance measurements.
- [x] **Benchmarking Suite (`bench/bench_rudp.c`)**: Codec throughput (Mops/s, Mpps for single frames/records) and amortized cost (ns/op), repeated samples and reproducible CSV/SVG performance graphs in `README.md` (`make bench`, `make bench-report`).
- [x] **Competitive transport benchmark (`bench/compare_transport.c`)**: Run the actual zcrudp, ENet and KCP engines (default and fast profiles) through the same virtual datagram link. Nine scenarios, five seeds, 2,400 ordered 4-byte messages per run; measured goodput, p50/p95/p99 delay, emitted bytes and host simulation cost. Pinned/checksummed dependencies, CSV, environment metadata and comparative graphs (`make compare`, `make test-compare`). Failed runs remain visible.
- [ ] **Physical network comparison & Netem harness**:
  - Add a shared real-socket test harness using Linux `netem` for adverse network emulation: Gilbert-Elliott burst loss models, Pareto delay/jitter distributions, and asymmetric packet reordering.
  - Measure empirical Cumulative Distribution Functions (CDFs) of latency (p50, p95, p99), goodput, CPU time, and allocation accounting against ENet and TCP baselines under matching constraints.
- [ ] **Broader workloads & Payload Scalability**:
  - Benchmark framing efficiency across realistic payload distributions (64B, 256B, 512B, 1200B MTU slices) in addition to 4B micro-records, documenting the framing-to-payload efficiency curve.
  - Stress-test WAN Bandwidth-Delay Product (BDP) limits at 80-150 ms RTT under high message rates (120 Hz) to quantify window saturation and validate backpressure mitigations.
  - Mixed reliable/unreliable channels and GNS/QUIC adapters with matched security and delivery semantics.
- [ ] **Fuzzing & Invariant Verification (`libFuzzer` / `AFL++`)**:
  - Run continuous fuzzing on datagram decoders and state machine transitions with ASan/UBSan to guarantee crash-free behavior on malformed or hostile network inputs.
  - Formally document and verify state machine invariants (RFC 1982 sequence distance, wrap safety, strictly bounded time/space complexity without OS-dependent variability).
- [x] **CMake Integration (`CMakeLists.txt`)**: Static/shared core and optional profiles, CTest with active Release assertions, POSIX tool opt-in, ABI settings propagated to consumers, `add_subdirectory` targets and relocatable `find_package` installation. `make test-cmake` validates package/embedded consumers and custom configuration; Linux static/shared tested, individual game-engine integrations not certified.
- [ ] **One-Command Multi-Language Bindings (Python, Node/Bun, Rust, Go, C++)**:
  - Provide a single command (e.g. `make bindings` or `pip install -e .`) to build and expose the C-ABI shared library (`librudp.so`).
  - Python binding (via `ctypes` or `cffi`) for rapid bot scripting, headless test simulation, and AI game client training.
  - Foreign Function Interface (FFI) templates for Node.js (`node-addon-api` / Bun FFI), Rust (bindgen crate), and Go (cgo).
- [x] **CI/CD & Memory Sanity**: GitHub Actions workflow with AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan) verifying 0 memory leaks and 0 undefined behaviors.

---

## Phase 7: Research tracks & next-gen architecture

Exploratory tracks for zero-copy streaming, asymmetric channel window partitioning, delay-gradient micro-pacing, and resilient multi-interface transport.

- [ ] **Copy-Avoiding Page Streaming & Portable Scatter-Gather (Tier 3 V2)**:
  Treat payload copy avoidance as a backend capability rather than promise strict physical zero-copy on every OS. Standard `sendmsg()` avoids user-space concatenation but may still copy into the kernel; Linux `MSG_ZEROCOPY`, overlapped Windows I/O, lwIP references and DMA all have different completion and lifetime rules.
  - **Prerequisite: explicit buffer-loan contract**: A submitted page is immutable and remains owned by the caller but borrowed by RUDP until it is ACKed, abandoned after retry failure, or released by reset. Return a caller cookie through a bounded completion event/callback. Distinguish network delivery completion from NIC/OS submission completion, and define cancellation before storing raw pointers in TX slots.
  - **Registered caller-owned page pool**: Register a fixed array of page entries once; each entry contains the base pointer, capacity, caller cookie, generation, protocol-reference count and asynchronous-I/O-reference count. Store only a generation-checked handle `{index16, generation16}` plus `offset32` and `length32` in each TX slot. Validate `offset + length <= capacity` without overflow. A page becomes reusable only when both reference counts reach zero; ACK, retry exhaustion and reset drop protocol references, while backend completion drops I/O references. Report releases through a bounded event queue processed by the same event loop, reject stale generations/double release, and keep `send_copy` as the safe fallback for callers that cannot honor the loan. Never drop a release when the queue is full: retain a `release_pending` bit in the registry and apply backpressure until the caller drains it. Optional debug mode may hash borrowed bytes to detect mutation before retransmission.
  - **Prerequisite: portable packet plan**: Define only neutral fixed-size spans such as `{const uint8_t *base, uint32_t length}` plus total length, packet token and bounded segment count. Keep `struct iovec`, `WSABUF`, `pbuf` and DMA descriptors out of the core. Copy the small mutable RUDP header into caller/adapter-owned scratch while borrowing the large immutable payload.
  - **Prerequisite: transactional egress**: Split scheduling into `prepare -> submit -> commit/cancel`. Preparing a view must not update timestamps, retry counts or fairness cursors. Commit only after the backend accepts the complete UDP datagram; `WOULD_BLOCK` leaves it eligible without double-counting a transmission.
  - **Reference flattening backend first**: Concatenate a packet plan into the existing byte buffer and require byte-for-byte equality with the current codec for normal sends, retransmissions, MTU boundaries, wrap and mixed channels. This remains the universal fallback when a backend lacks scatter-gather support or exceeds its segment/alignment limits.
  - **Backend adapters second**: Translate the same plan to POSIX `sendmsg(iovec)`, Windows `WSASendTo(WSABUF)`, lwIP `pbuf_chain(PBUF_REF)` and caller-provided DMA descriptors. Each adapter declares bounded capabilities (`max_segments`, alignment, synchronous/asynchronous completion and DMA-accessible memory); it may flatten without changing protocol behavior.
  - **Large-payload TX first**: Keep the existing inline/copy path for four-byte TFV records and measure the crossover before choosing a threshold. Add MTU-sized extents (roughly 512--1400 bytes) with 32-bit `offset + length` only for page streams. Defer receive-side zero-copy until the TX ownership and completion model is proven, because out-of-order RX must retain NIC buffers across gaps.
  - **Interactions to specify before implementation**: Header mutation on retransmission, ACK/reset release order, async completion after reset, finite descriptor exhaustion, FEC reads, AEAD implementations that require contiguous scratch, cache clean/invalidate for non-coherent DMA, and a maximum span count that never depends on heap allocation.

- [ ] **Heterogeneous Per-Channel Windows via Shared Static Metadata Slot Pool**:
  - Replace uniform per-channel arrays (`tx_buffer[RUDP_WINDOW_SIZE]`) with a single session-wide pool of fixed-size transmission metadata slots (for example 256 total slots).
  - Store small TFV payloads inline, but represent stream pages with a caller-owned immutable buffer reference, `offset`, `length` and release cookie. Do not make every transport slot 1,200--1,400 bytes: that would restore the copy and inflate session RAM. Keep the optional caller-owned page/RX storage separate from transport metadata and validate pointer/handle size on 32- and 64-bit ABIs.
    - **RAM break-even calculation**: With the registered pool, let `D = sizeof(rudp_page_extent_s)` and `L` be the inline payload length; common sequence/timer/slot metadata cancels from both alternatives. The proposed per-slot extent `{index16, generation16, offset32, length32}` is 12 bytes on both 32- and 64-bit ABIs, so it saves `L - 12` bytes per in-flight slot and becomes strictly smaller when `L > 12`. Verify the actual ABI with `_Static_assert`; report the separate fixed page-registry cost and amortize each registry entry across every extent referencing that page.
    - **Examples**: A four-byte TFV costs 4 bytes inline but 12 bytes by handle, so inline wins by 8 bytes. At 12 bytes the choices tie; at 16 bytes the handle saves 4 bytes; at 32 bytes it saves 20 bytes. A 512-byte extent saves 500 bytes per occupied slot (31.25 KiB across 64 slots), and a 1,400-byte extent saves 1,388 bytes (86.75 KiB across 64 slots), before accounting for the much smaller shared registry.
    - **Practical threshold**: `L > D` is only the RAM threshold. Scatter-gather setup, cache behavior, adapter limits and asynchronous bookkeeping can make copying faster for small buffers. Keep the inline/reference cutoff configurable and benchmark it per backend; use the planned 512--1,400-byte page range as the initial experiment, not as a universal constant.
  - Channels carve bounded quotas or contiguous slices from the metadata pool at initialization:
    - Channel 0 (Telemetry / Unreliable): 0 slots (saves 100% of unused TX/RX memory).
    - Channel 1 (Inputs / Critical Actions): 16 slots.
    - Channel 2 (Bulk Stream / Video Slices & Large Frames): 240 slots.
  - Zero dynamic heap allocation (`malloc`), strictly bounded session metadata, and higher in-flight capacity for selected channels. Measure descriptor size and page-pool memory separately; a zero-copy reference reduces transport-owned copies but does not make the caller's payload memory disappear.

- [ ] **Micro-Pacing & Delay-Gradient Congestion Control (SCReAM / L4S)**:
  - Line-rate and link-capacity paced transmission to eliminate transmission bursts and prevent queue bufferbloat on congested wireless or low-bandwidth links.
  - One-Way Delay (OWD) Gradient tracking: Measure transmit-to-receive delta trends ($\Delta D = D_i - D_{i-1}$).
  - Proactive rate adaptation: Detect queuing delay growth before packet loss occurs, preventing tail-latency inflation without aggressive throughput throttling.
  - Mitigate Wi-Fi and radio contention spikes with low-overhead delay tracking.

- [ ] **Sliding-Window Convolutional FEC & In-Band SACK**:
  - In-Band 64-bit SACK mask (8 bytes) packed into datagram headroom for fine-grained multi-packet loss recovery in a single RTT.
  - Sliding-Window FEC (RFC 8681): Generate parity across a moving window of in-flight segments, eliminating block-formation delay so isolated drops are recovered at 0 RTT on lossy wireless channels.
  - Adaptive Parity Ratio: 0% parity under clean link conditions, scaling dynamically (e.g., 6% to 12%) under measured loss rates.
  - Unequal Error Protection (UEP): Apply high-priority protection to stream descriptors and state headers, with lighter protection on volatile samples.

- [ ] **Intra-Refresh Low-Latency Video Slices & Edge Media Streaming**:
  - Integrate rolling Intra-Refresh slice transmission (e.g. progressive vertical column/macroblock refreshing per tick) maintaining a strictly flat bitrate without I-frame latency spikes.
  - Zero-copy pipeline for continuous video slices (H.264/HEVC/AV1 NAL units) and camera frames directly addressed by scatter-gather descriptors into decoder/render buffers.

- [ ] **Multipath Transport & Interface Diversity (Packet Spraying & RACK-TLP Recovery)**:
  - **Interface Diversity & Packet Spraying**: Multi-interface transmission (e.g. concurrent Wi-Fi + Cellular or dual radio links) with per-packet path selection for link redundancy and failover.
  - **Time-Based Loss Detection (RACK-TLP)**: Replace rigid Tri-ACK heuristics with time-based loss detection ($t_{\text{loss}} \ge \text{RTT} + \text{jitter}$) to prevent spurious retransmissions caused by packet reordering across asymmetric paths or wireless jitter.
  - **Delay-Driven Anti-Bufferbloat Pacing**: Rate pacing driven by fine-grained RTT variations, preventing queue buildup on bottleneck links during concurrent telemetry and state streams.
