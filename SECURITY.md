# Security Policy & Threat Model

## 1. Scope & Design Philosophy

`zcrudp` is a lightweight, zero-dependency reliable UDP transport library designed
for high-performance real-time game state synchronization and real-time messaging
on embedded and server targets. It is an ultra-low-latency,
zero-dynamic-allocation (zero-malloc) transport engine intended for controlled
LANs, VPCs, or private backbones.

### Cleartext Transport Notice

By design, the base protocol operates in **cleartext**:

- **No Encryption**: Payload frames and control headers are not encrypted on the wire.
- **No Peer Authentication**: Session handshakes (when introduced) and packet delivery do not cryptographically verify peer identity.
- **Data Integrity**: Transport reliability defends against packet loss, reordering, and corruption, but does not provide cryptographic tamper-proofing or replay defense against active on-path attackers (MitM).

## 2. Threat Model & Transport Security

By architecture, the base protocol operates entirely in **cleartext** with no
embedded cryptographic layer:

- **No In-Band Encryption**: Packet headers and 4-byte TFV payloads are transmitted unencrypted on the wire.
- **No Cryptographic Authentication**: Packets are routed solely based on the network 4-tuple (source IP, source port, destination IP, destination port).
- **No Cryptographic Integrity**: Standard UDP checksums only detect accidental corruption per RFC 8085 Section 3.4 and provide no protection against active on-path tampering or frame modification.

### Attacker levels

Security requirements should be documented per attacker level, not as a single
`secure` indicator:

| Level | Capability | Required protection |
|---|---|---|
| Blind attacker | Injection without prior observation | Authentication; unpredictable identifiers only as complementary defense |
| Active on-path observer | Capture, modification, duplication, reordering, suppression | MAC/AEAD, anti-replay, establishment freshness |
| Authorized malicious peer | Holds the association keys | Protocol validation, quotas, isolation; no authentication makes its ACKs honest |
| Local compromise / rollback | Restoration of old keys, counters and state | Persistence and recovery policy, not just a wire format |

A random initial sequence number or a hard-to-guess connection ID only slows a
blind attacker. Once the attacker observes traffic, these public values do not
replace an authenticator.

### Recommended Security Practices

If deployed in untrusted network environments or for security-sensitive game traffic:

1. **Encrypted Transport Layer**: Wrap RUDP traffic in a secure tunnel such as DTLS (Datagram Transport Layer Security) or WireGuard.
2. **Application-Level Security**: Encrypt and sign sensitive transactions (e.g., authentication tokens, inventory trades) at the application level before handing them to `zcrudp`.
3. **DoS and Amplification Prevention**: Enforce rate-limiting and connection validation at the firewall or edge load-balancer level to mitigate UDP amplification attacks. Bound responses sent before address validation, and never sacrifice the active session to admit an unknown candidate.

## 3. Attack Vectors & Mitigations

### A. Unauthenticated ACK Spoofing & Blind Injection

- **Risk**: An attacker capable of observing or guessing the UDP 4-tuple and in-flight sequence numbers can inject spoofed cumulative ACK frames. This could prematurely advance the sender's sliding window, causing silent packet loss, or trigger spurious Fast Retransmits.
- **Mitigation**: When operating over public or untrusted WAN networks, `zcrudp` **MUST** be encapsulated within a cryptographic tunnel such as **DTLS (Datagram Transport Layer Security)**, **WireGuard**, or **IPSec**. Cryptographic tunneling provides authenticated encryption (AEAD), replay protection, and endpoint authentication. A protection envelope must authenticate the entire datagram before any reliability state mutation, bind a session ID/nonce with a bounded anti-replay window, and reject `ACK`, `HELLO`, `RESET`, or data whose tag is invalid. After authentication, the engine must still validate that an ACK targets the active channel and generation, does not acknowledge beyond the last message actually sent, respects the sequence arithmetic, and never moves state backwards.

### B. Peer Restart Sequence Wedging

- **Risk**: If a peer crashes and restarts without a cryptographic handshake or randomized initial sequence number (ISN), its TX sequence numbers reset to 0. The remote surviving peer will reject incoming packets as out-of-window (or stale duplicates) because its expected sequence number is significantly ahead.
- **Mitigation**:
  1. **Liveness Monitoring**: Applications must integrate `rudp_touch()` on valid packet reception and periodically check `rudp_is_alive(ctx, now, idle_timeout)`.
  2. **Session Reset Protocol**: Applications must implement a reconnection handshake (or exchange a fresh session UUID) upon restart, calling `rudp_session_reset()` on both peers to re-synchronize sliding windows. An authenticated `HELLO`/`HELLO_ACK` establishment that installs fresh per-direction sequence anchors is specified in the roadmap.

### C. Unreliable Telemetry Muting

- **Limit**: The unreliable channel applies a bounded RFC 1982 sequence distance filter (`RUDP_UNRELIABLE_MAX_AHEAD = 512`). This bound only rejects implausibly large jumps (accidental reordering or burst loss). **It is not a defense against a malicious injector**: without authentication, an attacker who can send packets to the 4-tuple can mute an unreliable channel at will. Measured on the current code: 65 forged records, each within the 512 bound, cause 32,501 legitimate packets to be dropped (~135 s of telemetry at 240 Hz), and the attack can be repeated indefinitely. Independently, any outage longer than 512 emissions wedges the channel until the 16-bit sequence wraps: a 600-packet outage (~2.5 s at 240 Hz) causes 64,936 valid packets to be rejected (~270 s).
- **Mitigation**: Authenticated transport (AEAD envelope, DTLS, or WireGuard) so injected records fail tag verification. In parallel, the roadmap replaces the implicit first-record baseline with an explicitly established per-channel reference, and defines explicit behavior after more than `RUDP_UNRELIABLE_MAX_AHEAD` missing emissions: accept authenticated progression, request authenticated resync, or replace the incarnation — never stay wedged indefinitely.

### D. Congestion Collapse & IP Fragmentation Defense

- **Binary Exponential Backoff (Not Congestion Control)**: Loss-triggered retransmissions apply Karn-compliant exponential backoff (capped by `rto_max_ms`) to limit retransmission storms during blackouts. Per RFC 8085 Section 3.1.2, backoff alone is not a full TCP-friendly congestion controller: applications driving sustained high bitrates over shared links must apply application-level pacing or rate control.
- **MTU Clamping**: The intra-tick bundler (`rudp_session_build_datagram` / `rudp_session_prepare_datagram`) defaults to `RUDP_DEFAULT_MTU` (1400 bytes), keeping unencapsulated UDP/IPv4 datagrams below standard Ethernet limits to avoid IP fragmentation and amplification. Callers over constrained paths should lower this budget with `rudp_session_set_mtu()`.

## 4. Recommended Deployment Architecture

For public Internet deployments, embed `zcrudp` in a layered architecture:

```
+-------------------------------------------------------------+
|                     Application Layer                       |
|              (4-byte TFV Game / Realtime State)             |
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

When layering `zcrudp` over DTLS, WireGuard, or IPv6 paths (1280-byte minimum MTU), configure a tighter session budget with `rudp_session_set_mtu()` (for example `1200` bytes, or discover safe bounds with DPLPMTUD per RFC 8899) so outer encapsulation overhead does not cause fragmentation.

A datagram with a failed authentication tag must be dropped before reaching
`rudp_session_process_datagram*()`, not used to destroy the session: terminating an
association on a forged packet would itself be a denial-of-service vector.

## 5. Reporting a Vulnerability

If you discover a security vulnerability or critical memory safety issue within `zcrudp`, please report it responsibly by contacting the repository maintainer via private security advisory on GitHub rather than opening a public issue. Do NOT open a public GitHub issue for undisclosed vulnerabilities.
