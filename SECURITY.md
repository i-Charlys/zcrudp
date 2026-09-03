# Security Policy

## Scope and Threat Model

`zcrudp` is a lightweight, zero-dependency reliable UDP transport library designed for high-performance real-time game state synchronization.

### Cleartext Transport Notice

By design, the base protocol operates in **cleartext**:
- **No Encryption**: Payload frames and control headers are not encrypted on the wire.
- **No Peer Authentication**: Session handshakes (when introduced) and packet delivery do not cryptographically verify peer identity.
- **Data Integrity**: Transport reliability defends against packet loss, reordering, and corruption, but does not provide cryptographic tamper-proofing or replay defense against active on-path attackers (MitM).

### Recommended Security Practices

If deployed in untrusted network environments or for security-sensitive game traffic:
1. **Encrypted Transport Layer**: Wrap RUDP traffic in a secure tunnel such as DTLS (Datagram Transport Layer Security) or WireGuard.
2. **Application-Level Security**: Encrypt and sign sensitive transactions (e.g., authentication tokens, inventory trades) at the application level before handing them to `zcrudp`.
3. **DoS and Amplification Prevention**: Enforce rate-limiting and connection validation at the firewall or edge load-balancer level to mitigate UDP amplification attacks.

## Reporting a Vulnerability

If you discover a security vulnerability or critical memory safety issue within `zcrudp`, please report it responsibly by contacting the repository maintainer via private security advisory on GitHub rather than opening a public issue.
