# Structure and function relationships

See [ARCHITECTURE.md](ARCHITECTURE.md) for current data structures, wire layouts,
sizes and lifecycle, and [module boundaries](docs/MODULES.md) for source ownership.

| Operation | Principal calls |
| --- | --- |
| Context reset | `rudp_reset` calls `rudp_init` |
| Data reception | `rudp_recv` calls `rudp_recv_ack_ex` for a passive ACK |
| Explicit ACK | `rudp_recv_ack` calls `rudp_recv_ack_ex` |
| Frame codec | Frame packing/unpacking delegates to header and payload codecs |
| Session initialization | `rudp_session_init` initializes each channel context |
| Reliable submission | `rudp_session_send_reliable` calls `rudp_send` |
| Unreliable submission | Packs a datagram header and record |
| Datagram builder | Packs selected records and the datagram header |
| Datagram reception | Validates, decodes, processes ACKs and drains channel records |
| Session polling | Drains records retained by output backpressure |
| Channel reset | `rudp_session_reset_channel` calls `rudp_reset` |
| Session reset | Resets each channel |
| Stream pumping | `rudp_stream_pump` calls `rudp_session_send_reliable` |
| Motion encoding | `rudp_motion_encode` uses scalar encoding and magnitude calculation |

The application provides network calls between serialization and reception. For the
single-context API it also calls `rudp_tick()`, retrieves returned slot frames and
sends retransmissions. The session builder owns its own retransmission selection.
