# Security Policy
# Security Policy & Threat Model

## Scope and Threat Model
## 1. Scope & Design Philosophy

`zcrudp` is a lightweight, zero-dependency reliable UDP transport library designed for high-performance real-time game state synchronization.
`zcrudp` is an ultra-low-latency, zero-dynamic-allocation (zero-malloc) reliable UDP transport engine engineered specifically for real-time game state synchronization, embedded robotics, and AI KV-cache transport within controlled LANs, VPCs, or private backbones.

### Cleartext Transport Notice
---

By design, the base protocol operates in **cleartext**:
- **No Encryption**: Payload frames and control headers are not encrypted on the wire.
- **No Peer Authentication**: Session handshakes (when introduced) and packet delivery do not cryptographically verify peer identity.
- **Data Integrity**: Transport reliability defends against packet loss, reordering, and corruption, but does not provide cryptographic tamper-proofing or replay defense against active on-path attackers (MitM).
## 2. Threat Model & Transport Security (Critical 1)

### Recommended Security Practices
### Cleartext 4-Tuple UDP Transport

If deployed in untrusted network environments or for security-sensitive game traffic:
1. **Encrypted Transport Layer**: Wrap RUDP traffic in a secure tunnel such as DTLS (Datagram Transport Layer Security) or WireGuard.
2. **Application-Level Security**: Encrypt and sign sensitive transactions (e.g., authentication tokens, inventory trades) at the application level before handing them to `zcrudp`.
3. **DoS and Amplification Prevention**: Enforce rate-limiting and connection validation at the firewall or edge load-balancer level to mitigate UDP amplification attacks.
By architecture, the base protocol operates entirely in **cleartext** with no embedded cryptographic layer:
- **No In-Band Encryption**: Packet headers and 4-byte TFV payloads are transmitted unencrypted on the wire.
- **No Cryptographic Authentication**: Packets are routed solely based on the network 4-tuple (source IP, source port, destination IP, destination port).
- **No Cryptographic Integrity**: Standard UDP checksums provide transmission error detection, but no protection against active on-path tampering or frame modification.

## Reporting a Vulnerability
### Attack Vectors & Mitigations

If you discover a security vulnerability or critical memory safety issue within `zcrudp`, please report it responsibly by contacting the repository maintainer via private security advisory on GitHub rather than opening a public issue.
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

