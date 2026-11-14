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

/** Normal timer-based transmission */
#define RUDP_FAST_RETRANSMIT_OFF 0
/** Fast retransmit flag raised by Tri-ACK or NewReno partial ACK */
#define RUDP_FAST_RETRANSMIT_PENDING 1
/** Slot was in flight when fast recovery began (RFC 6582 NewReno) */
#define RUDP_FAST_RETRANSMIT_IN_RECOVERY 2

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
/** Explicit multi-channel ACK record (seq_num carries ACK N+1) */
#define RUDP_RECORD_FLAG_ACK (1U << 1)
/** Mask of defined wire record flag bits */
#define RUDP_RECORD_VALID_FLAGS_MASK (RUDP_RECORD_FLAG_RELIABLE | RUDP_RECORD_FLAG_ACK)

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
/** Declarative hint; reliable session delivery is always ordered */
#define RUDP_CHANNEL_FLAG_ORDERED (1U << 1)
/** Integration hint only (legacy name); zcrudp performs NO internal encryption */
#define RUDP_CHANNEL_FLAG_ENCRYPTED (1U << 2)
/** Clear alias indicating caller-managed external AEAD/DTLS/WireGuard protection */
#define RUDP_CHANNEL_HINT_EXTERNAL_AEAD RUDP_CHANNEL_FLAG_ENCRYPTED

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

#endif /* RUDP_CONFIG_H */
