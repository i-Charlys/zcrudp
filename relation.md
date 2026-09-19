# Structure and function relationships

The current data structures, wire layouts, sizes and lifecycle are documented
in [ARCHITECTURE.md](ARCHITECTURE.md). This page maps the principal function
calls in `src/rudp.c` and `src/profiles.c`.

```text
Original single-context path
  rudp_reset ----------------------> rudp_init
  rudp_recv -----------------------> rudp_recv_ack_ex (passive ACK)
  rudp_recv_ack -------------------> rudp_recv_ack_ex (deliberate ACK)
  rudp_pack_frame -----------------> rudp_pack_header + rudp_pack_payload
  rudp_unpack_frame ---------------> rudp_unpack_header + rudp_unpack_payload
  rudp_pack_ack -------------------> rudp_pack_header
  rudp_unpack_ack -----------------> rudp_unpack_header

Multichannel path
  rudp_session_init ---------------> rudp_init for each channel
  rudp_session_send_reliable ------> rudp_send
  rudp_session_send_unreliable ----> rudp_pack_datagram_header + rudp_pack_record
  rudp_session_build_datagram -----> rudp_pack_record + rudp_pack_datagram_header
  rudp_session_process_datagram ---+
                                   +-> process_datagram
  rudp_session_process_datagram_at-+      -> rudp_unpack_datagram_header
                                          -> rudp_unpack_record
                                          -> session_ack -> rudp_recv_ack_ex
                                          -> drain_channel
  rudp_session_poll ---------------> drain_channel
  rudp_session_reset_channel ------> rudp_reset
  rudp_session_reset -------------> rudp_session_reset_channel

Optional profiles
  rudp_stream_pump ----------------> rudp_session_send_reliable
  rudp_motion_encode -------------> rudp_scalar_encode + magnitude
```

The application supplies the network calls between serialization and reception.
It also decides when to call `rudp_tick`, retrieves any returned slot frames and
sends retransmissions. Function arrows above denote actual C calls; adjacent
steps in the application's event loop are described in the architecture guide.
