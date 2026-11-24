#ifndef RUDP_CONFIG_H
#define RUDP_CONFIG_H

#include <stdint.h>

/* Opt-in ABI: the caller attaches reliable RX storage only where needed. */
#ifndef RUDP_EXTERNAL_RX_STORAGE
#define RUDP_EXTERNAL_RX_STORAGE 0
#endif
#if RUDP_EXTERNAL_RX_STORAGE != 0 && RUDP_EXTERNAL_RX_STORAGE != 1
#error "RUDP_EXTERNAL_RX_STORAGE must be 0 or 1"
#endif
#define RUDP_RX_BITMAP_SIZE ((RUDP_WINDOW_SIZE + 7) / 8)

#ifndef RUDP_WINDOW_SIZE
#define RUDP_WINDOW_SIZE 64
#endif

#if RUDP_WINDOW_SIZE < 2 || RUDP_WINDOW_SIZE > 16384 ||                                \
    (RUDP_WINDOW_SIZE & (RUDP_WINDOW_SIZE - 1)) != 0
#error "RUDP_WINDOW_SIZE must be a power of 2 between 2 and 16384"
#endif

#ifndef RUDP_EXPORT
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(ZCRUDP_BUILDING_DLL)
#define RUDP_EXPORT __declspec(dllexport)
#elif defined(ZCRUDP_USE_DLL)
#define RUDP_EXPORT __declspec(dllimport)
#else
#define RUDP_EXPORT
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define RUDP_EXPORT __attribute__((visibility("default")))
#else
#define RUDP_EXPORT
#endif
#endif

#define RUDP_SLOT_FREE 0
#define RUDP_SLOT_IN_FLIGHT 1
/** Selectively acknowledged ahead of cumulative tail; never retransmitted */
#define RUDP_SLOT_SACKED 2
/** Unreliable update staged for intra-tick bundling; freed immediately on build */
#define RUDP_SLOT_UNRELIABLE 3
/** PR-SCTP FORWARD-SEQ control record staged in TX ring; freed immediately on build */
#define RUDP_SLOT_FWD_SEQ 4

/** Low-nibble fast-recovery state in rudp_slot_s::fast_retransmit */
#define RUDP_FAST_STATE_MASK 0x0FU
/** Normal timer-based transmission */
#define RUDP_FAST_RETRANSMIT_OFF 0
/** Fast retransmit flag raised by Tri-ACK, SACK FACK, or NewReno partial ACK */
#define RUDP_FAST_RETRANSMIT_PENDING 1
/** Slot was in flight when fast recovery began (RFC 6582 NewReno) */
#define RUDP_FAST_RETRANSMIT_IN_RECOVERY 2
/**Bit 3: K=2 XOR parity record already emitted for an odd-numbered pair slot */
#define RUDP_SLOT_FLAG_FEC_SENT 0x08U
/** High-nibble shift storing adaptive timeout backoff exponent (0..15) */
#define RUDP_SLOT_BACKOFF_SHIFT 4U

/** Standalone header size (Tier 1: seq_num + ack) */
#define RUDP_WIRE_HEADER_SIZE 4
/** Standard frame size (Tier 2: Header + TFV Packet) */
#define RUDP_WIRE_FRAME_SIZE 8
/** Datagram header size on wire (ack + ack_channel + count) */
#define RUDP_WIRE_DATAGRAM_HEADER_SIZE 4
/** Message record size on wire (channel_id + flags + seq_num + tfv) */
#define RUDP_WIRE_RECORD_SIZE 8
/** Maximum number of 8-byte records representable in an 8-bit datagram count */
#define RUDP_MAX_RECORDS_PER_DATAGRAM 255U

/** Unreliable fire-and-forget message */
#define RUDP_RECORD_FLAG_UNRELIABLE (0U)
/** Reliable message requiring sliding window delivery */
#define RUDP_RECORD_FLAG_RELIABLE (1U << 0)
/** Explicit ACK record (seq_num = cumulative ACK N+1, payload = 32-bit SACK bitmap) */
#define RUDP_RECORD_FLAG_ACK (1U << 1)
/** Optional K=2 XOR parity record (seq_num = even pair base, payload = A ^ B) */
#define RUDP_RECORD_FLAG_FEC_PARITY (1U << 2)
/** PR-SCTP FORWARD-SEQ record (seq_num = new_cum_seq, payload = reason + dropped_count) */
#define RUDP_RECORD_FLAG_FWD_SEQ (1U << 3)
/** XTP SACK/Status Request flag requesting an immediate ACK at receiver */
#define RUDP_RECORD_FLAG_SREQ (1U << 4)
/** XTP Delivery Request flag requesting an ACK once delivered to the application */
#define RUDP_RECORD_FLAG_DREQ (1U << 5)
/** DCCP-style RLE Ack Vector + ECN modifier combined with RUDP_RECORD_FLAG_ACK */
#define RUDP_RECORD_FLAG_ACK_VEC (1U << 7)
/** Mask of defined wire record flag bits */
#define RUDP_RECORD_VALID_FLAGS_MASK                                                   \
  (RUDP_RECORD_FLAG_RELIABLE | RUDP_RECORD_FLAG_ACK | RUDP_RECORD_FLAG_FEC_PARITY |    \
   RUDP_RECORD_FLAG_FWD_SEQ | RUDP_RECORD_FLAG_SREQ | RUDP_RECORD_FLAG_DREQ |          \
   RUDP_RECORD_FLAG_ACK_VEC)

/** PR-SCTP abandonment reasons encoded in FORWARD-SEQ payload.type */
#define RUDP_FWD_SEQ_REASON_TTL 1U
#define RUDP_FWD_SEQ_REASON_RETRIES 2U
#define RUDP_FWD_SEQ_REASON_RING_FULL 3U
#define RUDP_FWD_SEQ_REASON_MANUAL 4U

/** Peer is disconnected or timed out */
#define RUDP_STATE_DISCONNECTED 0
/** Active healthy connection */
#define RUDP_STATE_CONNECTED 1

#ifndef RUDP_MAX_RETRIES
/** Max retransmissions before declaring dead peer */
#define RUDP_MAX_RETRIES 10
#endif

#ifndef RUDP_MAX_CHANNELS
/** Default number of independent channels per session */
#define RUDP_MAX_CHANNELS 4
#endif

#if RUDP_MAX_CHANNELS < 1 || RUDP_MAX_CHANNELS > 255
#error "RUDP_MAX_CHANNELS must be between 1 and 255"
#endif

#ifndef RUDP_UNRELIABLE_MAX_AHEAD
/** Maximum plausible sequence jump before rejecting as spurious */
#define RUDP_UNRELIABLE_MAX_AHEAD 512
#endif

/* Channel Capability Bitwise Flags */
/** No reliable capability; zero is not a testable flag bit */
#define RUDP_CHANNEL_FLAG_UNRELIABLE (0U)
/** Allows reliable sends through the sliding TX window */
#define RUDP_CHANNEL_FLAG_RELIABLE (1U << 0)
/** Enforces strict in-order reliable delivery; omit for zero-HoL reliable unordered */
#define RUDP_CHANNEL_FLAG_ORDERED (1U << 1)
/** Integration hint only (legacy name); zcrudp performs NO internal encryption */
#define RUDP_CHANNEL_FLAG_ENCRYPTED (1U << 2)
/** Clear alias indicating caller-managed external AEAD/DTLS/WireGuard protection */
#define RUDP_CHANNEL_HINT_EXTERNAL_AEAD RUDP_CHANNEL_FLAG_ENCRYPTED
/** Enables opt-in K=2 XOR forward error correction in separate UDP loss domains */
#define RUDP_CHANNEL_FLAG_FEC_K2 (1U << 3)
/** Enables PR-SCTP timed/bounded partial reliability (FORWARD-SEQ) on channel */
#define RUDP_CHANNEL_FLAG_PR_SCTP (1U << 4)
/** Enables XTP SREQ/DREQ burst-tail ACK suppression on channel */
#define RUDP_CHANNEL_FLAG_XTP_SREQ (1U << 5)
/** Enables XTP FASTNAK immediate gap reporting when out-of-order data arrives */
#define RUDP_CHANNEL_FLAG_FASTNAK (1U << 6)
/** Enables DCCP RLE Ack Vector + ECN feedback decoding on channel */
#define RUDP_CHANNEL_FLAG_ACK_VEC (1U << 7)

#ifndef RUDP_DEFAULT_MTU
/** Safe default UDP datagram payload limit preventing IP fragmentation */
#define RUDP_DEFAULT_MTU 1400
#endif

/* Standardized Return & Error Codes */
/** Success / operation completed */
#define RUDP_OK 0
/** NULL pointer or invalid argument */
#define RUDP_ERR_INVALID_ARG -1
/** Connection dead or disconnected */
#define RUDP_ERR_DISCONNECTED -2
/** Transmission buffer is full */
#define RUDP_ERR_BUFFER_FULL -3
/** Sequence or ACK is outside active window boundaries */
#define RUDP_ERR_OUT_OF_WINDOW -4
/** Cryptographic authentication tag or anti-replay verification failed */
#define RUDP_ERR_AUTH -5

#endif /* RUDP_CONFIG_H */
