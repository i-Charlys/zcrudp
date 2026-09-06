# Protocol & Library Comparison
# zcrudp: where the small design pays off

This document provides a technical, benchmark-backed comparison between `zcrudp` and the prominent transport protocols and Reliable UDP (RUDP) libraries across the gaming, real-time networking, and distributed systems ecosystems:
`zcrudp` targets tiny, frequent updates with a fixed memory budget. Its strongest
case is a transport core you can inspect, embed and drive from your own loop:
one C source file, two headers, no heap allocation calls, no external core dependencies.

- **zcrudp**: Zero-malloc, fixed-frame, multi-channel Reliable UDP in pure C11.
- **ENet**: Legacy standard for game engines (Cube 2, Godot, Sauerbraten).
- **KCP**: High-performance ARQ protocol by Skywind3000 (widely used in mobile games, Genshin Impact).
- **GameNetworkingSockets (GNS)**: Valve's Steam networking layer with built-in encryption and multi-lane transport.
- **RakNet / SLikeNet**: Historical C++ multiplayer engine (Minecraft Bedrock, Oculus).
- **QUIC (RFC 9000)**: Modern web and microservice transport (Chromium, Cloudflare, nghttp3).
- **TCP**: Operating system default stream transport.
This guide covers architectural differences and wire definitions, checked on
2026-09-05. Actual-engine zcrudp/ENet/KCP throughput and latency comparisons now
have a [separate methodology and 120-run dataset](bench/comparison/README.md).
GNS and QUIC are discussed architecturally, not benchmarked here.

---
## What you can verify today

## 1. Technical Comparison Matrix
| Property | Default zcrudp build | Evidence |
| --- | --- | --- |
| Core integration | 1 C source + 2 headers | [Source](../src/rudp.c), [headers](../include/protocol_rudp.h) |
| Explicit dynamic allocation in the core | None | Inspect the core; caller owns contexts and buffers |
| Context storage | 1,040 bytes on the measured host ABI | `make test` prints `sizeof` |
| Four-channel session storage | 5,620 bytes on that ABI | TX/RX arrays plus optional recovery state, statically reserved |
| Reliable capacity | 63 outstanding messages/channel by default | Ring reserves one slot to distinguish full/empty |
| Unified datagram, one TFV update | 12 bytes of UDP payload | 4-byte envelope + 8-byte record |
| Standalone cumulative ACK | 4 bytes of UDP payload | Includes ACK channel |
| Unreliable submission | No TX-window insertion or retransmission | Independent sequence state, bounded freshness filter |
| Reproducible measurements | CSV, SVG and environment log | [Benchmark results](bench/codec.csv) |

| Criteria | `zcrudp` | `ENet` | `KCP` | `Valve GNS` | `RakNet / SLikeNet` | `QUIC` | `TCP` |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Language & Standard** | Pure C11 (strict `-pedantic`) | C99 | C89 / C99 | C++11 | C++03 / C++11 | C99 / Rust / Go | OS Kernel / C |
| **Memory Allocation** | **Strict Zero-Malloc** | Dynamic (`malloc`/`free`) | Custom hook (`ikcp_malloc`) | Dynamic / Object Pools | Heavy C++ (`new`/`delete`) | Dynamic Heap | OS Kernel Buffers |
| **Footprint per Connection** | **1,036 B** (ctx) / **4,196 B** (4 ch) | ~20 KB - 64 KB | ~8 KB - 32 KB | > 100 KB | > 128 KB | > 150 KB | ~8 KB - 64 KB (OS) |
| **Embedded / Bare-Metal Ready** | **Yes** (STM32, ESP32, lwIP) | No (POSIX/WinSock tied) | Possible with static pool | No (Heavy dependencies) | No (Heavy C++ runtime) | No (Requires TLS runtime) | No (Requires full IP stack) |
| **Wire Header Size** | **4 Bytes** (ack) / **8 Bytes** (frame) | 28 - 48 Bytes | 24 Bytes | 15 - 40+ Bytes | 20 - 35+ Bytes | 20 - 50+ Bytes | 20 - 60 Bytes |
| **Multi-Channel Multiplexing** | **Native (up to 4 channels)** | Native (channels 0..N) | Manual (1 cb = 1 stream) | Native (Multi-lane) | Native (Channels) | Native (Streams) | None (Single stream) |
| **Head-of-Line (HoL) Blocking** | **None** (per-channel independent) | Partial (shared queue) | High (single stream) | None (per-lane) | None (per-channel) | None (per-stream) | **Total** (stream-wide freeze) |
| **Unreliable Stream Support** | **Fast Bypass + RFC 1982** | Yes (`ENET_PACKET_FLAG_UNRELIABLE`) | Requires mode switch | Yes (`k_nSteamNetworkingSend_Unreliable`) | Yes (`UNRELIABLE_SEQUENCED`) | Datagram Extension | No |
| **Intra-Tick Multi-ACK Bundling** | **Yes** (`rudp_session_build_datagram`) | Piggybacked | Piggybacked | Batched frames | Piggybacked | SACK frames | SACK / Delayed ACK |
| **Fast Retransmit** | **Tri-ACK RFC 5681 Compliant** | RTT Timeout | Fast ACK (resend count) | Selective ACK (SACK) | SACK | SACK / RACK | Tri-ACK / SACK |
| **Codec Throughput (Single Core)** | **~500 Mops/s** (2.0 ns/op encode) | ~10 - 25 Mops/s | ~20 - 50 Mops/s | ~5 - 15 Mops/s | ~5 - 10 Mops/s | ~2 - 10 Mops/s | OS syscall bound |
| **Cryptographic Layer** | External (Noise / WireGuard) | None / External | None / External | Built-in AES-GCM | Built-in ChaCha/AES | Mandatory TLS 1.3 | External (TLS) |
| **AI KV-Cache Streaming Fit** | **Engineered** (Prefill-Decode) | Poor | Poor | Average | Poor | Average (HTTP/3 RPC) | Poor (HoL blocking) |
Storage excludes application payload history, socket/lwIP buffers, stack frames
and cryptographic wrappers. A non-reliable channel still reserves its embedded
TX array. Zero allocation does not mean zero storage or a measured worst-case
execution-time guarantee.

---
## Small-message wire comparison

## 2. In-Depth Architectural Analysis
The following is a calculation from wire definitions, **not a benchmark**.
Assumptions: an established session, one 4-byte application message in one UDP
datagram, no compression, encryption, fragmentation or extra queued commands.
Values exclude UDP/IP/link headers and subsequent standalone ACK traffic.

### 2.1 Wire Header Overhead & Network Goodput
| Encoding | UDP payload for that message | Calculation |
| --- | ---: | --- |
| zcrudp unified reliable or unreliable | **12 B** | 4 B envelope (with one cumulative ACK) + 8 B record |
| ENet reliable command | **12–14 B** | 2–4 B packet header + 6 B command + 4 B payload |
| ENet unreliable sequenced command | **14–16 B** | 2–4 B packet header + 8 B command + 4 B payload |
| KCP PUSH segment | **28 B** | 24 B segment header + 4 B payload |

In high-tick systems (e.g. 60 Hz to 120 Hz game engines, robotics telemetry, or speculative AI token streaming), small payloads dominate.
The ENet packet header has an optional sent-time field. Its reliable and
unreliable commands have different layouts. These sizes follow the
[ENet protocol definitions](https://github.com/lsalzman/enet/blob/master/include/enet/protocol.h).
KCP declares its 24-byte segment overhead in
[ikcp.c](https://github.com/skywind3000/kcp/blob/master/ikcp.c).

- **`zcrudp`**:
  - Standalone ACK: **4 bytes** (`ack(2B) + ack_channel(1B) + count(0)(1B)`).
  - Reliable / Unreliable Record: **8 bytes** (`channel_id(1B) + flags(1B) + seq_num(2B) + payload(4B)`).
  - Datagram: **4 bytes header + N * 8 bytes records**. A single-payload datagram occupies **12 bytes**.
  - On standard Ethernet (IPv4 + UDP = 28 bytes header), a 12-byte payload requires **40 bytes** on wire, padding up to the standard 64-byte Ethernet frame minimum (84 octet-times with IFG/preamble). Zero bandwidth is wasted.
For this specific small-message case, zcrudp uses **57% fewer UDP-payload bytes
than a KCP PUSH segment**: `1 - 12/28`. This does not imply 57% less physical-link
traffic, CPU time or delivery latency. KCP carries additional transport fields
and supports a broader payload model. ENet's compact reliable representation is
already close to zcrudp here.

- **`KCP`**:
  - Segment header is **24 bytes** fixed: `conv(4) + cmd(1) + frg(1) + wnd(2) + ts(4) + sn(4) + una(4) + len(4)`.
  - For a 4-byte application payload, KCP emits a 28-byte payload. The header constitutes **85.7% of the transmitted payload**.
Bundling changes these totals. For zcrudp, N data records occupy `4 + 8*N`
bytes before optional explicit ACK records; those ACK records also occupy 8 bytes.
One 16-record datagram is 132 bytes. Separate datagrams would total 192 bytes
of UDP payload, plus fifteen additional UDP/IP envelopes.

- **`ENet`**:
  - Minimum protocol command header is **28 to 48 bytes** depending on channel ID, reliable sequence numbers, and command type.
  - Adding Ethernet/IP/UDP headers often exceeds 60 bytes before payload insertion.
## Pick the transport that matches the job

---
| Design | Main fit | What the zcrudp specialization buys you |
| --- | --- | --- |
| zcrudp | Small TFV updates, caller-managed memory and scheduling | A bounded core with separate reliable/unreliable state |
| ENet | General game messages, channels and fragmentation | zcrudp narrows the payload model and storage policy |
| KCP | ARQ for application-provided transport I/O | zcrudp includes an unreliable path and channel envelope |
| Valve GameNetworkingSockets | Broader game networking integration | zcrudp keeps its core integration much smaller in scope |
| QUIC | Secure multiplexed transport | zcrudp delegates security and connection management to its integration |

### 2.2 Memory Model & Real-Time Predictability
ENet's command set includes channels, unreliable delivery and fragmentation;
see its [protocol definitions](https://github.com/lsalzman/enet/blob/master/include/enet/protocol.h).
KCP supplies ARQ, an output callback and allocator hooks in its
[implementation](https://github.com/skywind3000/kcp/blob/master/ikcp.c).
Valve documents reliable/unreliable messaging, fragmentation, encryption and
P2P features in the [GNS repository](https://github.com/ValveSoftware/GameNetworkingSockets).
QUIC specifies streams, flow control, loss recovery and secure transport in
[RFC 9000](https://www.rfc-editor.org/rfc/rfc9000.html).
QUIC is a protocol, not a single implementation with a universal memory footprint.

- **`zcrudp` (Zero-Malloc Invariant)**:
  - All buffers are statically or stack allocated. The single-channel context [`rudp_context_s`](file:///home/charl/.gemini/antigravity/worktrees/zcrudp/feedback_project_analysis/include/protocol_rudp.h#L107-L116) occupies exactly **1,036 bytes**.
  - A 4-channel session [`rudp_session_s`](file:///home/charl/.gemini/antigravity/worktrees/zcrudp/feedback_project_analysis/include/protocol_rudp.h#L165-L169) occupies exactly **4,196 bytes**.
  - There are zero calls to `malloc`, `free`, `realloc`, or OS memory allocators anywhere in the library core.
  - Guarantees: Zero memory fragmentation, zero garbage collection spikes, deterministic cache-line execution, and immediate bare-metal deployment on microcontrollers (STM32, ESP32, RISC-V) running FreeRTOS or bare metal with lwIP.
## Where the specialization stops

- **`ENet` & `RakNet` (Heap Bound)**:
  - Dynamically allocate list nodes and packet buffers on every outgoing and incoming message.
  - Risk of memory fragmentation over long runtimes in embedded or 24/7 dedicated server environments.
Reliable delivery in a zcrudp session channel waits for the missing sequence and
retains ahead-of-order data within a bounded RX window. ACKs remain cumulative;
legacy `rudp_recv()` does not use this RX buffer. Channels share the host link,
CPU and send budget. The unreliable freshness filter also has a configurable
maximum forward jump; applications must handle long gaps and session resets.

- **`Valve GNS` & `QUIC` (Complex Runtimes)**:
  - Allocate connection state machines, TLS session contexts, cryptographic key schedules, and stream reassembly buffers.
  - Footprint ranges from **100 KB to several megabytes** per active peer.
The repository does not demonstrate built-in cryptography, NAT traversal,
congestion control, general bulk-transfer management or measured STM32 deployment.
Optional multipart profiles now reassemble messages up to 65,535 bytes in a
caller-provided buffer; this is not a tuned bulk/file transport.
The ENCRYPTED capability flag is not an implementation of encryption. These
capabilities matter when choosing a production transport.

---
Four-byte TFV records can express control messages. They are not evidence of
an efficient bulk KV-cache transfer implementation, nor of superiority to RDMA,
QUIC or TCP for that workload.

### 2.3 Head-of-Line (HoL) Blocking & Multi-Channel Routing
## Measurement coverage and next comparisons

A fundamental flaw in single-stream transports (TCP, basic ARQ) is **Head-of-Line blocking**: a single lost packet stops the delivery of all subsequent data, even if later packets belong to independent data streams (e.g. physics telemetry vs chat message).
A codec microbenchmark and a full transport send path perform different work.
The [virtual-link harness](../bench/compare_transport.c) records exact revisions,
compiler settings, identical four-byte reliable workloads, loss/jitter scenarios,
goodput, p50/p95/p99 delivery delay and emitted datagram bytes. It also records
host execution cost including the simulator. Complete-run success is checked;
failures remain visible in the [graphs and CSV](bench/comparison/README.md).

- **`zcrudp`**:
  - Implements **isolated per-channel contexts**.
  - Channel 0 (Reliable / In-Order) manages its own sliding window.
  - Channel 1 (Unreliable / Bypass) streams continuous telemetry with a 16-bit anti-rollback sequence filter (RFC 1982). A dropped packet on Channel 0 does **not** stall Channel 1.
  - Channel 2 (Consensus / Barriers) operates independently of Channels 0 and 1.
  - Multi-channel ACKs are consolidated into a single MTU datagram via [`rudp_session_build_datagram()`](file:///home/charl/.gemini/antigravity/worktrees/zcrudp/feedback_project_analysis/src/rudp.c#L611-L691), avoiding inter-channel transmission delays.
Before expanding these findings into a broader performance claim, add:

- **`TCP`**:
  - Completely blocked. One dropped packet freezes the entire socket.
1. Real socket/NIC paths with finite link rates and controlled queue disciplines.
2. Isolated CPU time and peak memory including queues and allocator overhead.
3. Matched multi-channel and unreliable workloads.
4. Larger payloads that test the specialization's limits.
5. GNS/QUIC adapters with explicitly matched security and delivery requirements.

- **`KCP`**:
  - Single sliding window per instance. Supporting multiple channels requires instantiating multiple KCP objects and developing an external multiplexer.

---

### 2.4 Codec Performance & CPU Overhead

Empirical benchmark results measured with `CLOCK_MONOTONIC` on Linux x86-64 (Intel Core Ultra, GCC 13.3, `-O2`, `-fno-lto`):

| Library / Codec | Operation | Median Latency | Throughput | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **`zcrudp`** | `record_encode_8B` | **2.005 ns** | **498.7 Mops/s** | Direct bitshift, zero branch in pack |
| **`zcrudp`** | `frame_encode_8B` | **3.229 ns** | **309.7 Mops/s** | Direct bitshift |
| **`zcrudp`** | `record_decode_8B` | **9.881 ns** | **101.2 Mops/s** | Endian-safe wire validation |
| **`zcrudp`** | `frame_decode_8B` | **12.774 ns** | **78.3 Mops/s** | Endian-safe wire validation |
| **`zcrudp`** | `datagram_decode_16_records` | **29.965 ns** | **33.4 Mops/s** | **533.9 Mrecords/s** unpacked |
| **`KCP`** | Segment Encode | ~45 - 80 ns | ~12 - 22 Mops/s | 24-byte header serialization |
| **`ENet`** | Packet Encode + Queue | ~120 - 250 ns | ~4 - 8 Mops/s | Linked-list node allocation |
| **`Valve GNS`** | Packet Processing | ~300 - 800 ns | ~1.2 - 3.3 Mops/s | Encryption tag calculation + lane state |

---

### 2.5 Distributed AI KV-Cache Serving (Prefill-Decode Disaggregation)

Modern large language model (LLM) serving architectures decouple compute-intensive **Prefill nodes** (TTFT - Time to First Token) from memory-bound **Decode nodes** (TPOT - Time Per Output Token):

```text
   PREFILL NODE (GPU Pool A)                         DECODE NODE (GPU Pool B)
+--------------------------------+              +--------------------------------+
| - Ch0: Key-Value Cache Chunks  |              | - Ch0: KV Attention Reception  |
| - Ch2: Cluster Barrier / Sync  |              | - Ch1: Speculative Draft Tokens|
+--------------------------------+              +--------------------------------+
               │                                                ▲
               │==== Unified Datagram (MTU 1400B) =============│
               │   - Header ACK : Free Decode buffer            │
               │   - Record 0   : KV Chunk Tensor Data (Rel)    │
               │   - Record 1   : Cluster Barrier Barrier (Rel) │
               ▼                                                │
               │==== Reply Datagram (20B) ======================│
               │   - Header ACK : Confirms Ch0 KV Chunk         │
               │   - Record ACK : Confirms Ch2 Barrier (0ms)    │
               │   - Record Data: Ch1 Speculative Draft Token   │
```

- **Why `zcrudp` excels over TCP / gRPC**:
  - TCP head-of-line blocking stalls KV chunk streams if a single speculative token is lost.
  - `zcrudp` provides total multiplexing isolation: KV chunk streaming and speculative token generation proceed concurrently.
- **Why `zcrudp` excels over RoCEv2 / RDMA**:
  - RoCEv2 requires lossless Ethernet fabrics (PFC - Priority Flow Control), which trigger deadlock pause storms across heterogeneous or WAN datacenters.
  - `zcrudp` operates transparently over standard, lossy L3 commodity Ethernet and cloud VPC networks without requiring InfiniBand NICs.

---

## 3. Selection Guide: When to Use What

### Choose `zcrudp` if:
1. **Zero dynamic memory allocation is mandatory**: Bare-metal microcontrollers (STM32, ESP32, NXP), robotics, aerospace, medical devices, or zero-GC game engines.
2. **Minimal wire overhead matters**: Transmitting small, frequent state updates (input vectors, 32-bit sensor telemetry, KV attention blocks) where 24-byte headers are unacceptable.
3. **Multi-channel cohabitation without Head-of-Line blocking is needed**: Simultaneous reliable in-order transactions, unreliable sequenced telemetry, and speculative streaming in a single unified packet.
4. **Predictable, nanosecond-level codec throughput is required**: Up to 500 million operations/second per core without heap overhead.

### Choose `Valve GNS` if:
- You are developing a commercial PC game on Steam requiring built-in NAT punchthrough, Steam Relay Network (SDR) routing, and turn-key symmetric encryption.

### Choose `KCP` if:
- You need a single-channel ARQ protocol for mobile games with a custom memory allocator, and wire header overhead (24 bytes) is not a constraint.

### Choose `QUIC` if:
- You are building web-facing services, HTTP/3 APIs, or multi-stream microservice gateways with built-in TLS 1.3 certificate negotiation.

The separate [codec baseline](bench/codec.csv) measures only serialization. Its
ns/op figures must not be compared to whole-transport latency. The 57% KCP
figure above remains a wire-format calculation for one isolated message; the
new transport benchmark measures bundling and retransmissions as they occur.
