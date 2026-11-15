#ifndef RUDP_NEXTGEN_H
#define RUDP_NEXTGEN_H

/**
 * @file rudp_nextgen.h
 * @brief Phase 7 next-generation transport extensions (zcrudp::nextgen):
 *        1. Caller-backed Registered Page Pool & 12-byte Extent Descriptors
 *        2. Shared Static Metadata Slot Pool with per-channel QoS quotas
 *        3. SCReAM / L4S One-Way Delay-Gradient Micro-Pacer
 *        4. Adaptive Sliding-Window Convolutional FEC (RFC 8681 style)
 *        5. Multipath Failover, Weighted Spraying & Redundant Path Scheduler
 *
 * Strictly C11, zero dynamic allocation (malloc/free never called).
 */

#include "rudp_config.h"
#include "rudp_io.h"
#include "rudp_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RUDP_PAGE_SIZE
#define RUDP_PAGE_SIZE 4096U
#endif

#ifndef RUDP_PAGE_POOL_PAGES
#define RUDP_PAGE_POOL_PAGES 64U
#endif

#ifndef RUDP_SHARED_POOL_SIZE
#define RUDP_SHARED_POOL_SIZE 256U
#endif

#define RUDP_SHARED_NONE UINT16_MAX

#define RUDP_SHARED_DATA_INLINE 0U
#define RUDP_SHARED_DATA_EXTENT 1U

#define RUDP_SLIDING_FEC_MAX_W 8U

#ifndef RUDP_MAX_PATHS
#define RUDP_MAX_PATHS 4U
#endif

#define RUDP_MP_PRIMARY_FAILOVER 0U
#define RUDP_MP_WEIGHTED_SPRAY 1U
#define RUDP_MP_REDUNDANT 2U
#define RUDP_MP_ECF_SPRAY 3U

/* =========================================================================
 * 1. Registered Page Pool & Portable Packet Plan (12-byte extent ABI)
 * ========================================================================= */

typedef struct {
  uint16_t page_id;
  uint16_t offset;
  uint16_t len;
  uint16_t gen;
  uint32_t _reserved;
} rudp_page_extent_s;

RUDP_STATIC_ASSERT(sizeof(rudp_page_extent_s) == 12,
                   "rudp_page_extent_s must be 12 bytes");

typedef struct {
  uint16_t gen;
  uint16_t proto_refs;
  uint16_t io_refs;
  uint8_t allocated;
  uint8_t release_pending;
} rudp_page_desc_s;

typedef struct {
  uint8_t *backing;
  size_t page_count;
  rudp_page_desc_s descs[RUDP_PAGE_POOL_PAGES];
  uint16_t free_stack[RUDP_PAGE_POOL_PAGES];
  uint16_t free_top;
  uint16_t release_queue[RUDP_PAGE_POOL_PAGES];
  uint16_t release_head;
  uint16_t release_tail;
  uint16_t release_count;
} rudp_page_pool_s;

typedef struct {
  uint8_t inline_wire[RUDP_WIRE_DATAGRAM_HEADER_SIZE +
                      (RUDP_MAX_RECORDS_PER_DATAGRAM * RUDP_WIRE_RECORD_SIZE)];
  rudp_page_extent_s extents[RUDP_IOVEC_MAX];
  uint16_t inline_len;
  uint16_t total_wire_bytes;
  uint8_t record_count;
  uint8_t extent_count;
  uint8_t dscp;
  uint32_t path_entropy;
} rudp_packet_plan_s;

RUDP_EXPORT int rudp_page_pool_init(rudp_page_pool_s *pool, uint8_t *backing,
                                    size_t page_count);
RUDP_EXPORT int rudp_page_alloc(rudp_page_pool_s *pool, uint16_t len,
                                rudp_page_extent_s *out_extent);
RUDP_EXPORT int rudp_page_subextent(const rudp_page_pool_s *pool,
                                    rudp_page_extent_s parent, uint16_t sub_offset,
                                    uint16_t sub_len,
                                    rudp_page_extent_s *out_sub);
RUDP_EXPORT const uint8_t *rudp_page_resolve(const rudp_page_pool_s *pool,
                                             rudp_page_extent_s extent);
RUDP_EXPORT uint8_t *rudp_page_resolve_mut(rudp_page_pool_s *pool,
                                           rudp_page_extent_s extent);
RUDP_EXPORT int rudp_page_retain_proto(rudp_page_pool_s *pool,
                                       rudp_page_extent_s extent);
RUDP_EXPORT int rudp_page_release_proto(rudp_page_pool_s *pool,
                                        rudp_page_extent_s extent);
RUDP_EXPORT int rudp_page_retain_io(rudp_page_pool_s *pool,
                                    rudp_page_extent_s extent);
RUDP_EXPORT int rudp_page_release_io(rudp_page_pool_s *pool,
                                     rudp_page_extent_s extent);
RUDP_EXPORT size_t rudp_page_pool_drain_releases(rudp_page_pool_s *pool,
                                                 uint16_t *out_page_ids,
                                                 size_t max_ids);

RUDP_EXPORT int rudp_packet_plan_init(rudp_packet_plan_s *plan, uint8_t dscp,
                                      uint32_t path_entropy);
RUDP_EXPORT int rudp_packet_plan_set_header(rudp_packet_plan_s *plan,
                                            rudp_datagram_header_s hdr);
RUDP_EXPORT int rudp_packet_plan_add_record(rudp_packet_plan_s *plan,
                                            rudp_record_s rec);
RUDP_EXPORT int rudp_packet_plan_add_extent(rudp_packet_plan_s *plan,
                                            const rudp_page_pool_s *pool,
                                            rudp_page_extent_s extent);
RUDP_EXPORT int rudp_packet_plan_to_iovec(const rudp_page_pool_s *pool,
                                          const rudp_packet_plan_s *plan,
                                          rudp_iovec_collection_s *out_col);
RUDP_EXPORT int rudp_packet_plan_flatten(const rudp_page_pool_s *pool,
                                         const rudp_packet_plan_s *plan,
                                         uint8_t *out_buf, size_t max_len);

/* =========================================================================
 * 2. Shared Static Metadata Slot Pool (Asymmetric Per-Channel Quotas)
 * ========================================================================= */

typedef struct {
  uint16_t min_reserved;
  uint16_t max_limit;
} rudp_shared_quota_s;

typedef struct {
  uint32_t timestamp;
  uint16_t seq_num;
  uint16_t next_idx;
  uint8_t channel_id;
  uint8_t state;
  uint8_t data_kind;
  uint8_t tx_count;
  tfv_packet_u inline_payload;
  rudp_page_extent_s extent;
} rudp_shared_slot_s;

typedef struct {
  rudp_shared_slot_s slots[RUDP_SHARED_POOL_SIZE];
  rudp_shared_quota_s quotas[RUDP_MAX_CHANNELS];
  uint16_t chan_used[RUDP_MAX_CHANNELS];
  uint16_t chan_head[RUDP_MAX_CHANNELS];
  uint16_t chan_tail[RUDP_MAX_CHANNELS];
  uint16_t chan_next_seq[RUDP_MAX_CHANNELS];
  uint16_t chan_last_ack[RUDP_MAX_CHANNELS];
  uint16_t free_head;
  uint16_t total_used;
  rudp_page_pool_s *page_pool;
} rudp_shared_pool_s;

RUDP_EXPORT int rudp_shared_pool_init(rudp_shared_pool_s *pool,
                                      rudp_page_pool_s *page_pool);
RUDP_EXPORT int rudp_shared_pool_set_quota(rudp_shared_pool_s *pool,
                                           uint8_t channel_id,
                                           uint16_t min_reserved,
                                           uint16_t max_limit);
RUDP_EXPORT int rudp_shared_pool_enqueue_inline(rudp_shared_pool_s *pool,
                                                uint8_t channel_id,
                                                tfv_packet_u payload,
                                                uint32_t now_ms,
                                                uint16_t *out_seq);
RUDP_EXPORT int rudp_shared_pool_enqueue_extent(rudp_shared_pool_s *pool,
                                                uint8_t channel_id,
                                                rudp_page_extent_s extent,
                                                uint32_t now_ms,
                                                uint16_t *out_seq);
RUDP_EXPORT int rudp_shared_pool_ack(rudp_shared_pool_s *pool, uint8_t channel_id,
                                     uint16_t ack_seq, uint32_t sack_mask);
RUDP_EXPORT uint16_t rudp_shared_pool_chan_used(const rudp_shared_pool_s *pool,
                                                uint8_t channel_id);
RUDP_EXPORT uint16_t rudp_shared_pool_total_used(const rudp_shared_pool_s *pool);

/* =========================================================================
 * 3. SCReAM / L4S Delay-Gradient Micro-Pacer
 * ========================================================================= */

typedef struct {
  uint32_t rate_bytes_per_sec;
  uint32_t min_rate_bytes_per_sec;
  uint32_t max_rate_bytes_per_sec;
  uint32_t cwnd_bytes;
  uint32_t inflight_bytes;
  uint32_t base_owd_us;
  uint32_t epoch_min_owd_us;
  uint32_t prev_epoch_min_owd_us;
  uint32_t smooth_owd_us;
  uint32_t prev_smooth_owd_us;
  uint32_t target_queue_us;
  uint64_t token_fp; /**< Q16 bytes */
  uint64_t last_refill_us;
  uint64_t last_reduce_us;
  uint8_t owd_initialized;
  uint8_t epoch_samples;
  uint8_t has_reduced;
} rudp_pacer_s;

RUDP_EXPORT int rudp_pacer_init(rudp_pacer_s *pacer,
                                uint32_t initial_bytes_per_sec,
                                uint32_t min_bytes_per_sec,
                                uint32_t max_bytes_per_sec,
                                uint32_t target_queue_us, uint64_t now_us);
RUDP_EXPORT int rudp_pacer_on_owd_sample(rudp_pacer_s *pacer, uint32_t owd_us,
                                         uint32_t acked_bytes,
                                         uint8_t ce_marked);
RUDP_EXPORT bool rudp_pacer_refill_and_can_send(rudp_pacer_s *pacer,
                                                uint64_t now_us,
                                                uint32_t packet_bytes);
RUDP_EXPORT int rudp_pacer_consume(rudp_pacer_s *pacer, uint32_t packet_bytes);
RUDP_EXPORT uint32_t rudp_pacer_next_wait_us(const rudp_pacer_s *pacer,
                                             uint32_t packet_bytes);

/* =========================================================================
 * 4. Sliding-Window Convolutional FEC (RFC 8681 style)
 * ========================================================================= */

typedef struct {
  uint16_t base_seq;
  uint8_t span;
  uint8_t valid;
  tfv_packet_u xor_payload;
} rudp_sliding_fec_repair_s;

typedef struct {
  tfv_packet_u payloads[RUDP_SLIDING_FEC_MAX_W];
  uint16_t seqs[RUDP_SLIDING_FEC_MAX_W];
  uint8_t present[RUDP_SLIDING_FEC_MAX_W];
  uint8_t window_w;
  uint8_t parity_interval;
  uint8_t since_last_repair;
  uint16_t loss_permille;
} rudp_sliding_fec_s;

typedef struct {
  uint16_t base_seq;
  uint8_t span;
  uint8_t valid;
  uint32_t rhs_raw;
} rudp_sliding_fec_eq_s;

typedef struct {
  tfv_packet_u payloads[RUDP_SLIDING_FEC_MAX_W * 2U];
  uint16_t seqs[RUDP_SLIDING_FEC_MAX_W * 2U];
  uint8_t present[RUDP_SLIDING_FEC_MAX_W * 2U];
  uint8_t window_w;
  uint8_t eq_cursor;
  rudp_sliding_fec_eq_s pending_eqs[RUDP_SLIDING_FEC_MAX_W];
} rudp_sliding_fec_rx_s;

RUDP_EXPORT int rudp_sliding_fec_init(rudp_sliding_fec_s *fec, uint8_t window_w);
RUDP_EXPORT int rudp_sliding_fec_update_loss(rudp_sliding_fec_s *fec, bool lost);
RUDP_EXPORT int rudp_sliding_fec_encode(rudp_sliding_fec_s *fec, uint16_t seq,
                                        tfv_packet_u payload,
                                        rudp_sliding_fec_repair_s *out_repair);
RUDP_EXPORT int rudp_sliding_fec_rx_init(rudp_sliding_fec_rx_s *rx,
                                         uint8_t window_w);
RUDP_EXPORT int rudp_sliding_fec_rx_source(rudp_sliding_fec_rx_s *rx,
                                           uint16_t seq, tfv_packet_u payload);
RUDP_EXPORT int rudp_sliding_fec_rx_repair(rudp_sliding_fec_rx_s *rx,
                                           const rudp_sliding_fec_repair_s *repair,
                                           uint16_t *out_recovered_seq,
                                           tfv_packet_u *out_recovered_payload);
RUDP_EXPORT bool rudp_sliding_fec_rx_get(const rudp_sliding_fec_rx_s *rx,
                                         uint16_t seq,
                                         tfv_packet_u *out_payload);

/* =========================================================================
 * 5. Multipath Transport Scheduler (Failover, Spray, Redundant)
 * ========================================================================= */

typedef struct {
  uint32_t srtt_ms;
  uint32_t loss_permille;
  uint32_t weight;
  int32_t current_weight;
  uint32_t tx_bytes;
  uint32_t last_feedback_ms;
  uint16_t flow_hint;
  uint8_t active;
  uint8_t degraded;
} rudp_path_state_s;

typedef struct {
  rudp_path_state_s paths[RUDP_MAX_PATHS];
  uint8_t mode;
  uint8_t primary_path;
  uint16_t session_seed;
  uint32_t spray_cursor;
} rudp_multipath_s;

RUDP_EXPORT int rudp_multipath_init(rudp_multipath_s *mp, uint16_t session_seed,
                                    uint8_t mode);
RUDP_EXPORT int rudp_multipath_add_path(rudp_multipath_s *mp, uint8_t path_id,
                                        uint32_t weight, uint16_t flow_hint);
RUDP_EXPORT int rudp_multipath_on_feedback(rudp_multipath_s *mp, uint8_t path_id,
                                           uint32_t rtt_ms, bool lost,
                                           uint32_t now_ms);
RUDP_EXPORT int rudp_multipath_select_paths(
    rudp_multipath_s *mp, uint8_t channel_id, uint32_t packet_bytes,
    uint8_t out_path_ids[RUDP_MAX_PATHS], uint32_t out_entropy[RUDP_MAX_PATHS]);

#ifdef __cplusplus
}
#endif

#endif /* RUDP_NEXTGEN_H */
