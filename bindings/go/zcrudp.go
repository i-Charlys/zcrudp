// Package zcrudp provides cgo bindings over the zcrudp C11 library.
package zcrudp

/*
#cgo CFLAGS: -I${SRCDIR}/../../include
#cgo LDFLAGS: -L${SRCDIR}/../../build -lzcrudp
#include "protocol_rudp.h"
#include "protocol_profiles.h"

static tfv_packet_u go_make_tfv(uint8_t t, uint8_t f, uint16_t v) {
  tfv_packet_u p = {.type = t, .flags = f, .value = v};
  return p;
}
*/
import "C"
import (
	"fmt"
	"unsafe"
)

const (
	ChannelReliable   = uint8(C.RUDP_CHANNEL_FLAG_RELIABLE)
	ChannelOrdered    = uint8(C.RUDP_CHANNEL_FLAG_ORDERED)
	ChannelUnreliable = uint8(C.RUDP_CHANNEL_FLAG_UNRELIABLE)
	ChannelEncrypted  = uint8(C.RUDP_CHANNEL_FLAG_ENCRYPTED)
	ChannelFecK2      = uint8(C.RUDP_CHANNEL_FLAG_FEC_K2)
	MaxDatagramSize   = int(C.RUDP_DEFAULT_MTU) + int(C.RUDP_WIRE_DATAGRAM_HEADER_SIZE)
	CryptoOverhead    = int(C.RUDP_CRYPTO_OVERHEAD_BYTES)
)

// Packet represents an unpacked 4-byte TFV payload.
type Packet struct {
	Type  uint8
	Flags uint8
	Value uint16
}

// DeliveredRecord holds one delivered wire record from ProcessDatagram or Poll.
type DeliveredRecord struct {
	ChannelID uint8
	Flags     uint8
	SeqNum    uint16
	Payload   Packet
}

// Session wraps a caller-owned zero-allocation rudp_session_s.
type Session struct {
	raw C.rudp_session_s
}

func errFromCode(op string, rc C.int) error {
	if rc < 0 {
		return fmt.Errorf("zcrudp: %s returned %d", op, int(rc))
	}
	return nil
}

// NewSession initializes a multi-channel session.
func NewSession() (*Session, error) {
	s := &Session{}
	if err := errFromCode("rudp_session_init", C.rudp_session_init(&s.raw)); err != nil {
		return nil, err
	}
	return s, nil
}

// ConfigChannel configures per-channel reliability, ordering, and FEC K=2 flags.
func (s *Session) ConfigChannel(channelID uint8, flags uint8) error {
	return errFromCode("rudp_session_config_channel",
		C.rudp_session_config_channel(&s.raw, C.uint8_t(channelID), C.uint8_t(flags)))
}

// ConfigRecovery enables adaptive RTT, 32-bit SACK/FACK, and TLP recovery.
func (s *Session) ConfigRecovery(channelID uint8, initialMs, minMs, maxMs uint32) error {
	return errFromCode("rudp_session_config_recovery",
		C.rudp_session_config_recovery(&s.raw, C.uint8_t(channelID), C.uint32_t(initialMs), C.uint32_t(minMs), C.uint32_t(maxMs)))
}

// SendReliable queues a 4-byte TFV payload for reliable transmission.
func (s *Session) SendReliable(channelID uint8, pkt Packet, nowMs uint32) error {
	cp := C.go_make_tfv(C.uint8_t(pkt.Type), C.uint8_t(pkt.Flags), C.uint16_t(pkt.Value))
	return errFromCode("rudp_session_send_reliable",
		C.rudp_session_send_reliable(&s.raw, C.uint8_t(channelID), cp, C.uint32_t(nowMs)))
}

// QueueUnreliable stages an unreliable payload for intra-tick datagram bundling.
func (s *Session) QueueUnreliable(channelID uint8, pkt Packet) error {
	cp := C.go_make_tfv(C.uint8_t(pkt.Type), C.uint8_t(pkt.Flags), C.uint16_t(pkt.Value))
	return errFromCode("rudp_session_queue_unreliable",
		C.rudp_session_queue_unreliable(&s.raw, C.uint8_t(channelID), cp))
}

// HasPendingWork returns true when an immediate TX, probe, or ACK emission is due.
func (s *Session) HasPendingWork(nowMs, fallbackTimeoutMs uint32) bool {
	return bool(C.rudp_session_has_pending_work(&s.raw, C.uint32_t(nowMs), C.uint32_t(fallbackTimeoutMs)))
}

// BuildDatagram packs pending records and ACKs into outBuf and returns byte length.
func (s *Session) BuildDatagram(primaryAckChannel uint8, outBuf []byte, nowMs, fallbackTimeoutMs uint32) (int, error) {
	if len(outBuf) == 0 {
		return 0, fmt.Errorf("zcrudp: empty output buffer")
	}
	n := C.rudp_session_build_datagram(
		&s.raw,
		C.uint8_t(primaryAckChannel),
		(*C.uint8_t)(unsafe.Pointer(&outBuf[0])),
		C.size_t(len(outBuf)),
		C.uint32_t(nowMs),
		C.uint32_t(fallbackTimeoutMs),
	)
	if err := errFromCode("rudp_session_build_datagram", n); err != nil {
		return 0, err
	}
	return int(n), nil
}

// ProcessDatagramAt validates and processes an incoming datagram at timestamp nowMs.
func (s *Session) ProcessDatagramAt(inBuf []byte, nowMs uint32, maxRecords int) ([]DeliveredRecord, error) {
	if len(inBuf) == 0 || maxRecords <= 0 {
		return nil, fmt.Errorf("zcrudp: invalid process args")
	}
	recs := make([]C.rudp_record_s, maxRecords)
	n := C.rudp_session_process_datagram_at(
		&s.raw,
		(*C.uint8_t)(unsafe.Pointer(&inBuf[0])),
		C.size_t(len(inBuf)),
		&recs[0],
		C.size_t(maxRecords),
		C.uint32_t(nowMs),
	)
	if err := errFromCode("rudp_session_process_datagram_at", n); err != nil {
		return nil, err
	}
	out := make([]DeliveredRecord, int(n))
	for i := 0; i < int(n); i++ {
		out[i] = DeliveredRecord{
			ChannelID: uint8(recs[i].channel_id),
			Flags:     uint8(recs[i].flags),
			SeqNum:    uint16(recs[i].seq_num),
			Payload: Packet{
				Type:  uint8(recs[i].payload.type),
				Flags: uint8(recs[i].payload.flags),
				Value: uint16(recs[i].payload.value),
			},
		}
	}
	return out, nil
}

// CryptoCtx wraps a caller-owned rudp_crypto_ctx_s.
type CryptoCtx struct {
	raw C.rudp_crypto_ctx_s
}

// NewCryptoCtx initializes directional ChaCha20-Poly1305 AEAD + replay protection.
func NewCryptoCtx(key [32]byte, sessionID uint64, initiator bool) (*CryptoCtx, error) {
	c := &CryptoCtx{}
	initFlag := C.uint8_t(0)
	if initiator {
		initFlag = 1
	}
	if err := errFromCode("rudp_crypto_init",
		C.rudp_crypto_init(&c.raw, (*C.uint8_t)(unsafe.Pointer(&key[0])), C.uint64_t(sessionID), initFlag)); err != nil {
		return nil, err
	}
	return c, nil
}

// Wipe zeroes key material in constant time.
func (c *CryptoCtx) Wipe() {
	C.rudp_crypto_wipe(&c.raw)
}
