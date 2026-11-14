#ifndef RUDP_TYPES_H
#define RUDP_TYPES_H

#include "rudp_config.h"
#include "protocol_tfv.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Represents the header of a RUDP frame. Length: 4 bytes.
 */
typedef struct {
  uint16_t seq_num; /**< Sequence number of this packet */
  uint16_t ack;     /**< Cumulative ACK: Next expected sequence number (N+1) */
} rudp_header_s;

/**
 * @brief Represents a RUDP frame, containing a header and a TFV packet. Length:
 * 8 bytes.
 */
typedef struct {
  rudp_header_s header; /**< Header: 4 bytes */
  tfv_packet_u packet;  /**< TFV packet: 4 bytes */
} rudp_frame_s;

/**
 * @brief Represents one local TX-window slot. ABI size: 16 bytes.
 */
typedef struct {
  rudp_frame_s frame;
  uint32_t timestamp;
  uint8_t state;   /**< 0: FREE, 1: IN_FLIGHT, 2: SACKED */
  uint8_t retries; /**< Number of retransmission attempts for this slot */
  uint8_t fast_retransmit; /**< Bits 0..2: fast state; bit 3: FEC sent; bits 4..7: timeout backoff */
  uint8_t tx_count; /**< Number of transmissions on wire (0 = pending initial TX) */

} rudp_slot_s;

/** Extracts the low-nibble fast-recovery state (OFF / PENDING / IN_RECOVERY). */
static inline uint8_t rudp_slot_fast_state(const rudp_slot_s *slot) {
  return (uint8_t)(slot->fast_retransmit & 0x07U);
}

/** Sets the low-nibble fast-recovery state while preserving FEC bit and timeout backoff. */
static inline void rudp_slot_set_fast_state(rudp_slot_s *slot, uint8_t fast_state) {
  slot->fast_retransmit =
      (uint8_t)((slot->fast_retransmit & 0xF8U) | (fast_state & 0x07U));
}

/** Extracts the 4-bit adaptive timeout-backoff exponent (0..15) stored in the slot. */
static inline uint8_t rudp_slot_backoff(const rudp_slot_s *slot) {
  return (uint8_t)(slot->fast_retransmit >> RUDP_SLOT_BACKOFF_SHIFT);
}

/** Updates the 4-bit adaptive timeout-backoff exponent (0..15) stored in the slot. */
static inline void rudp_slot_set_backoff(rudp_slot_s *slot, uint8_t backoff) {
  slot->fast_retransmit =
      (uint8_t)((slot->fast_retransmit & RUDP_FAST_STATE_MASK) |
                ((backoff & 0x0FU) << RUDP_SLOT_BACKOFF_SHIFT));
}

/**
 * @brief Represents the full RUDP context / protocol state machine.
 */
typedef struct {
  rudp_slot_s tx_buffer[RUDP_WINDOW_SIZE]; /**< Sliding transmission window
                                              (ring buffer) */
  uint16_t head;                           /**< Next write index in tx_buffer */
  uint16_t tail;                           /**< Oldest unacknowledged in-flight index */
  uint16_t current_seq_num;    /**< Next sequence number to assign for outgoing
                                  packets */
  uint16_t expected_seq_num;   /**< Next expected sequence number for incoming
                                  packets */
  uint16_t last_ack_received;  /**< Last ACK number received from peer */
  uint8_t duplicate_ack_count; /**< Count of duplicate ACKs received (Fast
                                  Retransmit) */
  uint8_t state;               /**< Connection lifecycle state */
  uint32_t last_rx_time; /**< Timestamp of last received packet/ACK for liveness */
} rudp_context_s;

/**
 * @brief Represents the result of a RUDP tick operation.
 */
typedef struct {
  int count;  /**< Count of already-collected expired indices (>= 0) */
  int status; /**< RUDP_OK (0), RUDP_ERR_DISCONNECTED (-2), RUDP_ERR_INVALID_ARG
                 (-1) */
} rudp_tick_result_s;

/**
 * @brief Datagram Header (4 bytes, sent once per UDP datagram).
 *        If count == 0, this datagram represents a pure Standalone ACK.
 */
typedef struct {
  uint16_t ack;        /**< Piggybacked cumulative ACK sequence number */
  uint8_t ack_channel; /**< Channel ID to which the ACK applies */
  uint8_t count;       /**< Number of bundled records in this datagram (0 = Standalone
                          ACK) */
} rudp_datagram_header_s;

/**
 * @brief Individual Message Record (8 bytes, repeated 'count' times in
 * datagram).
 */
typedef struct {
  uint8_t channel_id;   /**< Target channel identifier (0..RUDP_MAX_CHANNELS-1) */
  uint8_t flags;        /**< Record flags (RUDP_RECORD_FLAG_*) */
  uint16_t seq_num;     /**< 16-bit sequence (reliable sliding window or unreliable
                           sequence) */
  tfv_packet_u payload; /**< 4-byte game payload */
} rudp_record_s;

/** Caller-owned reliable RX storage. One object per attached channel.
 * Do not share or modify while attached; detach before ending its lifetime. */
typedef struct {
  tfv_packet_u packets[RUDP_WINDOW_SIZE];
  uint8_t present[RUDP_RX_BITMAP_SIZE];
} rudp_rx_storage_s;

/**
 * @brief Represents an individual logical communication channel.
 */
typedef struct {
  uint8_t flags;                /**< Combination of RUDP_CHANNEL_FLAG_* */
  uint8_t channel_id;           /**< Channel identifier (0 to RUDP_MAX_CHANNELS - 1) */
  uint16_t last_unreliable_seq; /**< Last accepted unreliable sequence number
                                   (RX anti-rollback) */
  uint8_t has_unreliable_seq;   /**< Flag: 1 if last_unreliable_seq is
                                   initialized, 0 otherwise */
  uint8_t ack_pending;    /**< Flag: 1 if new reliable packet received needing ACK */
  uint16_t last_ack_sent; /**< Last cumulative ACK transmitted for this channel */
  uint16_t next_unreliable_seq; /**< Next unreliable sequence number to transmit
                                   (TX) */
  uint8_t priority;             /**< Lower values preempt background data; equal values
                                   round-robin. */
  uint8_t dscp;                 /**< Optional socket integration hint, 0..63. */
  rudp_context_s ctx; /**< Dedicated sliding window context (used if RELIABLE) */
#if RUDP_EXTERNAL_RX_STORAGE
  tfv_packet_u *rx_buffer;
  uint8_t *rx_present;
#else
  tfv_packet_u rx_buffer[RUDP_WINDOW_SIZE];
  uint8_t rx_present[RUDP_RX_BITMAP_SIZE];
#endif
  uint32_t srtt_scaled, rttvar_scaled; /**< Fixed point: RTT x8, deviation x4. */
  uint32_t last_rtt_sample;
  uint16_t rto_ms, rto_min_ms, rto_max_ms, rto_initial_ms;
  uint16_t unrel_candidate_seq;
  uint8_t unrel_ahead_streak; /**< Bits 0..2: streak; channels[0] bits 3..7: MTU lo */
  uint8_t chan_backoff;       /**< Bits 0..3: backoff; bit 4: gap ACK; ch0 5..7: MTU hi */
} rudp_channel_s;

/**
 * @brief Optional caller-owned saturating counters for observability and diagnostics.
 *        Zero bytes are stored inside rudp_session_s; pass to *_ex() when desired.
 */
typedef struct {
  uint32_t rx_datagrams;                /**< Validated datagrams accepted */
  uint32_t rx_malformed;                /**< Datagrams rejected during Pass-1 validation */
  uint32_t rx_ack_rejected;             /**< Stale, future, or unsent-slot ACKs rejected */
  uint32_t rx_out_of_window;            /**< Reliable records dropped outside RX window */
  uint32_t rx_unreliable_jump_rejected; /**< Implausible unreliable sequence jumps filtered */
  uint32_t rx_unreliable_resyncs;       /**< Unreliable baseline re-anchors after outage */
  uint32_t rx_sack_blocks;              /**< Out-of-order TX slots marked via SACK */
  uint32_t rx_fec_recoveries;           /**< Missing reliable records reconstructed via K=2 XOR */
} rudp_stats_s;

/**
 * @brief Represents a peer session holding multiple independent channels.
 */
typedef struct {
  rudp_channel_s channels[RUDP_MAX_CHANNELS]; /**< Multi-channel array */
  uint8_t active_channels;                    /**< Number of configured channels */
  uint8_t rr_cursor;     /**< Round-robin egress cursor for channel fairness */
  uint16_t session_seed; /**< Seed for path entropy calculation */
} rudp_session_s;

/* Compile-time verification of ABI struct sizes (C11 Static Asserts) */
#ifdef __cplusplus
#define RUDP_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define RUDP_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
RUDP_STATIC_ASSERT(sizeof(rudp_header_s) == 4, "rudp_header_s size must be 4 bytes");
RUDP_STATIC_ASSERT(sizeof(rudp_frame_s) == 8, "rudp_frame_s size must be 8 bytes");
RUDP_STATIC_ASSERT(sizeof(rudp_slot_s) == 16, "rudp_slot_s size must be 16 bytes");
RUDP_STATIC_ASSERT(sizeof(rudp_datagram_header_s) == 4,
               "rudp_datagram_header_s size must be 4 bytes");
RUDP_STATIC_ASSERT(sizeof(rudp_record_s) == 8, "rudp_record_s size must be 8 bytes");
#if !RUDP_EXTERNAL_RX_STORAGE && RUDP_WINDOW_SIZE == 64 && RUDP_MAX_CHANNELS == 4
RUDP_STATIC_ASSERT(sizeof(rudp_context_s) == 1040,
               "rudp_context_s size must be 1040 bytes");
RUDP_STATIC_ASSERT(sizeof(rudp_channel_s) == 1340,
               "rudp_channel_s size must be 1340 bytes");
RUDP_STATIC_ASSERT(sizeof(rudp_session_s) == 5364,
               "rudp_session_s size must be 5364 bytes");
#endif
RUDP_STATIC_ASSERT(sizeof(rudp_context_s) ==
                   (sizeof(rudp_slot_s) * (RUDP_WINDOW_SIZE) + 16),
               "rudp_context_s size mismatch");
#if !RUDP_EXTERNAL_RX_STORAGE
RUDP_STATIC_ASSERT(sizeof(rudp_channel_s) ==
                   ((sizeof(rudp_context_s) + 12 + 4 * RUDP_WINDOW_SIZE +
                     (RUDP_WINDOW_SIZE + 7) / 8 + 3) &
                    ~(size_t)3) +
                       24,
               "rudp_channel_s size mismatch");
RUDP_STATIC_ASSERT(sizeof(rudp_session_s) ==
                   (sizeof(rudp_channel_s) * (RUDP_MAX_CHANNELS) + 4),
               "rudp_session_s size mismatch");
#endif
RUDP_STATIC_ASSERT((RUDP_WINDOW_SIZE >= 2) && (RUDP_WINDOW_SIZE <= 16384) &&
                       ((RUDP_WINDOW_SIZE & (RUDP_WINDOW_SIZE - 1)) == 0),
                   "RUDP_WINDOW_SIZE must be a power of 2 in 2..16384");
RUDP_STATIC_ASSERT((RUDP_MAX_CHANNELS >= 1) && (RUDP_MAX_CHANNELS <= 255),
                   "RUDP_MAX_CHANNELS must be in 1..255");

#endif /* RUDP_TYPES_H */
