# Roadmap

Completed work and remaining tasks. Phase numbers are kept for links from the
implementation notes; tooling can progress independently of protocol changes.

## Project direction: ultra-low-latency reliable transport, everything is a plugin

Build a general-purpose C11 transport for reliable, ultra-low-latency messaging,
with explicit memory ownership and replaceable capabilities, from MCU to server.
Simple to use by default, deeply customizable through public interfaces.
Application profiles are consumers of the engine, not limits on its scope.

- [ ] **Equal extension interfaces**: Official and third-party implementations
  of storage, delivery, recovery, scheduling, protection, wire formats and
  transport must use the same public contracts. No privileged internal access
  for an official implementation. Define mandatory guarantees and reject
  incompatible compositions before exchanging application data.
- [ ] **Ready-to-use default composition**: Provide a documented, tested default
  that lets applications initialize a session and send messages without manually
  assembling every capability. Keep advanced composition available through typed
  C11 descriptors and designated initializers.
- [ ] **First-class static composition**: Select and link capabilities at build
  time on embedded targets. Require no dynamic loader, hidden heap allocation,
  global lock or plugin discovery in the data path. Measure dispatch overhead;
  dynamic loading remains an optional host facility.
- [ ] **Bound the dynamic extension surface**: only three subsystems need
  replaceable callback interfaces (a light per-session vtable): transport I/O
  sink/source (socket, simulation, lwIP), packet-protection envelope (AEAD /
  Noise / DTLS / cleartext), and the external storage pool
  (`RUDP_EXTERNAL_RX_STORAGE`). Codec, ARQ and scheduling stay direct C11 in the
  core to preserve inlining, branch predictability and audit simplicity.
- [ ] **Replace recovery without forking**: Demonstrate an external recovery
  provider using only installed public headers, while preserving the selected
  delivery contract. Publish its memory, traffic and latency costs and verify
  behavior under losses, reordering, backpressure and retry exhaustion.
- [ ] **General message model with a compact TFV path**: Support bounded
  variable-length payloads without forcing every message through four-byte TFV
  records. Preserve an efficient inline path for small updates; separate message
  size, storage capacity, sequence-window bounds and path MTU.
- [ ] **Low latency under mixed load**: Test urgent messages alongside bulk
  transfers, including loss, finite bandwidth and saturated queues. Coordinate
  recovery, scheduling and congestion budgets; report completion, latency tails,
  traffic and total memory with matching delivery and security requirements.
- [ ] **First implementation milestone**: Close the existing correctness defects,
  then implement transactional egress, extract caller-owned storage and validate
  the external recovery-provider contract. Reuse the detailed tasks below rather
  than introducing parallel implementations.

Acceptance demonstration: a developer adapts the engine to another network or
hardware target through an external module, without forking the core, and can
reproduce its delivery guarantees and measured resource/performance behavior.

## Architecture target: externally composable capabilities

Design proposal: [EXTENSIBILITY.md](docs/EXTENSIBILITY.md). Official and external
modules must use the same public contracts; application profiles do not constrain
the scope of the engine. This design is not implemented by the current file split.

- [ ] Specify typed, versioned plugin contracts, lifecycle, dependency validation,
  fixed caller-owned state and an initialization-time composition builder.
- [ ] Specify message, memory-handle and packet-plan contracts independently of
  socket I/O; distinguish message, transmission-attempt and buffer identities.
- [ ] Prove transactional submission and separate protocol/I/O retirement with a
  simulated backend, then caller-owned TX/RX storage and an external provider.
- [ ] Publish a plugin SDK and conformance suite; build one plugin in a separate
  repository using only installed public headers and exported targets.
- [ ] Compare static specialization against typed runtime dispatch at packet
  boundaries; reject unbounded dispatch, mandatory waits and hidden allocations.
- [ ] Specify protected compositions: authenticate before reliability mutation,
  full-width nonce/attempt rules, replay/epoch handling, MTU accounting and
  authenticated profile negotiation. Do not silently downgrade secure profiles.
- [ ] Reconcile existing wire/recovery proposals before implementation: channel
  limits, sequence width, SACK reordering tolerance, FEC/protection order and
  delivery semantics. Buffer capacities and local plugin ABI are separate from
  wire compatibility. Dynamic loading and hot replacement remain optional,
  deferred host features with mandatory draining of callbacks and buffer loans.

## Sessions, protection and timing

Extend the existing transactional-egress, security and recovery tasks below;
keep one implementation of each contract.

- [ ] **Transactional TX and a common time contract first**: Share duration
  validation, saturating backoff, RTT sampling and attempt-count semantics between
  standalone and session paths. The `prepare -> submit -> commit/cancel` egress
  split is specified once under Cross-cutting priority ("Make egress transactional");
  keep one implementation. Distinguish backend acceptance/commit, physical egress
  when observable, and asynchronous buffer release. ACK validation must not
  acknowledge merely queued, never-committed data. Prove the contract with
  a bounded simulated backend that refuses sends and delays completions.
- [ ] **Separate session lifetime from protection and health**: Define a bounded
  active/candidate session manager, protection-provider lifecycle and independent
  health state. Model the lifecycle explicitly as CLOSED -> CANDIDATE -> CONFIRMED
  -> ACTIVE -> DRAINING -> CLOSED, with at most one active and one candidate;
  activation must be idempotent. A candidate failure or injected HELLO must not
  destroy the active session; activate a replacement only after authentication,
  freshness and explicit confirmation. Bind timers, events and I/O operations to a
  local generation. Old completions cannot mutate the new session but must still
  release their loans.
- [ ] **Define delivery outcomes across reconnection**: Report an explicit result
  such as `DELIVERY_UNKNOWN` for unresolved messages when replacing an incarnation.
  Do not assume that a missing ACK means the application never received the data.
  Transparent replay requires a separate application identity/deduplication contract.
- [ ] **Establish unreliable references explicitly**: Remove first-record baseline
  selection in the established-session profile. Distinguish `UNRELIABLE_UNORDERED`
  (delivery without retransmission, no sequence reference needed) from
  `UNRELIABLE_SEQUENCED` (newer-only delivery with an authentically established
  initial reference); the 512 bound becomes a plausibility policy, not an
  anti-injection defense. Specify behavior after more than
  `RUDP_UNRELIABLE_MAX_AHEAD` missing emissions, including authenticated resync or
  incarnation replacement, instead of leaving the channel indefinitely wedged.
  The near-term re-anchoring fix is tracked as a P0 defect below; the initial
  references belong to the authenticated `SESSION_INIT` exchange described in the
  Phase 5 wire redesign.
- [ ] **Specify sequence reuse limits**: Authentication alone does not prove safe
  reuse of 16-bit sequences. Demonstrate bounded packet age and reuse conditions,
  add an authenticated reliability epoch, or define a wider-sequence wire profile.
  Choose explicitly one profile: compact bounded 16-bit (with demonstrated
  lifetime/reuse bounds), an authenticated external reliability epoch, or a
  widened-sequence v2 wire format; for high-throughput servers, widening the
  sequences is generally preferable to multiplying handshakes to preserve 16 bits.
  Test delayed authentic ACKs across reuse, exact half-range distances and wrap.
- [ ] **Make liveness an explicit policy**: Provide `OFF`, `RX_IDLE` and `PROBED`
  modes with a documented default composition and validated configuration. Separate
  valid fresh RX, ACK progress, round-trip evidence and local service/admission gaps.
  Update RX activity automatically after complete admissibility checks; exact replay
  and malformed packets must not refresh health. Incoming traffic alone does not
  prove the outgoing direction works. Expose suspicion and terminal failure causes.
- [ ] **Bound probes and event-loop work**: Share scheduling budgets across data,
  repairs, ACKs, PING/PONG and optional keepalives. Count only committed attempts,
  never replay missed emissions after a pause, and expose the next wakeup and TX
  readiness interests without busy polling. Define timestamp validity independently
  of zero, the 32-bit time horizon, suspend/resume and optional clock width/resolution.
- [ ] **Qualify a real protection provider**: Start by evaluating a DTLS 1.3
  reference adapter behind public contracts, without making its library mandatory.
  Check exact version, licence, static-memory behavior, error paths, entropy,
  anti-replay, MTU overhead, handshake peak RAM and established latency. Noise and
  a proprietary PSK envelope remain alternatives requiring their own qualification
  and integration specification. No implicit downgrade and no security claim based
  only on a mock provider or an encrypt/decrypt interface. Acceptance criteria for
  any provider: a forged ACK causes no TX withdrawal or ACK progress; an authentic
  ACK beyond the last emission is rejected semantically; a packet from an old
  incarnation has no effect on the new one; an injected HELLO never alters the
  active session; an unauthenticated RESET is ignored; a duplicated handshake is
  idempotent; no nonce/key pair is reused (instrumented); resource use stays bounded
  under saturation; entropy failure closes explicitly with no fallback.
- [ ] **Benchmark proposed recovery policies before adoption**: Compare oldest-slot
  recovery against the existing policy under multiple losses and finite queues.
  Keep a distinct TLP deferred until it can share or replace the Phase 5 tail-loss
  probe and early-tail-copy work (one TLP specification per wire-format generation).
  Preserve the fixed-point estimator unless a measured alternative is preferable;
  report example constants, wire-cost calculations and measured results separately.
- [ ] **Complete the closure evidence**: Preserve injection/restart reproductions,
  kill the surviving M5 bounds-check mutation, automate mutation reports, and test
  stale generations, old authenticated packets, candidate failure, blocked backends,
  time wrap, long pauses and resource exhaustion. `SECURITY.md` was rewritten
  2026-10-22 (merged-file corruption fixed, false 512-bound claim corrected); keep
  API hints synchronized with the headers. Close the audit only with the
  implemented scope and integrated commit.

## Remaining correctness fixes

- [x] **Validate scatter/gather collections**: Check `len` against remaining
  capacity before addition in `rudp_iovec_add()`. In `rudp_iovec_flatten()`, validate
  count, pointers and the sum of segment lengths before copying; do not trust the
  public `total_len` field. Fixed 2026-10-22: overflow-safe addition
  (`len > 65535 - total_len`), per-segment validation and `count` bound in
  `rudp_iovec_flatten()`; regressions in `tests/test_phase4.c` (`iovec_bounds`).
- [x] **Unify retransmission arithmetic**: Saturating backoff implemented in both
  `rudp_tick()` (`src/reliability.c`) and `channel_slot_timeout_ms()`
  (`src/session_tx.c`), and retry-limit parity + disconnect signaling
  (`1 + RUDP_MAX_RETRIES`, `RUDP_ERR_DISCONNECTED` without packing dead channel
  records) verified in `tests/test_rudp.c` (`test_retry_parity_and_no_zombie_disconnect`)
  and `tests/test_phase6.c` (`test_retry_disconnect_all_apis`).
- [x] **Validate record flags before mutation**: Pass-1 validation in
  `src/session_rx.c` rejects unknown/conflicting flags and enforces channel
  `RELIABLE` vs `UNRELIABLE` capabilities before mutating session state;
  verified in `tests/test_rudp.c` (`test_strict_pass1_validation_and_ack_only_null_out`)
  and `tests/test_mutations.py`.

## P0: confirmed defects (review 2026-10-22)

Reproduced by the proof-of-concept suite (`zcrudp_poc`, ASan/UBSan) against
commit `0c5d742`; measurements quoted inline. Checked entries landed with the
2026-10-22 corrective patch and subsequent Phase 4–6 fixes.

- [x] **Relocatable shared install**: `INSTALL_RPATH` set to `$ORIGIN` /
  `@loader_path` on every module; `make test-modules` passes with
  `BUILD_SHARED_LIBS=ON`.
- [x] **C++-consumable headers**: `RUDP_STATIC_ASSERT` maps to `static_assert` in
  C++; `make test-cxx` compiles every public header with g++ (C++11/17/20) and CI
  runs g++ and clang++. Document that `-pedantic` C++ is unsupported while
  `tfv_packet_u` keeps an anonymous struct.
- [x] **Scatter/gather overflow**: see "Validate scatter/gather collections" above
  (`poc_iovec`: 4 GiB read under ASan before the fix).
- [x] **Fast-retransmit flag on an unsent slot**: the first transmission clears a
  pending fast-retransmit request (`poc_fr_unsent` produced an immediate duplicate
  with `retries = 1` before the fix).
- [x] **Unreliable re-anchoring after outages**: `RUDP_UNREL_RESYNC_STREAK` in
  `src/session_rx.c` adopts the new baseline after K self-consistent ahead
  records; verified by `test_unreliable_far_jump_resync` (`tests/test_rudp.c`).
- [x] **Fresh/gap ACK signal**: explicit `RUDP_RECORD_FLAG_ACK` record emitted when
  `RUDP_CHAN_FLAG_GAP_ACK_PENDING` is set on the primary channel (`src/session_tx.c`),
  preserving Tri-ACK on bidirectional traffic; verified in `tests/test_phase6.c`.
- [ ] **Builder reports "nothing to send"**: `rudp_session_has_pending_work()` and
  `rudp_session_next_wakeup_ms()` report idle state before building; changing
  `rudp_session_build_datagram()` to return `0` and adding a dedicated keepalive
  control kind remain deferred to wire v2.
- [x] **Retry-limit parity and signal**: standalone and session both stop after
  `1 + RUDP_MAX_RETRIES` transmissions without packing dead channel records and
  return `RUDP_ERR_DISCONNECTED`; tested across all 4 session TX APIs in
  `tests/test_phase6.c` and `tests/test_rudp.c`.
- [x] **Define `timeout == 0` in fixed mode as matching `rudp_tick(timeout = 0)`**:
  removed immediate same-timestamp retransmit overrides in `src/session_tx.c`
  so fixed mode uses `(uint32_t)(now - slot->timestamp) > limit` uniformly;
  regression test added in `tests/test_phase6.c` (`12` same-timestamp builds).
- [x] **Restore the configured initial RTO on `reset_channel`**: `rto_initial_ms`
  stored per channel and restored on `rudp_session_reset_channel()` (`src/session.c`,
  tested in `tests/test_phase4.c`).
- [x] **Count duplicate ACKs only for transmitted slots**: duplicate ACKs only count
  once `slot->tx_count > 0U` (`src/reliability.c`, tested in `tests/test_rudp.c`).
- [x] **Motion profile**: jerk computed only from `tx->samples >= 3` (`src/motion.c`),
  tested in `tests/test_phase4.c` (`motion_cold_start_and_stream_reset`).
- [x] **Entropy helper**: `rudp_calc_entropy()` mixes `session_seed`, `channel_id`,
  and `path_id` orthogonally (`include/rudp_io.h`), `rudp_session_set_seed()` is
  wired through session + multipath selection, and `test_entropy_orthogonality`
  (`tests/test_rudp.c`) verifies non-collision.
- [x] **Byte-order macros**: explicit Windows/MSVC and endianness detection in
  `include/protocol_tfv.h`, byte-layout verified in `tests/test_tfv.c` and
  `s390x` Big-Endian cross-compile (`make test-arch`).
- [ ] **Package metadata**: `project(VERSION)`, `C_VISIBILITY_PRESET hidden`, and
  `zcrudpConfigVersion.cmake` are in place (`tests/test_cmake.py`); `VERSION`/`SOVERSION`
  on shared module targets and per-module filtered header installation remain open.
- [x] **SECURITY.md rewrite**: the file interleaved two versions (duplicated
  titles, paragraphs cut mid-sentence) and claimed the 512 unreliable bound
  "prevents malicious desynchronization from muting telemetry channels", which is
  false (65 forged records mute 135 s at 240 Hz; `poc_unrel_mute`). Rewritten
  2026-10-22 as a single version with the measured limits and the four attacker
  levels.

## P1: near-term features and API additions (review 2026-10-22)

- [x] **Unreliable intra-tick bundling**: `rudp_session_queue_unreliable()` stages multiple unreliable TFV updates into a lightweight per-channel intra-tick queue (with `rudp_session_has_pending_work()` wakeup integration and rollback support on `cancel_datagram`) so `rudp_session_build_datagram()` / `prepare_datagram()` bundles multiple `UNRELIABLE` and `RELIABLE` records up to the MTU; covered in `tests/test_phase6.c`.
- [x] **Reliable unordered delivery**: `RUDP_CHANNEL_FLAG_RELIABLE` without `RUDP_CHANNEL_FLAG_ORDERED` delivers in-window records immediately with zero head-of-line blocking, deduplicating via `rx_present` and advancing `expected_seq_num` across the contiguous prefix.
- [x] **`rudp_session_next_wakeup()`**: `rudp_session_next_wakeup_ms()` returns exact ms until next RTO, TLP or RACK expiry (0 when ACK, FEC, unsent or fast-retransmit work is ready).
- [x] **Stream reset API**: `rudp_stream_tx_reset()` and `rudp_stream_rx_reset()` implemented and tested in `test_phase4.c`.

## P2: test and CI gaps

Review-specific gaps (2026-10-22); the broader tooling roadmap lives in Phase 6.

- [x] Unit tests for `io.c` (100% API coverage including zero-copy iovec spans, cursor exhaustion, validation) and coverage of the session builder's retry-limit disconnect path across all 4 TX APIs (`tests/test_phase6.c`).
- [ ] CI matrix: 32-bit (`x86`), 64-bit Big-Endian (`s390x`), and ARM64 (`aarch64`)
  cross-compiles with static asserts (`make test-arch`), C++ consumers
  (`g++`/`zig c++`), shared-install consumers (`test-modules`), ASan/UBSan,
  mutation tests (`test-mutations`), and fuzzing (`test_phase6`) run in CI;
  native macOS (clang), Windows MSVC, and line-coverage gates remain open.
- [ ] Benchmarks: bidirectional traffic, per-tick heartbeats, reset/reconnect,
  outage above 512 packets; KCP profile with explicit `ikcp_flush()` per tick and
  `ikcp_wndsize(kcp, 64, 128)` (default KCP window is 32 and `ikcp_nodelay`
  imposes a >= 10 ms flush interval, biasing the comparison).

## Cross-cutting priority: modular architecture and explicit ownership

### Acceptance criteria: ultra-low latency and reliable delivery

Ultra-low latency and reliability take precedence over modularity, copy
avoidance and throughput. A refactor or optional optimization is acceptable
only when it preserves the delivery contract and demonstrates its latency
behavior against the unchanged baseline under matching workloads and resources.

- [ ] **Define latency budgets per workload**: Specify target hardware, offered
  rate, payload sizes, RTT, loss/reordering model and application backpressure;
  set explicit p99, tail-loss recovery and maximum queue-age budgets. Report
  p50/p95/p99, observed maximum and failures across multiple seeds. Do not infer
  end-to-end latency from codec ns/op or claim a universal network guarantee.
- [ ] **Preserve reliable semantics**: For reliable channels, retain ordered,
  duplicate-free delivery within a session, bounded buffering, backpressure and
  explicit failure on retry exhaustion. Never silently discard accepted reliable
  data to improve latency or throughput. A deadline expiry must have an explicit
  failure/cancellation contract; dropping obsolete samples belongs to a separately
  selected unreliable profile. Preserve inspectable unacknowledged state on
  failure; reliability cannot guarantee delivery through a permanent outage.
- [ ] **Gate architecture changes on both properties**: Compare unchanged
  messages and delivery semantics on clean links, isolated/burst loss,
  reordering, ACK loss, one-message tails, finite bandwidth, queue saturation
  and application stalls. Set tolerances before measurement, retain failed runs,
  and reject unexplained tail-latency regressions or silent reliability loss.
  Avoid mandatory batching delays, unbounded queues and blocking waits in the
  transport fast path. Keep optional hardware paths behind the same checks.

These are design tasks, not implemented capabilities or verified performance
claims. Preserve the existing wire format during the initial refactoring.

- [x] **Establish module boundaries**: Separate byte codecs/validation, reliable
  TX/ACK/recovery, ordered RX storage, session routing/scheduling, optional
  application profiles, and platform I/O adapters. Keep portable C11,
  zero dynamic allocation, strict validation and bounded state mandatory.
  Use explicit dependencies and CMake targets; avoid per-record virtual dispatch
  or a combinatorial set of switches. Keep a simple default facade and document
  API/ABI migration before changing public structures.
- [x] **First modularization implementation**: Independent codec, reliability,
  session, I/O, stream, scalar and motion targets; compatible facade headers,
  private ordered-RX implementation, common ABI definitions, formatting rules,
  installed static/shared module consumers and minimal codec/scalar build tests.
  See [module integration](docs/MODULES.md). No new scheduling or wire behavior.
- [x] **Optional caller-owned RX storage**: `ZCRUDP_EXTERNAL_RX_STORAGE=ON`
  allows channel-specific attachment with bounded lifetimes, no shared RX object,
  atomic rejection for unattached reliable input, and reset/backpressure/wrap
  tests under ASan/UBSan. Default inline ABI remains available. TX/recovery
  arrays are still embedded; the full shared-slot-pool task remains open.
- [ ] **Decouple channel configuration from reliable storage first**: Accept
  caller-owned fixed TX/RX/recovery storage or bounded slices of a static pool,
  sized independently per channel. An unreliable-only channel should reserve no
  reliable arrays. Define allowed incoming record kinds, exhaustion,
  reconfiguration and reset rules before making buffers nullable. At the current
  64-slot default, RX payloads plus bitmap cost 264 B/channel, not 640 B;
  measure net savings including handles, alignment and recovery metadata.
  Reuse the phase-7 heterogeneous-window design rather than creating a second
  pool implementation. Preserve inline four-byte TFV storage.
- [ ] **Document single-owner execution**: One execution context mutates each
  session, including timers, RX, scheduling, reset and completion processing.
  Multiple sessions can be partitioned among workers. If cross-thread submission
  is required, provide an optional bounded SPSC adapter for exactly one producer
  and one consumer, with C11 acquire/release publication, explicit backpressure
  and a clear payload-lifetime contract. No implicit thread safety, shared-core
  locks, or mandatory atomics in the portable engine.
- [ ] **Apply borrowed-buffer principles to large payloads**: Complete the
  existing phase-7 page-loan and scatter-gather contract: immutable borrowed
  spans, generation-checked handles, bounded release events, and separate
  protocol-ACK and asynchronous-I/O completion lifetimes. Start with a flattening
  reference adapter; compare its bytes with scatter-gather output. Do not send
  raw C structs as wire bytes or remove complete validation before state mutation.
- [ ] **Make egress transactional** (canonical `prepare -> submit -> commit/cancel`
  contract; referenced by the Sessions section above and by the Phase 7 page-loan
  track): Implement the prerequisite before asynchronous backends.
  A failed or blocked submission must not consume ACK-pending state, advance
  retries, or start a send timer. Evaluate reserve/fill/commit payload publication
  only for large streams; define cancellation and outstanding-reservation limits.
- [ ] **Batch I/O and retirement with latency bounds**: Build on the demo's
  existing `sendmmsg()` path; evaluate bounded receive/completion batches in host
  adapters. Preserve partial-send handling, fairness, pacing and buffer ownership.
  Do not wait to fill a batch for latency-sensitive records. ACK aggregation
  must preserve the existing delivery/backpressure semantics.
- [ ] **Measure before selecting a fast path**: Compare copied and borrowed
  payloads at 4, 64, 256, 512 and approximately 1200 bytes. Count copied bytes,
  syscalls, retained payload memory, descriptors, CPU and p99 under finite queues,
  loss, reordering and application stalls. Test reset with late I/O completion,
  stale handles, pool exhaustion and cancellation. Validate representative builds
  (unreliable-only, reliable ordered, mixed, optional profiles) against the current
  behavior and document exact RAM on 32- and 64-bit targets.

Keep RDMA, GPU/NIC integration, NUMA/queue steering and kernel bypass in optional
research adapters with measured prerequisites. Do not add seqlocks, hazard
pointers, epoch reclamation, TSX or universal cache-line padding to solve an
unmeasured concurrency problem in the single-owner core. Hardware-specific
estimates are not zcrudp benchmarks; external-memory export alone does not
establish NIC DMA support or end-to-end zero-copy. See the phase-7 tasks for
related work.

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
  - Compile public headers as C++ in CI: `RUDP_STATIC_ASSERT` maps to `static_assert` in C++; `make test-cxx` builds every public header with g++ (C++11/17/20); CI runs g++ and clang++ (added 2026-10-22). `-pedantic` C++ remains unsupported while `tfv_packet_u` uses an anonymous struct.
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
  - Re-sync `ARCHITECTURE.md` with 1040-byte context layout, 16-byte slot diagram, modular serializers, and dead peer state machine.
  - Add `.clang-format` configuration for consistent code styling across C and C++ integrations.
  - Add `SECURITY.md` stating cleartext transport notice and recommending DTLS for untrusted networks.

---

## Phase 4: Receive buffering and channel profiles
*Depends on Phase 3 working bidirectional engine.*

- [x] **RX Out-of-Order Reassembly Buffer**: Bounded payload slots + bitmap, wrap-safe ordered drain and explicit application backpressure via `rudp_session_poll()`. Selective storage with cumulative ACKs, not selective ACK signaling; see `docs/PHASE4.md`.
- [x] **Reliable paced-loss completion regression**: All five seeds of both 2,400-message `240hz-loss1` and `240hz-loss5` scenarios complete with unchanged workload, timeout, backoff and retry limit. Regression in `tests/test_comparison.py`; baseline data preserved in `docs/bench/comparison/`.
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
  - [x] **Optional Unreliable Scalar Profiles** (`protocol_profiles.h`, independent stream/scalar/motion sources; explicitly bound, never length-dispatched against ACKs):
    - [x] **Track A (Compact4)**: seq16 + scalar16; type/flags belong to the out-of-band profile, not an unchanged TFV payload.
    - [x] **Track B (Rolling8)**: seq16 + current16 + previous delta16 + control16. Recovers the previous sample when the next datagram arrives, without a retransmission round trip; not zero elapsed time.
    - [x] **Track C (Adaptive Redundancy)**: Fixed-cadence scalar acceleration/jerk threshold crossings produce one or two identical Rolling8 datagrams for caller emission; no ACK wait, receiver deduplication. Benefit under correlated loss is not guaranteed or benchmarked against other libraries.
  - [x] **Egress Scheduler & MTU Packing (Intra-Tick Bundler)**: Batch intra-tick payloads and multi-channel ACKs into MTU-sized UDP datagrams without cross-tick delay (Anti-Nagle Principle).

---

## Phase 5: Recovery, security and platform support
*Depends on Phase 4.*

- [x] **Unified Scatter-Gather I/O Layer (Zero-Copy Universal I/O)**:
  - Portable OS-agnostic vectored I/O API (`rudp_iovec_s`, `rudp_iovec_collection_s`) wired directly to session egress via `rudp_session_build_iovec()` and transactional `rudp_session_prepare_iovec()` (`include/rudp_session.h`, `src/session_tx.c`).
  - Propagates lead-channel `dscp` and collision-free `path_entropy` (`rudp_calc_entropy`) for multi-path UDP port diversity without `malloc`.
- [x] **Modular Cryptographic Layer (Noise / WireGuard - External Wrapper)**:
  - Standalone `zcrudp::crypto` module (`include/rudp_crypto.h`, `src/crypto.c`) keeping `zcrudp` 100% zero-dependency and zero-malloc.
  - Constant-time RFC 8439 ChaCha20-Poly1305 IETF AEAD + directional key derivation (`is_initiator` reflection defense) + 64-packet sliding anti-replay window + pluggable hardware/OS AEAD vtable (`rudp_aead_ops_s`) + volatile secret wipe (`rudp_crypto_wipe`).
  - Authenticates the entire UDP datagram before any RUDP state mutation (`--crypto 1` in `demo_loss`).
- [x] **Adaptive RTT & Dynamic Timeout**: Opt-in session recovery, timestamped receive API, fixed-point SRTT/RTTVAR, conservative Karn sampling at most once per RTT, configurable base-RTO bounds, and timeout-only backoff separate from fast repairs. Unchanged wire format and TX slots. See `docs/ADAPTIVE_RECOVERY.md`; `make test-recovery-stress` covers 200 paced-loss runs.
- [ ] **Breaking core wire/recovery redesign (single format, no legacy-wire compatibility)**:
  Replace the current session-only timing policy with one bounded recovery engine used directly by `rudp_context_s` and wrapped by `rudp_session_s`. This is an intentional wire break: both endpoints must use the new format; do not add downgrade negotiation or preserve decoding of the previous format.
  - **Preserve the clean data format**: Keep full 16-bit `seq_num` and `ack` values and keep normal frames/records at 8 bytes. Do not steal sequence bits, payload bits, or TFV type values. Keep the existing 16-bit serial arithmetic and the `65535 -> 0` behavior. Encode control metadata with masks and shifts, never C bitfields.
  - **Compact homogeneous runs in session datagrams**: Avoid paying a four-byte record header for every four-byte payload when several consecutive records share a channel and kind. Keep the four-byte datagram header, then encode each run as `{base_seq16, channel_kind8, item_count8}` followed by `item_count` raw four-byte TFV payloads; sequence numbers after `base_seq` are implicit. A datagram therefore costs `4 + ack_prefix_bytes + sum(4 + 4*N_run)` instead of `4 + 8*N`. One isolated data record remains 12 bytes, two same-run records fall from 20 to 16 bytes, and ten fall from 84 to 48 bytes when no extra channel ACK is present. Permit multiple runs for mixed channels, precede them with the compact ACK prefix described below, and keep FEC source and repair runs in independent UDP loss domains. Require strict length validation, bounded run counts, wrap-safe implicit sequence expansion, and no cross-tick batching delay.
  - **Dedicated 4-byte control exchange**: Reuse the otherwise-unused `seq_num` word of standalone 4-byte ACKs. Packet length already distinguishes a control header from an 8-byte data frame. Define its low byte as a packed control byte with the channel in bits 0..2 (up to 8 channels; if `RUDP_MAX_CHANNELS` must exceed 8, dedicate a full byte to the channel in control frames), kind in bits 3..5 (`ACK`, `RTT_PROBE`, `RTT_REPLY`, `RECOVERY_ACK`, `HELLO`, `HELLO_ACK`, `RESET`), and a two-bit control epoch in bits 6..7; keep the high byte reserved and zero. Do not hard-cap the wire format at 4 channels: inputs + state + stream + telemetry + audio/VoIP exceed 4 quickly. In the session header, carry the same control byte in `ack_channel` when `count == 0`; data datagrams keep kind/epoch zero. Normal ACK/probe request/reply controls continue to carry the full 16-bit cumulative ACK.
  - **Compact ACK prefix for ACK-only and mixed datagrams**: Let the four-byte header acknowledge the primary channel, then place every additional pending channel ACK before the data runs as four bytes `{channel_id8, ack_flags8, ack16}` instead of the current eight-byte `rudp_record_s` with an unused TFV payload. Pack the last header byte as `ack_count` (2 bits, 0..3) plus `run_count` (6 bits, 0..63); make the builder split a datagram before the run count overflows and test the bound against worst-case channel/kind alternation. Validate exactly the ACK prefix first and then every declared run before mutating state. Four simultaneous channel ACKs cost 16 bytes (`4 + 3*4`) instead of 28 (`4 + 3*8`), while a mixed datagram pays the same compact prefix and no additional UDP/IP packet. Do not force an odd entry count or emit padding ACKs: UDP accepts arbitrary lengths, bytewise codecs do not benefit from total-length alignment, and only genuinely pending entries may count as deliberate duplicate ACKs for Tri-ACK. Reserve ACK space before scheduling data, as the current builder already does. Reserve a fresh/gap ACK indication for the primary ACK (control byte when `count == 0`, `ack_flags8` in the compact prefix for mixed datagrams) so duplicate-ACK (Tri-ACK) counting no longer depends on `count == 0`; see the P0 fresh/gap ACK defect.
  - **RTT samples come only from probes**: At the first reliable transmission, emit one standalone `RTT_PROBE`; the receiver immediately returns `RTT_REPLY` with the same epoch and its current cumulative ACK. Store the local probe-send time and accept a sample only for the matching outstanding epoch. Data ACKs advance reliability but never train SRTT, so retransmission ambiguity, cumulative jumps, RX buffering, and FEC reconstruction cannot contaminate the estimator. Later probes are sent only when the estimate is stale or recovery indicates a possible path change, with at most one outstanding probe and no probe retransmission loop.
  - **Audits #1/#2/#6: restart-safe establishment and first-unreliable anchoring without permanent header cost**: Add a bounded per-channel `HELLO`/`HELLO_ACK` state machine before accepting data. For these control kinds, reinterpret the 16-bit ACK word as a caller-seeded random initial sequence number; each direction installs the peer's fresh reliable and unreliable sequence anchor and resets old TX/RX/FEC/probe state atomically. Never let the first unreliable record establish an arbitrary receive baseline: it must fall within `RUDP_UNRELIABLE_MAX_AHEAD` of the sequence negotiated for that channel. Reject normal data and ACK controls until establishment completes, rate-limit `HELLO`/`RESET`, and test simultaneous open, one-sided restart, an arbitrary first unreliable sequence, blind ACK/data injection before establishment, lost handshake controls, delayed packets from the prior epoch, and sequence wrap. This fixes restart wedging and first-record baseline poisoning, and makes blind sequence guessing harder without enlarging normal frames. A 16-bit random anchor is only probabilistic hardening (at most 65,536 possibilities), not authentication: cleartext deployments remain vulnerable to an observer or accurate injector and still require the documented DTLS/WireGuard/AEAD wrapper on untrusted networks.
  - **Core-owned adaptive state**: Move `srtt_scaled`, `rttvar_scaled`, `rto_ms`, `rto_min_ms`, `rto_max_ms`, and `last_rtt_sample` from `rudp_channel_s` into `rudp_context_s`; make session recovery call the core implementation instead of maintaining a second timer path. The expected default standalone context cost is +24 bytes (1040 -> 1064), while the session should not duplicate those fields.
  - **Recover the timeout-counter RAM**: Store the retransmission count and timeout-backoff exponent in the low/high nibbles of the existing slot retry byte. Add masked accessors and compile-time bounds (`RUDP_MAX_RETRIES <= 15`, backoff exponent <= 15); remove `timeout_backoffs[RUDP_WINDOW_SIZE]`. With a 64-slot window and four channels, target approximately 1340 bytes/channel and 5364 bytes/session before any explicit FEC decoder state. Confirm every size with `_Static_assert` rather than relying on these estimates.
  - **Exactly one early tail copy**: While the initial RTT probe is outstanding, continue sending new data and allow one copy of the oldest unacknowledged slot at a configurable 100--150 ms delay. This copy is loss exploration, not an RTT sample. Once the probe reply seeds SRTT/RTTVAR, use the adaptive timer; a separate conservative RTO remains the final recovery timer. The wire-compatible precursor for the current format is the Phase 5 tail-loss probe below.
  - **Audit #7: automatic liveness on every valid receive path**: Update `last_rx_time` only after complete wire validation for standalone controls, core frames, session headers, and every addressed channel in a multi-channel datagram. Reuse the rate-limited RTT probe as keepalive traffic when idle. Expose session-level health without requiring applications to remember a separate `rudp_touch()` call, and test idle, unreliable-only, one-way, malformed-input, and clock-wrap cases.
  - **Observable rejection counters**: Add bounded saturating counters (or an optional caller-owned statistics sink) for stale/future ACKs, implausible unreliable jumps, malformed datagrams, out-of-window reliable records, FEC failures, probe timeouts, and rate-limited controls. Rejected unreliable jumps must remain observable instead of being indistinguishable from an idle poll; preserve `RUDP_UNRELIABLE_MAX_AHEAD` as a configurable plausibility bound.
  - **Probe and repair budget**: Share one per-context pacing budget across original data, ACKs, RTT controls, tail copies, retransmissions, and parity. At most one RTT probe and one early tail copy may be outstanding; neither can recursively arm another. New application data continues during calibration, while parity and speculative repairs must yield when the finite-rate link is already saturated. Define epoch wrap, delayed replies, reset, clock wrap, ACK loss, reordering, application stalls, and retry-limit transitions before implementation.
  - **Cross-tier conformance tests**: Feed identical traces through the standalone context and session wrapper and require identical sequence advancement, RTT samples, RTO updates, retransmission choices, and disconnect decisions. First case to write: the retry-limit gap (standalone stops after 1 + `RUDP_MAX_RETRIES` transmissions, the session builder after one more; see P0). Fuzz the packed codec separately from the state machine, then run finite-bandwidth/queue benchmarks reporting completion, useful bytes, total wire bytes, p99 and worst-case latency, CPU, and exact static RAM.
- [ ] **Revisit recovery memory / traffic / latency tradeoffs (explicitly deferred)**: Preserve the measured adaptive-recovery baseline (5,620 B/session, +352 B versus phase 4; about +9% UDP-payload traffic at 5% loss, p99 median 57 ms versus 263 ms). Investigate packed timeout counters, optional recovery-state storage, shared RTT estimation only where path/queue semantics permit it, and selective ACK extensions that suppress redundant retransmissions. Target retaining the latency improvement while reducing added RAM and returning toward phase-4 traffic; these are experimental objectives, not guarantees. Validate with unchanged baseline workloads plus finite-rate links, bounded queues, reordering, correlated loss and application stalls before claiming a simultaneous improvement.
- [x] **Close residual audit and public-API drift**:
  - [x] `RUDP_CHANNEL_FLAG_ORDERED` enforced for zero-HoL unordered vs ordered delivery (`src/rx.c`, `tests/test_phase5.c`), `RUDP_CHANNEL_HINT_EXTERNAL_AEAD` added in `include/rudp_config.h`, and reliable sends on `UNRELIABLE` channels rejected.
  - [x] Remove the duplicated/stale Doxygen `@return` lines, document the actual public return values and current inert hints, and synchronize `ARCHITECTURE.md` with the checked structures and state machine.
  - [x] Explicit Windows/MSVC and endianness detection added to byte-order helpers in `include/protocol_tfv.h` (retained under `RUDP_` public scope for TFV callers/tests).
  - [x] Mutation-test job (`make test-mutations` / `tests/test_mutations.py`) running in CI to catch parser bounds, stale/future ACK, reset, capability, and length regressions.
- [x] **Estimable / Dead-Reckoning Classification**: Kinematic client-side predictor (`rudp_motion_dr_s` in `zcrudp::motion`: `HOLD`, `LINEAR`, damped `MANEUVER` quadratic extrapolation with bounded horizon clamp and clone deduplication).
- [x] **Transport recovery roadmap (same messages and delivery guarantees as ENet / KCP-fast)**:
  - [x] **Selective ACKs**: 32-bit wire-compatible SACK bitmap in explicit `RUDP_RECORD_FLAG_ACK` records, wrap-safe validation rejecting unsent/out-of-window bits, and RFC 6675 FACK 1-RTT multi-loss repair without Go-Back-N.
  - [x] **Reordering-tolerant time-based loss detection**: Strict Karn SACK RTT sampling + RFC 8985 RACK reorder-window timer for lost fast-repairs confirmed by >= 3 SACKed packets above the hole.
  - [x] **Tail-loss probe**: RFC 8985 TLP firing once at `1.5 * SRTT + 2 ms` on application-limited tail packets (`< 3` un-SACKed packets above) without inflating exponential backoff.
  - [x] **Finite-rate comparison gate**: Deterministic finite-bandwidth (2000 kbps) and bounded-queue (64 pkts) virtual-link comparison gate added in `tests/test_comparison.py`, preserving the historical unlimited-link baseline.
- [x] **Optional XOR K=2 for rare independent loss**: `RUDP_CHANNEL_FLAG_FEC_K2` + `RUDP_RECORD_FLAG_FEC_PARITY` zero-RTT single-loss recovery keeping pair members and parity in separate UDP datagrams, with transactional rollback support and telemetry counters (`rudp_stats_s`).

---

## Phase 6: Tests and tooling

- [x] **Extended Test Suite**: Existing `tests/test_rudp.c`, `tests/test_phase4.c` and `tests/test_window.c` cover wraparound, out-of-order delivery, malformed/future ACK handling, output backpressure and retry-limit disconnection. Revalidated through Make and Release CTest; broader fuzzing and adverse-network coverage remain separate work.
- [x] **Interactive CLI Demo (`examples/demo_loss.c`)**: POSIX two-peer UDP demo with reliable/unreliable commands, configurable loss/latency/jitter, seeded simulation, bounded delay queue, automatic traffic and integration tests (`make demo`, `make test-tools`).
  - [x] **Sub-tick poll during bursts**: The event loop polls with `timeout = 0` while a burst is outstanding (auto-generation running, reliable slots unacked, or datagrams queued) so measured latency is not quantised to the OS scheduler tick; it blocks up to 5 ms only when idle. Removes the 0..5 ms phase offset that previously measured scheduler latency rather than transport latency.
  - [x] **Vectored egress (Target A pattern)**: `flush()` sorts the due datagrams by `(due, seq)` and, on Linux, hands the whole intra-tick burst to the kernel in one `sendmmsg()` call instead of looping on `sendto()`; other POSIX hosts keep the `sendto()` loop. A blocked socket or short send stops the drain and preserves cross-tick ordering. `tests/test_demo_queue.c` hooks `sendmmsg()` accordingly.
- [x] **Visual protocol replay**: Real-engine deterministic trace, offline HTML/SVG replay with seek/pause controls and reduced-motion support, with optional locally generated GIF output (`make visual-trace`, `make visual-report`). Scripted losses/reordering are labeled as explanatory, not comparative performance measurements.
- [x] **Benchmarking Suite (`bench/bench_rudp.c`)**: Codec throughput (Mops/s, Mpps for single frames/records) and amortized cost (ns/op), repeated samples and reproducible CSV reports and optional local SVG output (`make bench`, `make bench-report`).
- [x] **Competitive transport benchmark (`bench/compare_transport.c`)**: Run the actual zcrudp, ENet and KCP engines (default and fast profiles) through the same virtual datagram link. Nine scenarios, five seeds, 2,400 ordered 4-byte messages per run; measured goodput, p50/p95/p99 delay, emitted bytes and host simulation cost. Pinned/checksummed dependencies, CSV, environment metadata and comparative graphs (`make compare`, `make test-compare`). Failed runs remain visible.
- [x] **Transport-capacity fairness matrix**: Preserves the 63-message admission cap as the matched-capacity baseline (`ZCRUDP_ADMISSION_CAP` in `bench/compare_transport.c`) and adds deterministic admission-cap sweep (`ZCRUDP_ADMISSION_CAP=32`, etc.) and capacity checks in `tests/test_comparison.py`.
- [x] **Physical network comparison & Netem harness (`bench/netem_harness.py`, `make test-netem`)**:
  - Real-socket UDP loopback (`zcrudp` via `bindings/python/zcrudp.py`) adverse link emulator implementing Gilbert-Elliott burst loss, Pareto delay/jitter, and asymmetric packet reordering, plus reproducible Linux `tc qdisc netem` commands (`--print-netem`).
  - Measures empirical CDFs of latency (`p50`, `p95`, `p99`, `max`), goodput, process CPU time, and C heap allocation accounting (`0` for `zcrudp`) against a real-socket `TCP_NODELAY` loopback baseline (`make test-netem` smoke test in CI; live real-socket `ENet` comparison remains external to the virtual-link suite in `bench/compare_transport.c`).
- [x] **Broader workloads & Payload Scalability**: Validated in `tests/test_phase6.c` across `4B`, `64B`, `256B`, `512B`, and `1200B` `rudp_stream` payloads, WAN BDP stress (`100 ms` RTT at `120 Hz`), and mixed reliable/unreliable channel bundling.
- [x] **Fuzzing & Invariant Verification (`libFuzzer` / `AFL++`)**: `tests/fuzz_rudp.c` provides `LLVMFuzzerTestOneInput` / `rudp_fuzz_one_input` verifying Pass-1 RX atomicity, ring bounds, transactional `prepare`/`cancel` rollback, codec roundtrips, and ChaCha20-Poly1305 AEAD tamper rejection (with 5,000 deterministic iterations run in `tests/test_phase6.c` under ASan/UBSan).
- [x] **CMake Integration (`CMakeLists.txt`)**: Static/shared core and optional profiles, CTest with active Release assertions, POSIX tool opt-in, ABI settings propagated to consumers, `add_subdirectory` targets and relocatable `find_package` installation. `make test-cmake` validates package/embedded consumers and custom configuration; Linux static/shared tested, individual game-engine integrations not certified.
- [x] **One-Command Multi-Language Bindings (Python, Node/Bun, Rust, Go, C++)**:
  - `make bindings` builds `libzcrudp.so`, and `make test-bindings` validates the C++11/17/20 RAII header (`bindings/cpp/zcrudp.hpp`), Python `ctypes` wrapper (`bindings/python/zcrudp.py`), Rust FFI crate wrapper (`bindings/rust/zcrudp.rs`), Go `cgo` wrapper (`bindings/go/zcrudp.go`), and Node.js ESM FFI binding (`bindings/node/zcrudp.mjs`).
- [x] **CI/CD & Memory Sanity**: GitHub Actions workflow with AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan) verifying 0 memory leaks and 0 undefined behaviors.

---

## Phase 7: Research tracks & next-gen architecture

Exploratory tracks for zero-copy streaming, asymmetric channel window partitioning, delay-gradient micro-pacing, and resilient multi-interface transport. The low-latency video-slicing and Vulkan GPU-NIC pipeline tracks moved to [docs/RESEARCH_MEDIA_GPU.md](docs/RESEARCH_MEDIA_GPU.md) (companion `zcrudp-media` project).

- [x] **Copy-Avoiding Page Streaming & Portable Scatter-Gather (`zcrudp::nextgen`)**:
  Implemented in `include/rudp_nextgen.h` and `src/nextgen.c` (validated in `tests/test_phase7.c`): caller-owned registered page pool (`rudp_page_pool_s`), 12-byte generation-checked extents (`rudp_page_extent_s`, `RUDP_STATIC_ASSERT(sizeof == 12)`), dual `proto_refs`/`io_refs` counting with bounded coalesced release queue (`rudp_page_pool_drain_releases`), portable `rudp_packet_plan_s`, reference byte-buffer flattening (`rudp_packet_plan_flatten`), and portable scatter-gather conversion (`rudp_packet_plan_to_iovec`).

- [x] **Heterogeneous Per-Channel Windows via Shared Static Metadata Slot Pool (`zcrudp::nextgen`)**:
  Implemented in `rudp_shared_pool_s` (`RUDP_SHARED_POOL_SIZE == 256` total static slots) with per-channel bounded reservation quotas (`rudp_shared_quota_s` via `rudp_shared_pool_set_quota`), storing inline TFV payloads or 12-byte page extents and releasing `proto_refs` automatically on cumulative ACK or SACK bitmap advancement (`rudp_shared_pool_ack`), with zero dynamic allocation (`malloc`).

- [x] **Micro-Pacing & Delay-Gradient Congestion Control (SCReAM / L4S)**:
  Implemented in `rudp_pacer_s`: One-Way Delay (OWD) baseline & smoothed delta tracking + L4S ECN feedback with proactive rate scaling before loss and sub-ms transmission pacing credits (`rudp_pacer_on_owd_sample`, `rudp_pacer_refill_and_can_send`, `rudp_pacer_consume`, `rudp_pacer_next_wait_us`).

- [x] **Sliding-Window Convolutional FEC (RFC 8681)**:
  Implemented in `rudp_sliding_fec_s` / `rudp_sliding_fec_rx_s`: zero-RTT sliding-window XOR parity generation over in-flight sequences (`rudp_sliding_fec_encode`), single-loss XOR recovery (`rudp_sliding_fec_rx_repair`), and dynamic adaptive parity scaling (`rudp_sliding_fec_update_loss`).

- [ ] **Intra-Refresh Low-Latency Video Slices & Edge Media Streaming** and
  **Heterogeneous GPU-NIC Direct Pipeline (Vulkan Acceleration & Zero-Host-Copy)**:
  moved to [docs/RESEARCH_MEDIA_GPU.md](docs/RESEARCH_MEDIA_GPU.md). These are
  companion research tracks (`zcrudp-media`), not core C11 transport work; keep
  `TODO.md` focused on the transport engine.

- [x] **Multipath Transport & Interface Diversity (`rudp_multipath_s`)**:
  Implemented `RUDP_MP_PRIMARY_FAILOVER`, `RUDP_MP_WEIGHTED_SPRAY`, and `RUDP_MP_REDUNDANT` path scheduling across up to `RUDP_MAX_PATHS` interfaces with RTT/loss path health tracking and deterministic ECMP flow entropy (`rudp_multipath_add_path`, `rudp_multipath_on_feedback`, `rudp_multipath_select_paths`, `rudp_calc_entropy`).
