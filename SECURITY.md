# Security Policy & Threat Model

## 1. Scope & Design Philosophy

`zcrudp` is an ultra-low-latency, zero-dynamic-allocation (zero-malloc) reliable UDP transport engine engineered specifically for real-time game state synchronization, embedded robotics, and AI KV-cache transport within controlled LANs, VPCs, or private backbones.

---

## 2. Threat Model & Transport Security (Critical 1)

### Cleartext 4-Tuple UDP Transport

By architecture, the base protocol operates entirely in **cleartext** with no embedded cryptographic layer:
- **No In-Band Encryption**: Packet headers and 4-byte TFV payloads are transmitted unencrypted on the wire.
- **No Cryptographic Authentication**: Packets are routed solely based on the network 4-tuple (source IP, source port, destination IP, destination port).
- **No Cryptographic Integrity**: Standard UDP checksums provide transmission error detection, but no protection against active on-path tampering or frame modification.

### Attack Vectors & Mitigations

#### A. Unauthenticated ACK Spoofing & Blind Injection
- **Risk**: An attacker capable of observing or guessing the UDP 4-tuple and in-flight sequence numbers can inject spoofed cumulative ACK frames. This could prematurely advance the sender's sliding window, causing silent packet loss, or trigger spurious Fast Retransmits.
- **Mitigation**: When operating over public or untrusted WAN networks, `zcrudp` **MUST** be encapsulated within a cryptographic tunnel such as **DTLS (Datagram Transport Layer Security)**, **WireGuard**, or **IPSec**. Cryptographic tunneling provides authenticated encryption (AEAD), replay protection, and endpoint authentication.

#### B. Peer Restart Sequence Wedging (High 6)
- **Risk**: If a peer crashes and restarts without a cryptographic handshake or randomized initial sequence number (ISN), its TX sequence numbers reset to 0. The remote surviving peer will reject incoming packets as out-of-window (or stale duplicates) because its expected sequence number is significantly ahead.
- **Mitigation**:
  1. **Liveness Monitoring**: Applications must integrate `rudp_touch()` on valid packet reception and periodically check `rudp_is_alive(ctx, now, idle_timeout)`.
  2. **Session Reset Protocol**: Applications must implement a reconnection handshake (or exchange a fresh session UUID) upon restart, calling `rudp_session_reset()` on both peers to re-synchronize sliding windows.

#### C. Unreliable Telemetry Muting Defense (Critical 2)
- **Mitigation**: The unreliable channel incorporates a bounded RFC 1982 sequence distance filter (`RUDP_UNRELIABLE_MAX_AHEAD = 512`). Unbounded forward jumps (e.g., jumping ahead by 32,767 packets) are dropped, preventing malicious desynchronization from muting telemetry channels.

#### D. Congestion Collapse & IP Fragmentation Defense (High 3 & High 8)
- **Binary Exponential Backoff**: Retransmissions apply exponential backoff (capped at 64x) to avoid congestive collapse under severe packet loss.
- **MTU Clamping**: The intra-tick bundler (`rudp_session_build_datagram`) clamps outgoing datagrams to `RUDP_DEFAULT_MTU` (1400 bytes), preventing IP fragmentation vulnerabilities and amplification.

---

## 3. Recommended Deployment Architecture

For public Internet deployments, embed `zcrudp` in a layered architecture:

```
+-------------------------------------------------------------+
|                     Application Layer                       |
|           (4-byte TFV Game / AI KV-Cache State)             |
+-------------------------------------------------------------+
                              |
                              v
+-------------------------------------------------------------+
|                     zcrudp Session Layer                    |
|    - Multi-Channel Multiplexing (Reliable / Unreliable)     |
|    - Sliding Window, Cumulative ACK, Tri-ACK, RTO Backoff   |
+-------------------------------------------------------------+
                              |
                              v
+-------------------------------------------------------------+
|              Security Layer (DTLS / WireGuard)              |
|        - Authenticated Encryption (AES-GCM / ChaCha20)      |
|        - Cryptographic Replay Protection & PFS Key Exchange |
+-------------------------------------------------------------+
                              |
                              v
+-------------------------------------------------------------+
|                       UDP / IPv4 / IPv6                     |
+-------------------------------------------------------------+
```

---

## 4. Reporting Security Vulnerabilities

If you identify a security flaw or buffer boundary vulnerability in `zcrudp`:
- Please submit a private vulnerability advisory via GitHub Security Advisories.
- Do NOT open a public GitHub issue for undisclosed vulnerabilities.

