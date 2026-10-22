# Research: low-latency media & GPU-NIC pipeline (companion track `zcrudp-media`)

These two tracks were moved out of `TODO.md` Phase 7 (2026-10-22 review) so the
roadmap stays focused on the core C11 transport. They are companion research
tracks, not core engine work: the core (`zcrudp-core`) remains 100% pure
standard C11, OS-agnostic, zero-allocation, zero-dependency.

## Intra-Refresh Low-Latency Video Slices & Edge Media Streaming (Codec-Agnostic & Universal)

- **Codec-Agnostic Payload Slices**: Treat video frames as opaque binary streams; avoid hardcoding to a single codec. Support dual interchangeable profiles:
  - *Ultra-LAN / Dedicated Radio Profile*: PyroWave intra-only Vulkan compute codec (150--300 Mbit/s, < 0.1 ms encode/decode, 0 temporal GOP latency).
  - *Bandwidth-Constrained WAN / Mobile Profile*: H.264 / HEVC / AV1 with rolling vertical Intra-Refresh (8--20 Mbit/s, low flat bitrate, no I-frame spikes).
- **Fixed 1024-byte MTU Chunking (Themaister / PyroFling approach)**: Divide any frame bitstream into deterministic, fixed-size chunks of 1024 bytes (fitting cleanly inside standard 1500-byte MTU after IP/UDP/RUDP headers of ~1056 bytes on wire), completely eliminating path MTU black holes and IP fragmentation.
- **Zero-Allocation Packet Slicing**: Packetize video frames without heap allocation via a deterministic descriptor header: `{uint32_t frame_id, uint16_t chunk_idx, uint16_t chunk_count, uint32_t payload_len}` mapped directly to caller GPU/staging buffers.
- **Erasure Channel Optimization & Sliding XOR FEC**: Exploit UDP checksum erasure (dropped corrupt packets leave distinct sequence holes). Pair macroblock slices with causal XOR FEC parity slices (`[1,2,4,8]`) for 0-RTT immediate loss recovery.
- **Independent Frame Recovery & Zero-Lag Drop**: Because intra-only frames or intra-refresh slices lack temporal GOP cascades, an incomplete frame is immediately abandoned when the next `frame_id` arrives, preventing queue backlog and display latency accumulation.

## Heterogeneous GPU-NIC Direct Pipeline (Universal Vulkan Acceleration & Zero-Host-Copy)

- **Strict Architectural Decoupling (Core vs Optional Acceleration)**:
  - *Core Engine (`zcrudp-core`)*: Remains 100% pure standard C11, OS-agnostic, zero-allocation, zero-dependency. Compilable on bare-metal, RTOS, headless game servers, and all desktop/console platforms.
  - *Hardware Acceleration (`zcrudp-vulkan`)*: Optional companion module for platforms with modern GPU/Vulkan support (Windows, Linux, Android, Switch).
- **Hybrid Data/Control Plane Split**:
  - *GPU Data Plane (Vulkan Compute)*: Exploit ZCRUDP's deterministic layout to execute 1024B packet slicing and parallel SIMD Causal XOR FEC entirely in VRAM via universal SPIR-V compute shaders (< 5 microseconds execution).
  - *CPU Control Plane (C Engine)*: Because GPUs cannot interface with OS network sockets and struggle with branchy millisecond timers, the host CPU retains sole responsibility for lightweight ACK routing, RTO/PTO state, and socket dispatch.
- **Asynchronous Pipelining & Non-Blocking Timeline Semaphores**:
  - Prevent CPU stall: Avoid blocking CPU waits (`vkWaitForFences`). Chain compute shaders to graphics render queues via standard Vulkan Timeline Semaphores (`VK_KHR_timeline_semaphore`). The CPU non-blockingly polls semaphore completion (`vkGetSemaphoreCounterValue`) during its tick.
  - If the GPU slips below the network tick deadline, the CPU skips the pending video frame and continues delivering 240 Hz input/telemetry packets without latency penalty.
- **Tiered Memory Hierarchy (ReBAR vs Legacy Non-ReBAR Systems)**:
  - *Tier 1: Direct VRAM-to-NIC P2P DMA (Systems with ReBAR / SAM)*:
    - Supported on modern GPUs (RTX 3000+, RX 6000+, Intel Arc) with compatible BIOS/motherboards.
    - VRAM exported directly to NIC without touching host system RAM (Multi-OS: Windows `VK_KHR_external_memory_win32`/DirectStorage, Linux `VK_KHR_external_memory_fd`/`dma-buf`, Android `AHardwareBuffer`).
  - *Tier 2: Universal Host-Visible Pinned Ring (Legacy Systems WITHOUT ReBAR)*:
    - Essential for millions of gamers on older GPUs (GTX 1060/1070/1080, RTX 2060/2070/2080, GTX 1660, legacy chipsets, laptops).
    - Allocate a fixed 2--4 MiB circular ring in pinned host RAM with `VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT`.
    - The GPU compute shader streams packet slices directly into host RAM across PCIe via hardware Write-Combining (12--16 GB/s on PCIe 3.0/4.0).
    - **Zero CPU `memcpy`**: The CPU still does NOT copy the payload; it simply hands the pinned host pointers to standard OS sockets (`WSASendMsg` / `sendmsg`), and the NIC standard DMA streams them onto the wire.
  - *Tier 3: Pure C11 CPU Fallback*: Standard memory copy path for headless servers or environments without Vulkan compute shaders.
- **Universal Micro-Pacing & Anti-Bufferbloat**:
  - Smooth bursty slice transmission across frame intervals (e.g. 1 packet every ~25 microseconds at 240 Hz) to eliminate switch/router queue congestion (bufferbloat).
  - Platform-neutral rate pacing:
    - *Hardware/Kernel pacing*: Linux `SO_TXTIME` / `fq` qdisc where available.
    - *Host timer pacing*: Windows multimedia / high-resolution waitable timers (`CreateWaitableTimerExW` with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) and high-precision user-space leaky-bucket loops.
    - *Congestion Feedback*: Core SCReAM delay-gradient tracking (OWD) scaling back bitrates dynamically before packet loss occurs.
