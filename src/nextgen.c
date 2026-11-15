#include "rudp_nextgen.h"
#include "rudp_codec.h"
#include <string.h>

#define RUDP_NEXTGEN_MTU_BYTES                                                         \
  (RUDP_DEFAULT_MTU + RUDP_WIRE_DATAGRAM_HEADER_SIZE)

static uint8_t gf256_mul(uint8_t a, uint8_t b) {
  uint8_t p = 0U;
  for (unsigned i = 0U; i < 8U; ++i) {
    uint8_t mask = (uint8_t)(-(int8_t)(b & 1U));
    p ^= (uint8_t)(a & mask);
    uint8_t hi = (uint8_t)(a & 0x80U);
    a = (uint8_t)(a << 1U);
    if (hi != 0U) {
      a ^= 0x1DU;
    }
    b = (uint8_t)(b >> 1U);
  }
  return p;
}

static uint8_t gf256_inv(uint8_t a) {
  if (a <= 1U) {
    return a;
  }
  uint8_t acc = 1U;
  uint8_t p = a;
  for (unsigned bit = 1U; bit < 8U; ++bit) {
    p = gf256_mul(p, p);
    acc = gf256_mul(acc, p);
  }
  return acc;
}

static inline uint32_t gf256_mul_u32(uint32_t v, uint8_t c) {
  uint32_t out = 0U;
  for (unsigned b = 0U; b < 4U; ++b) {
    uint8_t lane = (uint8_t)((v >> (b * 8U)) & 0xFFU);
    out |= (uint32_t)gf256_mul(lane, c) << (b * 8U);
  }
  return out;
}

static inline uint8_t cauchy_coeff(uint16_t repair_seq, uint16_t src_seq) {
  uint8_t x = (uint8_t)(repair_seq & 0x7FU);
  uint8_t y = (uint8_t)(0x80U | (src_seq & 0x7FU));
  return gf256_inv((uint8_t)(x ^ y));
}

/* =========================================================================
 * 1. Registered Page Pool & Portable Packet Plan
 * ========================================================================= */

static bool page_extent_valid(const rudp_page_pool_s *pool,
                              rudp_page_extent_s extent) {
  if (!pool || !pool->backing || extent.page_id >= pool->page_count) {
    return false;
  }
  const rudp_page_desc_s *d = &pool->descs[extent.page_id];
  if (!d->allocated || d->release_pending || d->gen != extent.gen) {
    return false;
  }
  if (extent.len == 0U) {
    return false;
  }
  if ((size_t)extent.offset + (size_t)extent.len > RUDP_PAGE_SIZE) {
    return false;
  }
  return true;
}

static void maybe_enqueue_page_release(rudp_page_pool_s *pool, uint16_t page_id) {
  rudp_page_desc_s *d = &pool->descs[page_id];
  if (d->allocated && !d->release_pending && d->proto_refs == 0U && d->io_refs == 0U) {
    d->release_pending = 1U;
    pool->release_queue[pool->release_tail] = page_id;
    pool->release_tail =
        (uint16_t)((pool->release_tail + 1U) % RUDP_PAGE_POOL_PAGES);
    pool->release_count++;
  }
}

int rudp_page_pool_init(rudp_page_pool_s *pool, uint8_t *backing, size_t page_count) {
  if (!pool || !backing || page_count == 0U || page_count > RUDP_PAGE_POOL_PAGES) {
    return RUDP_ERR_INVALID_ARG;
  }
  memset(pool, 0, sizeof(*pool));
  pool->backing = backing;
  pool->page_count = page_count;
  for (size_t i = 0; i < page_count; ++i) {
    size_t rev = page_count - 1U - i;
    pool->descs[i].gen = 1U;
    pool->free_stack[i] = (uint16_t)rev;
  }
  pool->free_top = (uint16_t)page_count;
  return RUDP_OK;
}

int rudp_page_alloc(rudp_page_pool_s *pool, uint16_t len,
                    rudp_page_extent_s *out_extent) {
  if (!pool || !pool->backing || !out_extent || len == 0U || len > RUDP_PAGE_SIZE) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (pool->free_top == 0U) {
    return RUDP_ERR_BUFFER_FULL;
  }
  uint16_t id = pool->free_stack[--pool->free_top];
  rudp_page_desc_s *d = &pool->descs[id];
  d->allocated = 1U;
  d->release_pending = 0U;
  d->proto_refs = 1U;
  d->io_refs = 0U;
  *out_extent = (rudp_page_extent_s){
      .page_id = id,
      .offset = 0U,
      .len = len,
      .gen = d->gen,
      ._reserved = 0U,
  };
  return RUDP_OK;
}

int rudp_page_subextent(const rudp_page_pool_s *pool, rudp_page_extent_s parent,
                        uint16_t sub_offset, uint16_t sub_len,
                        rudp_page_extent_s *out_sub) {
  if (!out_sub || !page_extent_valid(pool, parent) || sub_len == 0U) {
    return RUDP_ERR_INVALID_ARG;
  }
  if ((size_t)sub_offset + (size_t)sub_len > (size_t)parent.len) {
    return RUDP_ERR_INVALID_ARG;
  }
  *out_sub = (rudp_page_extent_s){
      .page_id = parent.page_id,
      .offset = (uint16_t)(parent.offset + sub_offset),
      .len = sub_len,
      .gen = parent.gen,
      ._reserved = 0U,
  };
  return RUDP_OK;
}

const uint8_t *rudp_page_resolve(const rudp_page_pool_s *pool,
                                 rudp_page_extent_s extent) {
  if (!page_extent_valid(pool, extent)) {
    return NULL;
  }
  size_t byte_off =
      ((size_t)extent.page_id * RUDP_PAGE_SIZE) + (size_t)extent.offset;
  return pool->backing + byte_off;
}

uint8_t *rudp_page_resolve_mut(rudp_page_pool_s *pool, rudp_page_extent_s extent) {
  if (!page_extent_valid(pool, extent)) {
    return NULL;
  }
  size_t byte_off =
      ((size_t)extent.page_id * RUDP_PAGE_SIZE) + (size_t)extent.offset;
  return pool->backing + byte_off;
}

int rudp_page_retain_proto(rudp_page_pool_s *pool, rudp_page_extent_s extent) {
  if (!page_extent_valid(pool, extent)) {
    return RUDP_ERR_INVALID_ARG;
  }
  rudp_page_desc_s *d = &pool->descs[extent.page_id];
  if (d->proto_refs == UINT16_MAX) {
    return RUDP_ERR_BUFFER_FULL;
  }
  d->proto_refs++;
  return RUDP_OK;
}

int rudp_page_release_proto(rudp_page_pool_s *pool, rudp_page_extent_s extent) {
  if (!page_extent_valid(pool, extent)) {
    return RUDP_ERR_INVALID_ARG;
  }
  rudp_page_desc_s *d = &pool->descs[extent.page_id];
  if (d->proto_refs == 0U) {
    return RUDP_ERR_INVALID_ARG;
  }
  d->proto_refs--;
  maybe_enqueue_page_release(pool, extent.page_id);
  return RUDP_OK;
}

int rudp_page_retain_io(rudp_page_pool_s *pool, rudp_page_extent_s extent) {
  if (!page_extent_valid(pool, extent)) {
    return RUDP_ERR_INVALID_ARG;
  }
  rudp_page_desc_s *d = &pool->descs[extent.page_id];
  if (d->io_refs == UINT16_MAX) {
    return RUDP_ERR_BUFFER_FULL;
  }
  d->io_refs++;
  return RUDP_OK;
}

int rudp_page_release_io(rudp_page_pool_s *pool, rudp_page_extent_s extent) {
  if (!page_extent_valid(pool, extent)) {
    return RUDP_ERR_INVALID_ARG;
  }
  rudp_page_desc_s *d = &pool->descs[extent.page_id];
  if (d->io_refs == 0U) {
    return RUDP_ERR_INVALID_ARG;
  }
  d->io_refs--;
  maybe_enqueue_page_release(pool, extent.page_id);
  return RUDP_OK;
}

size_t rudp_page_pool_drain_releases(rudp_page_pool_s *pool, uint16_t *out_page_ids,
                                     size_t max_ids) {
  if (!pool) {
    return 0U;
  }
  size_t drained = 0U;
  while (pool->release_count > 0U && (!out_page_ids || drained < max_ids)) {
    uint16_t id = pool->release_queue[pool->release_head];
    pool->release_head =
        (uint16_t)((pool->release_head + 1U) % RUDP_PAGE_POOL_PAGES);
    pool->release_count--;

    rudp_page_desc_s *d = &pool->descs[id];
    d->allocated = 0U;
    d->release_pending = 0U;
    d->gen = (uint16_t)(d->gen + 1U);
    if (d->gen == 0U) {
      d->gen = 1U;
    }
    if (pool->free_top < pool->page_count) {
      pool->free_stack[pool->free_top++] = id;
    }
    if (out_page_ids) {
      out_page_ids[drained] = id;
    }
    drained++;
  }
  return drained;
}

int rudp_packet_plan_init(rudp_packet_plan_s *plan, uint8_t dscp,
                          uint32_t path_entropy) {
  if (!plan || dscp > 63U) {
    return RUDP_ERR_INVALID_ARG;
  }
  memset(plan, 0, sizeof(*plan));
  plan->inline_len = RUDP_WIRE_DATAGRAM_HEADER_SIZE;
  plan->total_wire_bytes = RUDP_WIRE_DATAGRAM_HEADER_SIZE;
  plan->dscp = dscp;
  plan->path_entropy = path_entropy;
  return RUDP_OK;
}

int rudp_packet_plan_set_header(rudp_packet_plan_s *plan,
                                rudp_datagram_header_s hdr) {
  if (!plan) {
    return RUDP_ERR_INVALID_ARG;
  }
  int ret = rudp_pack_datagram_header(&hdr, plan->inline_wire,
                                      RUDP_WIRE_DATAGRAM_HEADER_SIZE);
  return (ret == RUDP_WIRE_DATAGRAM_HEADER_SIZE) ? RUDP_OK : ret;
}

int rudp_packet_plan_add_record(rudp_packet_plan_s *plan, rudp_record_s rec) {
  if (!plan) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (plan->record_count >= RUDP_MAX_RECORDS_PER_DATAGRAM ||
      (size_t)plan->inline_len + RUDP_WIRE_RECORD_SIZE > sizeof(plan->inline_wire) ||
      plan->total_wire_bytes > UINT16_MAX - RUDP_WIRE_RECORD_SIZE) {
    return RUDP_ERR_BUFFER_FULL;
  }
  int ret = rudp_pack_record(&rec, plan->inline_wire + plan->inline_len,
                             sizeof(plan->inline_wire) - plan->inline_len);
  if (ret != RUDP_WIRE_RECORD_SIZE) {
    return ret;
  }
  plan->inline_len = (uint16_t)(plan->inline_len + RUDP_WIRE_RECORD_SIZE);
  plan->total_wire_bytes = (uint16_t)(plan->total_wire_bytes + RUDP_WIRE_RECORD_SIZE);
  plan->record_count++;
  return RUDP_OK;
}

int rudp_packet_plan_add_extent(rudp_packet_plan_s *plan,
                                const rudp_page_pool_s *pool,
                                rudp_page_extent_s extent) {
  if (!plan || !page_extent_valid(pool, extent)) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (plan->extent_count >= (uint8_t)(RUDP_IOVEC_MAX - 1U) ||
      extent.len > (uint16_t)(UINT16_MAX - plan->total_wire_bytes)) {
    return RUDP_ERR_BUFFER_FULL;
  }
  plan->extents[plan->extent_count++] = extent;
  plan->total_wire_bytes = (uint16_t)(plan->total_wire_bytes + extent.len);
  return RUDP_OK;
}

int rudp_packet_plan_to_iovec(const rudp_page_pool_s *pool,
                              const rudp_packet_plan_s *plan,
                              rudp_iovec_collection_s *out_col) {
  if (!pool || !plan || !out_col || plan->inline_len < RUDP_WIRE_DATAGRAM_HEADER_SIZE ||
      plan->inline_len > sizeof(plan->inline_wire) ||
      plan->extent_count >= RUDP_IOVEC_MAX) {
    return RUDP_ERR_INVALID_ARG;
  }
  rudp_iovec_reset(out_col);
  int ret = rudp_iovec_add(out_col, plan->inline_wire, plan->inline_len);
  if (ret != RUDP_OK) {
    return ret;
  }
  for (uint8_t i = 0U; i < plan->extent_count; ++i) {
    const uint8_t *ptr = rudp_page_resolve(pool, plan->extents[i]);
    if (!ptr) {
      rudp_iovec_reset(out_col);
      return RUDP_ERR_INVALID_ARG;
    }
    ret = rudp_iovec_add(out_col, ptr, plan->extents[i].len);
    if (ret != RUDP_OK) {
      rudp_iovec_reset(out_col);
      return ret;
    }
  }
  out_col->dscp = plan->dscp;
  out_col->path_entropy = plan->path_entropy;
  return RUDP_OK;
}

int rudp_packet_plan_flatten(const rudp_page_pool_s *pool,
                             const rudp_packet_plan_s *plan, uint8_t *out_buf,
                             size_t max_len) {
  rudp_iovec_collection_s col;
  int ret = rudp_packet_plan_to_iovec(pool, plan, &col);
  if (ret != RUDP_OK) {
    return ret;
  }
  return rudp_iovec_flatten(&col, out_buf, max_len);
}

/* =========================================================================
 * 2. Shared Static Metadata Slot Pool
 * ========================================================================= */

int rudp_shared_pool_init(rudp_shared_pool_s *pool, rudp_page_pool_s *page_pool) {
  if (!pool) {
    return RUDP_ERR_INVALID_ARG;
  }
  memset(pool, 0, sizeof(*pool));
  pool->page_pool = page_pool;
  for (uint16_t i = 0U; i < RUDP_SHARED_POOL_SIZE; ++i) {
    pool->slots[i].state = RUDP_SLOT_FREE;
    pool->slots[i].next_idx =
        (i + 1U < RUDP_SHARED_POOL_SIZE) ? (uint16_t)(i + 1U) : RUDP_SHARED_NONE;
  }
  pool->free_head = 0U;

  uint16_t base_min = (RUDP_SHARED_POOL_SIZE >= (uint16_t)(RUDP_MAX_CHANNELS * 8U))
                          ? 8U
                          : (uint16_t)(RUDP_SHARED_POOL_SIZE / RUDP_MAX_CHANNELS);
  for (uint8_t c = 0U; c < RUDP_MAX_CHANNELS; ++c) {
    uint16_t def_max = (c == 1U && RUDP_SHARED_POOL_SIZE >= 192U)
                           ? 192U
                           : (uint16_t)(RUDP_SHARED_POOL_SIZE / 4U);
    if (def_max < base_min) {
      def_max = base_min;
    }
    pool->quotas[c] = (rudp_shared_quota_s){
        .min_reserved = base_min,
        .max_limit = def_max,
    };
    pool->chan_head[c] = RUDP_SHARED_NONE;
    pool->chan_tail[c] = RUDP_SHARED_NONE;
  }
  return RUDP_OK;
}

int rudp_shared_pool_set_quota(rudp_shared_pool_s *pool, uint8_t channel_id,
                               uint16_t min_reserved, uint16_t max_limit) {
  if (!pool || channel_id >= RUDP_MAX_CHANNELS || min_reserved > max_limit ||
      max_limit > RUDP_SHARED_POOL_SIZE || pool->chan_used[channel_id] > max_limit) {
    return RUDP_ERR_INVALID_ARG;
  }
  uint32_t total_min = min_reserved;
  for (uint8_t c = 0U; c < RUDP_MAX_CHANNELS; ++c) {
    if (c != channel_id) {
      total_min += pool->quotas[c].min_reserved;
    }
  }
  if (total_min > RUDP_SHARED_POOL_SIZE) {
    return RUDP_ERR_INVALID_ARG;
  }
  pool->quotas[channel_id].min_reserved = min_reserved;
  pool->quotas[channel_id].max_limit = max_limit;
  return RUDP_OK;
}

static int shared_pool_alloc_slot(rudp_shared_pool_s *pool, uint8_t ch,
                                  uint16_t *out_idx) {
  if (ch >= RUDP_MAX_CHANNELS || pool->free_head == RUDP_SHARED_NONE) {
    return RUDP_ERR_BUFFER_FULL;
  }
  if (pool->chan_used[ch] >= pool->quotas[ch].max_limit) {
    return RUDP_ERR_BUFFER_FULL;
  }
  uint16_t global_free = (uint16_t)(RUDP_SHARED_POOL_SIZE - pool->total_used);
  if (pool->chan_used[ch] >= pool->quotas[ch].min_reserved) {
    uint16_t reserved_others = 0U;
    for (uint8_t j = 0U; j < RUDP_MAX_CHANNELS; ++j) {
      if (j != ch && pool->chan_used[j] < pool->quotas[j].min_reserved) {
        reserved_others =
            (uint16_t)(reserved_others +
                       (pool->quotas[j].min_reserved - pool->chan_used[j]));
      }
    }
    if (global_free <= reserved_others) {
      return RUDP_ERR_BUFFER_FULL;
    }
  }

  uint16_t idx = pool->free_head;
  pool->free_head = pool->slots[idx].next_idx;
  pool->slots[idx].next_idx = RUDP_SHARED_NONE;
  if (pool->chan_head[ch] == RUDP_SHARED_NONE) {
    pool->chan_head[ch] = idx;
    pool->chan_tail[ch] = idx;
  } else {
    pool->slots[pool->chan_tail[ch]].next_idx = idx;
    pool->chan_tail[ch] = idx;
  }
  pool->chan_used[ch]++;
  pool->total_used++;
  *out_idx = idx;
  return RUDP_OK;
}

static void shared_slot_release_extent_ref(rudp_shared_pool_s *pool,
                                           rudp_shared_slot_s *slot) {
  if (slot->data_kind == RUDP_SHARED_DATA_EXTENT && pool->page_pool) {
    (void)rudp_page_release_proto(pool->page_pool, slot->extent);
    slot->data_kind = RUDP_SHARED_DATA_INLINE;
  }
}

int rudp_shared_pool_enqueue_inline(rudp_shared_pool_s *pool, uint8_t channel_id,
                                    tfv_packet_u payload, uint32_t now_ms,
                                    uint16_t *out_seq) {
  if (!pool || channel_id >= RUDP_MAX_CHANNELS) {
    return RUDP_ERR_INVALID_ARG;
  }
  uint16_t idx = 0U;
  int ret = shared_pool_alloc_slot(pool, channel_id, &idx);
  if (ret != RUDP_OK) {
    return ret;
  }
  uint16_t seq = pool->chan_next_seq[channel_id]++;
  pool->slots[idx] = (rudp_shared_slot_s){
      .timestamp = now_ms,
      .seq_num = seq,
      .next_idx = RUDP_SHARED_NONE,
      .channel_id = channel_id,
      .state = RUDP_SLOT_IN_FLIGHT,
      .data_kind = RUDP_SHARED_DATA_INLINE,
      .tx_count = 1U,
      .inline_payload = payload,
      .extent = {0},
  };
  if (out_seq) {
    *out_seq = seq;
  }
  return RUDP_OK;
}

int rudp_shared_pool_enqueue_extent(rudp_shared_pool_s *pool, uint8_t channel_id,
                                    rudp_page_extent_s extent, uint32_t now_ms,
                                    uint16_t *out_seq) {
  if (!pool || !pool->page_pool || channel_id >= RUDP_MAX_CHANNELS ||
      !page_extent_valid(pool->page_pool, extent)) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (pool->page_pool->descs[extent.page_id].proto_refs == UINT16_MAX) {
    return RUDP_ERR_BUFFER_FULL;
  }
  uint16_t idx = 0U;
  int ret = shared_pool_alloc_slot(pool, channel_id, &idx);
  if (ret != RUDP_OK) {
    return ret;
  }
  pool->page_pool->descs[extent.page_id].proto_refs++;
  uint16_t seq = pool->chan_next_seq[channel_id]++;
  pool->slots[idx] = (rudp_shared_slot_s){
      .timestamp = now_ms,
      .seq_num = seq,
      .next_idx = RUDP_SHARED_NONE,
      .channel_id = channel_id,
      .state = RUDP_SLOT_IN_FLIGHT,
      .data_kind = RUDP_SHARED_DATA_EXTENT,
      .tx_count = 1U,
      .inline_payload = {.raw = 0U},
      .extent = extent,
  };
  if (out_seq) {
    *out_seq = seq;
  }
  return RUDP_OK;
}

int rudp_shared_pool_ack(rudp_shared_pool_s *pool, uint8_t channel_id,
                         uint16_t ack_seq, uint32_t sack_mask) {
  if (!pool || channel_id >= RUDP_MAX_CHANNELS) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (pool->chan_head[channel_id] == RUDP_SHARED_NONE) {
    return (ack_seq == pool->chan_next_seq[channel_id]) ? 0 : RUDP_ERR_INVALID_ARG;
  }

  uint16_t head_seq = pool->slots[pool->chan_head[channel_id]].seq_num;
  int16_t ack_diff = (int16_t)(ack_seq - head_seq);
  int16_t sent_span = (int16_t)(pool->chan_next_seq[channel_id] - head_seq);
  if (ack_diff < 0 || ack_diff > sent_span) {
    return RUDP_ERR_INVALID_ARG;
  }

  int freed = 0;
  while (pool->chan_head[channel_id] != RUDP_SHARED_NONE) {
    uint16_t cur = pool->chan_head[channel_id];
    rudp_shared_slot_s *slot = &pool->slots[cur];
    if ((int16_t)(ack_seq - slot->seq_num) <= 0) {
      break;
    }
    pool->chan_head[channel_id] = slot->next_idx;
    if (pool->chan_head[channel_id] == RUDP_SHARED_NONE) {
      pool->chan_tail[channel_id] = RUDP_SHARED_NONE;
    }
    shared_slot_release_extent_ref(pool, slot);
    slot->state = RUDP_SLOT_FREE;
    slot->next_idx = pool->free_head;
    pool->free_head = cur;
    pool->chan_used[channel_id]--;
    pool->total_used--;
    freed++;
  }
  pool->chan_last_ack[channel_id] = ack_seq;

  if (sack_mask != 0U && pool->chan_head[channel_id] != RUDP_SHARED_NONE) {
    uint16_t cur = pool->chan_head[channel_id];
    while (cur != RUDP_SHARED_NONE) {
      rudp_shared_slot_s *slot = &pool->slots[cur];
      int16_t rel = (int16_t)(slot->seq_num - ack_seq);
      if (rel >= 1 && rel <= 32) {
        uint32_t bit = UINT32_C(1) << (unsigned)(rel - 1);
        if ((sack_mask & bit) && slot->state == RUDP_SLOT_IN_FLIGHT) {
          slot->state = RUDP_SLOT_SACKED;
          shared_slot_release_extent_ref(pool, slot);
        }
      }
      cur = slot->next_idx;
    }
  }
  return freed;
}

uint16_t rudp_shared_pool_chan_used(const rudp_shared_pool_s *pool,
                                    uint8_t channel_id) {
  if (!pool || channel_id >= RUDP_MAX_CHANNELS) {
    return 0U;
  }
  return pool->chan_used[channel_id];
}

uint16_t rudp_shared_pool_total_used(const rudp_shared_pool_s *pool) {
  return pool ? pool->total_used : 0U;
}

/* =========================================================================
 * 3. SCReAM / L4S Delay-Gradient Micro-Pacer
 * ========================================================================= */

static uint32_t pacer_burst_cap_bytes(uint32_t rate_bps) {
  uint32_t two_mtu = (uint32_t)(2U * RUDP_NEXTGEN_MTU_BYTES);
  uint32_t burst_20ms = rate_bps / 50U;
  return (burst_20ms > two_mtu) ? burst_20ms : two_mtu;
}

int rudp_pacer_init(rudp_pacer_s *pacer, uint32_t initial_bytes_per_sec,
                    uint32_t min_bytes_per_sec, uint32_t max_bytes_per_sec,
                    uint32_t target_queue_us, uint64_t now_us) {
  if (!pacer || min_bytes_per_sec == 0U ||
      min_bytes_per_sec > initial_bytes_per_sec ||
      initial_bytes_per_sec > max_bytes_per_sec || target_queue_us == 0U) {
    return RUDP_ERR_INVALID_ARG;
  }
  memset(pacer, 0, sizeof(*pacer));
  pacer->rate_bytes_per_sec = initial_bytes_per_sec;
  pacer->min_rate_bytes_per_sec = min_bytes_per_sec;
  pacer->max_rate_bytes_per_sec = max_bytes_per_sec;
  pacer->target_queue_us = target_queue_us;
  uint32_t min_cwnd = (uint32_t)(4U * RUDP_NEXTGEN_MTU_BYTES);
  uint32_t rate_cwnd = initial_bytes_per_sec / 10U;
  pacer->cwnd_bytes = (rate_cwnd > min_cwnd) ? rate_cwnd : min_cwnd;
  pacer->token_fp = (uint64_t)(2U * RUDP_NEXTGEN_MTU_BYTES) << 16;
  pacer->last_refill_us = now_us;
  return RUDP_OK;
}

int rudp_pacer_on_owd_sample(rudp_pacer_s *pacer, uint32_t owd_us,
                             uint32_t acked_bytes, uint8_t ce_marked) {
  if (!pacer || pacer->min_rate_bytes_per_sec == 0U) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (acked_bytes >= pacer->inflight_bytes) {
    pacer->inflight_bytes = 0U;
  } else {
    pacer->inflight_bytes -= acked_bytes;
  }

  if (!pacer->owd_initialized) {
    pacer->base_owd_us = owd_us;
    pacer->epoch_min_owd_us = owd_us;
    pacer->prev_epoch_min_owd_us = owd_us;
    pacer->smooth_owd_us = owd_us;
    pacer->prev_smooth_owd_us = owd_us;
    pacer->owd_initialized = 1U;
    pacer->epoch_samples = 0U;
  } else {
    if (owd_us < pacer->base_owd_us) {
      pacer->base_owd_us = owd_us;
    }
    if (owd_us < pacer->epoch_min_owd_us) {
      pacer->epoch_min_owd_us = owd_us;
    }
    pacer->epoch_samples++;
    if (pacer->epoch_samples >= 32U) {
      uint32_t min2 = (pacer->prev_epoch_min_owd_us < pacer->epoch_min_owd_us)
                          ? pacer->prev_epoch_min_owd_us
                          : pacer->epoch_min_owd_us;
      pacer->base_owd_us = min2;
      pacer->prev_epoch_min_owd_us = pacer->epoch_min_owd_us;
      pacer->epoch_min_owd_us = owd_us;
      pacer->epoch_samples = 0U;
    }
    pacer->prev_smooth_owd_us = pacer->smooth_owd_us;
    pacer->smooth_owd_us =
        (uint32_t)(((uint64_t)pacer->smooth_owd_us * 7ULL + owd_us) / 8ULL);
  }

  uint32_t qdelay = (pacer->smooth_owd_us > pacer->base_owd_us)
                        ? (pacer->smooth_owd_us - pacer->base_owd_us)
                        : 0U;
  int32_t grad_us =
      (int32_t)pacer->smooth_owd_us - (int32_t)pacer->prev_smooth_owd_us;
  uint64_t rtt_guard_us = (uint64_t)pacer->base_owd_us * 2ULL;
  if (rtt_guard_us < 10000ULL) {
    rtt_guard_us = 10000ULL;
  }
  bool time_advancing = (pacer->last_refill_us > pacer->last_reduce_us);
  bool rtt_guarded =
      pacer->has_reduced && time_advancing &&
      ((pacer->last_refill_us - pacer->last_reduce_us) < rtt_guard_us);
  bool can_reduce = (ce_marked != 0U) || !rtt_guarded;
  bool heavy_cong = (ce_marked != 0U) || (qdelay > pacer->target_queue_us);
  bool early_grad =
      (qdelay > ((pacer->target_queue_us * 3U) / 4U)) && (grad_us > 500);

  if (heavy_cong || early_grad) {
    if (can_reduce) {
      uint32_t div = heavy_cong ? 8U : 16U;
      uint32_t drop = pacer->rate_bytes_per_sec / div;
      if (drop < 1024U) {
        drop = 1024U;
      }
      uint32_t next = (pacer->rate_bytes_per_sec > drop)
                          ? (pacer->rate_bytes_per_sec - drop)
                          : pacer->min_rate_bytes_per_sec;
      if (next < pacer->min_rate_bytes_per_sec) {
        next = pacer->min_rate_bytes_per_sec;
      }
      pacer->rate_bytes_per_sec = next;

      uint32_t cwnd_floor = (uint32_t)(2U * RUDP_NEXTGEN_MTU_BYTES);
      uint32_t c_drop = pacer->cwnd_bytes / div;
      pacer->cwnd_bytes =
          (pacer->cwnd_bytes > c_drop) ? (pacer->cwnd_bytes - c_drop) : cwnd_floor;
      if (pacer->cwnd_bytes < cwnd_floor) {
        pacer->cwnd_bytes = cwnd_floor;
      }
      if (ce_marked == 0U) {
        pacer->last_reduce_us = pacer->last_refill_us;
        pacer->has_reduced = 1U;
      }
    }
  } else if (qdelay <= (pacer->target_queue_us / 2U) && grad_us <= 250) {
    uint32_t gain = pacer->rate_bytes_per_sec / 16U;
    if (gain < 1024U) {
      gain = 1024U;
    }
    uint64_t next = (uint64_t)pacer->rate_bytes_per_sec + gain;
    pacer->rate_bytes_per_sec =
        (next > pacer->max_rate_bytes_per_sec)
            ? pacer->max_rate_bytes_per_sec
            : (uint32_t)next;
    uint32_t cwnd_step =
        (acked_bytes > RUDP_NEXTGEN_MTU_BYTES) ? RUDP_NEXTGEN_MTU_BYTES : acked_bytes;
    uint64_t next_cwnd = (uint64_t)pacer->cwnd_bytes + cwnd_step;
    uint64_t max_cwnd = (uint64_t)pacer->max_rate_bytes_per_sec / 4ULL;
    if (max_cwnd < 4ULL * RUDP_NEXTGEN_MTU_BYTES) {
      max_cwnd = 4ULL * RUDP_NEXTGEN_MTU_BYTES;
    }
    pacer->cwnd_bytes = (next_cwnd > max_cwnd) ? (uint32_t)max_cwnd : (uint32_t)next_cwnd;
  }
  return RUDP_OK;
}

bool rudp_pacer_refill_and_can_send(rudp_pacer_s *pacer, uint64_t now_us,
                                    uint32_t packet_bytes) {
  if (!pacer || pacer->rate_bytes_per_sec == 0U || packet_bytes == 0U) {
    return false;
  }
  if (now_us > pacer->last_refill_us) {
    uint64_t elapsed_us = now_us - pacer->last_refill_us;
    if (elapsed_us > 1000000ULL) {
      elapsed_us = 1000000ULL;
    }
    uint64_t prod = elapsed_us * (uint64_t)pacer->rate_bytes_per_sec;
    uint64_t whole = prod / 1000000ULL;
    uint64_t rem = ((prod % 1000000ULL) << 16) / 1000000ULL;
    uint64_t add_fp = (whole << 16) + rem;
    uint64_t cap_fp = (uint64_t)pacer_burst_cap_bytes(pacer->rate_bytes_per_sec) << 16;
    if (pacer->token_fp + add_fp > cap_fp) {
      pacer->token_fp = cap_fp;
    } else {
      pacer->token_fp += add_fp;
    }
    pacer->last_refill_us = now_us;
  }
  uint64_t need_fp = (uint64_t)packet_bytes << 16;
  return (pacer->token_fp >= need_fp) &&
         ((uint64_t)pacer->inflight_bytes + packet_bytes <= pacer->cwnd_bytes);
}

int rudp_pacer_consume(rudp_pacer_s *pacer, uint32_t packet_bytes) {
  if (!pacer || packet_bytes == 0U) {
    return RUDP_ERR_INVALID_ARG;
  }
  uint64_t need_fp = (uint64_t)packet_bytes << 16;
  if (pacer->token_fp < need_fp ||
      (uint64_t)pacer->inflight_bytes + packet_bytes > pacer->cwnd_bytes) {
    return RUDP_ERR_BUFFER_FULL;
  }
  pacer->token_fp -= need_fp;
  pacer->inflight_bytes += packet_bytes;
  return RUDP_OK;
}

uint32_t rudp_pacer_next_wait_us(const rudp_pacer_s *pacer, uint32_t packet_bytes) {
  if (!pacer || pacer->rate_bytes_per_sec == 0U) {
    return UINT32_MAX;
  }
  if ((uint64_t)pacer->inflight_bytes + packet_bytes > pacer->cwnd_bytes) {
    return UINT32_MAX;
  }
  uint64_t need_fp = (uint64_t)packet_bytes << 16;
  if (pacer->token_fp >= need_fp) {
    return 0U;
  }
  uint64_t deficit_fp = need_fp - pacer->token_fp;
  uint64_t rate_fp_per_us_num = deficit_fp * 1000000ULL;
  uint64_t rate_fp = (uint64_t)pacer->rate_bytes_per_sec << 16;
  uint64_t wait_us = (rate_fp_per_us_num + rate_fp - 1ULL) / rate_fp;
  return (wait_us > UINT32_MAX) ? UINT32_MAX : (uint32_t)wait_us;
}

/* =========================================================================
 * 4. Sliding-Window Convolutional FEC (RFC 8681 style)
 * ========================================================================= */

int rudp_sliding_fec_init(rudp_sliding_fec_s *fec, uint8_t window_w) {
  if (!fec || window_w < 2U || window_w > RUDP_SLIDING_FEC_MAX_W) {
    return RUDP_ERR_INVALID_ARG;
  }
  memset(fec, 0, sizeof(*fec));
  fec->window_w = window_w;
  fec->parity_interval = window_w;
  return RUDP_OK;
}

int rudp_sliding_fec_update_loss(rudp_sliding_fec_s *fec, bool lost) {
  if (!fec || fec->window_w < 2U) {
    return RUDP_ERR_INVALID_ARG;
  }
  uint32_t sample = lost ? 1000U : 0U;
  fec->loss_permille =
      (uint16_t)(((uint32_t)fec->loss_permille * 7U + sample) / 8U);

  uint8_t interval = fec->window_w;
  if (fec->loss_permille >= 120U) {
    interval = 2U;
  } else if (fec->loss_permille >= 40U) {
    interval = (fec->window_w > 4U) ? 4U : fec->window_w;
  } else if (fec->loss_permille >= 10U) {
    interval = (fec->window_w > 6U) ? 6U : fec->window_w;
  }
  if (interval < 2U) {
    interval = 2U;
  }
  fec->parity_interval = interval;
  return RUDP_OK;
}

int rudp_sliding_fec_encode(rudp_sliding_fec_s *fec, uint16_t seq,
                            tfv_packet_u payload,
                            rudp_sliding_fec_repair_s *out_repair) {
  if (!fec || !out_repair || fec->window_w < 2U) {
    return RUDP_ERR_INVALID_ARG;
  }
  size_t slot = (size_t)(seq % RUDP_SLIDING_FEC_MAX_W);
  fec->payloads[slot] = payload;
  fec->seqs[slot] = seq;
  fec->present[slot] = 1U;
  fec->since_last_repair++;

  if (fec->since_last_repair < fec->parity_interval) {
    *out_repair = (rudp_sliding_fec_repair_s){0};
    return 0;
  }

  /* Collect contiguous run ending at seq up to window_w and encode via Cauchy GF(2^8) */
  uint8_t span = 0U;
  uint16_t base = seq;
  for (uint8_t back = 0U; back < fec->window_w; ++back) {
    uint16_t cand_seq = (uint16_t)(seq - back);
    size_t idx = (size_t)(cand_seq % RUDP_SLIDING_FEC_MAX_W);
    if (!fec->present[idx] || fec->seqs[idx] != cand_seq) {
      break;
    }
    base = cand_seq;
    span++;
  }
  if (span < 2U) {
    *out_repair = (rudp_sliding_fec_repair_s){0};
    return 0;
  }
  uint32_t acc_raw = 0U;
  for (uint8_t k = 0U; k < span; ++k) {
    uint16_t cand_seq = (uint16_t)(base + k);
    size_t idx = (size_t)(cand_seq % RUDP_SLIDING_FEC_MAX_W);
    uint8_t c = cauchy_coeff(seq, cand_seq);
    acc_raw ^= gf256_mul_u32(fec->payloads[idx].raw, c);
  }
  fec->since_last_repair = 0U;
  *out_repair = (rudp_sliding_fec_repair_s){
      .base_seq = base,
      .span = span,
      .valid = 1U,
      .xor_payload = {.raw = acc_raw},
  };
  return 1;
}

int rudp_sliding_fec_rx_init(rudp_sliding_fec_rx_s *rx, uint8_t window_w) {
  if (!rx || window_w < 2U || window_w > RUDP_SLIDING_FEC_MAX_W) {
    return RUDP_ERR_INVALID_ARG;
  }
  memset(rx, 0, sizeof(*rx));
  rx->window_w = window_w;
  return RUDP_OK;
}

int rudp_sliding_fec_rx_source(rudp_sliding_fec_rx_s *rx, uint16_t seq,
                               tfv_packet_u payload) {
  if (!rx || rx->window_w < 2U) {
    return RUDP_ERR_INVALID_ARG;
  }
  size_t mod = (size_t)(RUDP_SLIDING_FEC_MAX_W * 2U);
  size_t idx = (size_t)(seq % mod);
  rx->seqs[idx] = seq;
  rx->payloads[idx] = payload;
  rx->present[idx] = 1U;
  return RUDP_OK;
}

static void fec_rx_store_recovered(rudp_sliding_fec_rx_s *rx, uint16_t seq,
                                   uint32_t raw, int *recovered_total,
                                   uint16_t *out_recovered_seq,
                                   tfv_packet_u *out_recovered_payload) {
  size_t mod = (size_t)(RUDP_SLIDING_FEC_MAX_W * 2U);
  size_t idx = (size_t)(seq % mod);
  if (rx->present[idx] && rx->seqs[idx] == seq) {
    return;
  }
  tfv_packet_u pkt = {.raw = raw};
  rx->seqs[idx] = seq;
  rx->payloads[idx] = pkt;
  rx->present[idx] = 1U;
  if (*recovered_total == 0) {
    if (out_recovered_seq) {
      *out_recovered_seq = seq;
    }
    if (out_recovered_payload) {
      *out_recovered_payload = pkt;
    }
  }
  (*recovered_total)++;
}

static bool fec_rx_solve_pending(rudp_sliding_fec_rx_s *rx, int *recovered_total,
                                 uint16_t *out_recovered_seq,
                                 tfv_packet_u *out_recovered_payload) {
  size_t mod = (size_t)(RUDP_SLIDING_FEC_MAX_W * 2U);
  bool any_new = false;
  bool progress = true;

  while (progress) {
    progress = false;
    uint16_t col_seqs[16];
    uint8_t num_cols = 0U;
    uint8_t mat[RUDP_SLIDING_FEC_MAX_W][16];
    uint32_t rhs_vec[RUDP_SLIDING_FEC_MAX_W];
    uint8_t num_rows = 0U;
    memset(mat, 0, sizeof(mat));

    for (uint8_t e = 0U; e < RUDP_SLIDING_FEC_MAX_W; ++e) {
      rudp_sliding_fec_eq_s *eq = &rx->pending_eqs[e];
      if (!eq->valid) {
        continue;
      }
      uint16_t repair_seq = (uint16_t)(eq->base_seq + eq->span - 1U);
      uint32_t rhs = eq->rhs_raw;
      uint8_t missing = 0U;
      uint16_t miss_seq = 0U;
      uint8_t miss_coeff = 0U;

      for (uint8_t k = 0U; k < eq->span; ++k) {
        uint16_t seq = (uint16_t)(eq->base_seq + k);
        size_t idx = (size_t)(seq % mod);
        uint8_t c = cauchy_coeff(repair_seq, seq);
        if (rx->present[idx] && rx->seqs[idx] == seq) {
          rhs ^= gf256_mul_u32(rx->payloads[idx].raw, c);
        } else {
          missing++;
          miss_seq = seq;
          miss_coeff = c;
        }
      }
      if (missing == 0U) {
        eq->valid = 0U;
        continue;
      }
      if (missing == 1U) {
        uint32_t rec_raw = gf256_mul_u32(rhs, gf256_inv(miss_coeff));
        fec_rx_store_recovered(rx, miss_seq, rec_raw, recovered_total,
                               out_recovered_seq, out_recovered_payload);
        eq->valid = 0U;
        any_new = true;
        progress = true;
        continue;
      }
      /* Add row to local GF(2^8) elimination matrix */
      if (num_rows < RUDP_SLIDING_FEC_MAX_W) {
        bool overflow_cols = false;
        for (uint8_t k = 0U; k < eq->span; ++k) {
          uint16_t seq = (uint16_t)(eq->base_seq + k);
          size_t idx = (size_t)(seq % mod);
          if (rx->present[idx] && rx->seqs[idx] == seq) {
            continue;
          }
          uint8_t col_idx = num_cols;
          for (uint8_t c = 0U; c < num_cols; ++c) {
            if (col_seqs[c] == seq) {
              col_idx = c;
              break;
            }
          }
          if (col_idx == num_cols) {
            if (num_cols >= 16U) {
              overflow_cols = true;
              break;
            }
            col_seqs[num_cols++] = seq;
          }
          mat[num_rows][col_idx] = cauchy_coeff(repair_seq, seq);
        }
        if (!overflow_cols) {
          rhs_vec[num_rows++] = rhs;
        } else {
          memset(mat[num_rows], 0, sizeof(mat[num_rows]));
        }
      }
    }

    if (progress || num_rows < 2U || num_cols == 0U) {
      continue;
    }

    /* Bounded RREF Gaussian elimination over GF(2^8) */
    uint8_t piv_row = 0U;
    for (uint8_t c = 0U; c < num_cols && piv_row < num_rows; ++c) {
      uint8_t sel = num_rows;
      for (uint8_t r = piv_row; r < num_rows; ++r) {
        if (mat[r][c] != 0U) {
          sel = r;
          break;
        }
      }
      if (sel == num_rows) {
        continue;
      }
      if (sel != piv_row) {
        for (uint8_t j = 0U; j < num_cols; ++j) {
          uint8_t tmp = mat[piv_row][j];
          mat[piv_row][j] = mat[sel][j];
          mat[sel][j] = tmp;
        }
        uint32_t tr = rhs_vec[piv_row];
        rhs_vec[piv_row] = rhs_vec[sel];
        rhs_vec[sel] = tr;
      }
      uint8_t inv_piv = gf256_inv(mat[piv_row][c]);
      for (uint8_t j = 0U; j < num_cols; ++j) {
        mat[piv_row][j] = gf256_mul(mat[piv_row][j], inv_piv);
      }
      rhs_vec[piv_row] = gf256_mul_u32(rhs_vec[piv_row], inv_piv);
      for (uint8_t r = 0U; r < num_rows; ++r) {
        if (r != piv_row && mat[r][c] != 0U) {
          uint8_t factor = mat[r][c];
          for (uint8_t j = 0U; j < num_cols; ++j) {
            mat[r][j] ^= gf256_mul(factor, mat[piv_row][j]);
          }
          rhs_vec[r] ^= gf256_mul_u32(rhs_vec[piv_row], factor);
        }
      }
      piv_row++;
    }

    for (uint8_t r = 0U; r < num_rows; ++r) {
      uint8_t nz = 0U;
      uint8_t nz_col = 0U;
      for (uint8_t c = 0U; c < num_cols; ++c) {
        if (mat[r][c] != 0U) {
          nz++;
          nz_col = c;
        }
      }
      if (nz == 1U) {
        uint8_t inv_c = gf256_inv(mat[r][nz_col]);
        uint32_t rec_raw = gf256_mul_u32(rhs_vec[r], inv_c);
        fec_rx_store_recovered(rx, col_seqs[nz_col], rec_raw, recovered_total,
                               out_recovered_seq, out_recovered_payload);
        any_new = true;
        progress = true;
      }
    }
  }
  return any_new;
}

int rudp_sliding_fec_rx_repair(rudp_sliding_fec_rx_s *rx,
                               const rudp_sliding_fec_repair_s *repair,
                               uint16_t *out_recovered_seq,
                               tfv_packet_u *out_recovered_payload) {
  if (!rx || !repair || !repair->valid || repair->span < 2U ||
      repair->span > rx->window_w) {
    return RUDP_ERR_INVALID_ARG;
  }
  size_t mod = (size_t)(RUDP_SLIDING_FEC_MAX_W * 2U);
  uint16_t repair_seq = (uint16_t)(repair->base_seq + repair->span - 1U);
  uint8_t missing = 0U;
  uint16_t miss_seq = 0U;
  uint8_t miss_coeff = 0U;
  uint32_t rhs = repair->xor_payload.raw;

  for (uint8_t k = 0U; k < repair->span; ++k) {
    uint16_t seq = (uint16_t)(repair->base_seq + k);
    size_t idx = (size_t)(seq % mod);
    uint8_t c = cauchy_coeff(repair_seq, seq);
    if (rx->present[idx] && rx->seqs[idx] == seq) {
      rhs ^= gf256_mul_u32(rx->payloads[idx].raw, c);
    } else {
      missing++;
      miss_seq = seq;
      miss_coeff = c;
    }
  }
  if (missing == 0U) {
    return 0;
  }

  int recovered_total = 0;
  if (missing == 1U) {
    uint32_t rec_raw = gf256_mul_u32(rhs, gf256_inv(miss_coeff));
    fec_rx_store_recovered(rx, miss_seq, rec_raw, &recovered_total,
                           out_recovered_seq, out_recovered_payload);
    (void)fec_rx_solve_pending(rx, &recovered_total, out_recovered_seq,
                               out_recovered_payload);
    return recovered_total;
  }

  /* Buffer multi-unknown equation and attempt joint GF(2^8) solve */
  rx->pending_eqs[rx->eq_cursor] = (rudp_sliding_fec_eq_s){
      .base_seq = repair->base_seq,
      .span = repair->span,
      .valid = 1U,
      .rhs_raw = repair->xor_payload.raw,
  };
  rx->eq_cursor = (uint8_t)((rx->eq_cursor + 1U) % RUDP_SLIDING_FEC_MAX_W);
  (void)fec_rx_solve_pending(rx, &recovered_total, out_recovered_seq,
                             out_recovered_payload);
  return recovered_total;
}

bool rudp_sliding_fec_rx_get(const rudp_sliding_fec_rx_s *rx, uint16_t seq,
                             tfv_packet_u *out_payload) {
  if (!rx || rx->window_w < 2U) {
    return false;
  }
  size_t mod = (size_t)(RUDP_SLIDING_FEC_MAX_W * 2U);
  size_t idx = (size_t)(seq % mod);
  if (!rx->present[idx] || rx->seqs[idx] != seq) {
    return false;
  }
  if (out_payload) {
    *out_payload = rx->payloads[idx];
  }
  return true;
}

/* =========================================================================
 * 5. Multipath Transport Scheduler
 * ========================================================================= */

int rudp_multipath_init(rudp_multipath_s *mp, uint16_t session_seed, uint8_t mode) {
  if (!mp || mode > RUDP_MP_ECF_SPRAY) {
    return RUDP_ERR_INVALID_ARG;
  }
  memset(mp, 0, sizeof(*mp));
  mp->session_seed = session_seed;
  mp->mode = mode;
  return RUDP_OK;
}

int rudp_multipath_add_path(rudp_multipath_s *mp, uint8_t path_id, uint32_t weight,
                            uint16_t flow_hint) {
  if (!mp || path_id >= RUDP_MAX_PATHS || weight == 0U) {
    return RUDP_ERR_INVALID_ARG;
  }
  mp->paths[path_id] = (rudp_path_state_s){
      .srtt_ms = 20U,
      .loss_permille = 0U,
      .weight = weight,
      .current_weight = 0,
      .tx_bytes = 0U,
      .last_feedback_ms = 0U,
      .flow_hint = flow_hint,
      .active = 1U,
      .degraded = 0U,
  };
  if (!mp->paths[mp->primary_path].active) {
    mp->primary_path = path_id;
  }
  return RUDP_OK;
}

static uint8_t multipath_best_healthy(const rudp_multipath_s *mp,
                                      bool allow_degraded) {
  uint8_t best = RUDP_MAX_PATHS;
  for (uint8_t i = 0U; i < RUDP_MAX_PATHS; ++i) {
    const rudp_path_state_s *p = &mp->paths[i];
    if (!p->active || (!allow_degraded && p->degraded)) {
      continue;
    }
    if (best == RUDP_MAX_PATHS || p->srtt_ms < mp->paths[best].srtt_ms) {
      best = i;
    }
  }
  return best;
}

int rudp_multipath_on_feedback(rudp_multipath_s *mp, uint8_t path_id, uint32_t rtt_ms,
                               bool lost, uint32_t now_ms) {
  if (!mp || path_id >= RUDP_MAX_PATHS || !mp->paths[path_id].active) {
    return RUDP_ERR_INVALID_ARG;
  }
  rudp_path_state_s *p = &mp->paths[path_id];
  p->srtt_ms = (p->srtt_ms * 7U + rtt_ms) / 8U;
  uint32_t ls = lost ? 1000U : 0U;
  p->loss_permille = (p->loss_permille * 7U + ls) / 8U;
  p->last_feedback_ms = now_ms;
  p->tx_since_feedback = 0U;
  p->feedback_seen = 1U;
  p->degraded = (p->loss_permille >= 200U) ? 1U : 0U;

  if (mp->mode == RUDP_MP_PRIMARY_FAILOVER &&
      (!mp->paths[mp->primary_path].active || mp->paths[mp->primary_path].degraded)) {
    uint8_t better = multipath_best_healthy(mp, false);
    if (better < RUDP_MAX_PATHS) {
      mp->primary_path = better;
    }
  }
  return RUDP_OK;
}

int rudp_multipath_select_paths(rudp_multipath_s *mp, uint8_t channel_id,
                                uint32_t packet_bytes,
                                uint8_t out_path_ids[RUDP_MAX_PATHS],
                                uint32_t out_entropy[RUDP_MAX_PATHS]) {
  if (!mp || !out_path_ids || !out_entropy || packet_bytes == 0U) {
    return RUDP_ERR_INVALID_ARG;
  }

  if (mp->mode == RUDP_MP_REDUNDANT) {
    int count = 0;
    for (uint8_t i = 0U; i < RUDP_MAX_PATHS; ++i) {
      if (mp->paths[i].active && !mp->paths[i].degraded) {
        out_path_ids[count] = i;
        out_entropy[count] =
            rudp_calc_entropy(mp->session_seed, channel_id, mp->paths[i].flow_hint);
        mp->paths[i].tx_bytes += packet_bytes;
        mp->paths[i].tx_since_feedback++;
        count++;
      }
    }
    if (count == 0) {
      uint8_t fb = multipath_best_healthy(mp, true);
      if (fb < RUDP_MAX_PATHS) {
        out_path_ids[0] = fb;
        out_entropy[0] =
            rudp_calc_entropy(mp->session_seed, channel_id, mp->paths[fb].flow_hint);
        mp->paths[fb].tx_bytes += packet_bytes;
        mp->paths[fb].tx_since_feedback++;
        count = 1;
      }
    }
    return count;
  }

  if (mp->mode == RUDP_MP_WEIGHTED_SPRAY || mp->mode == RUDP_MP_ECF_SPRAY) {
    int32_t total_w = 0;
    uint8_t best = RUDP_MAX_PATHS;
    for (uint8_t i = 0U; i < RUDP_MAX_PATHS; ++i) {
      rudp_path_state_s *p = &mp->paths[i];
      if (!p->active || p->degraded) {
        continue;
      }
      int32_t w = (int32_t)p->weight;
      p->current_weight += w;
      total_w += w;
      if (best == RUDP_MAX_PATHS || p->current_weight > mp->paths[best].current_weight) {
        best = i;
      }
    }
    if (best < RUDP_MAX_PATHS) {
      if (mp->mode == RUDP_MP_ECF_SPRAY) {
        uint8_t fast = multipath_best_healthy(mp, false);
        if (fast < RUDP_MAX_PATHS && fast != best &&
            (mp->paths[best].srtt_ms / 2U) >
                ((mp->paths[fast].srtt_ms / 2U) + (mp->paths[fast].srtt_ms / 4U))) {
          /* Undo credit deduction on gated slow path; route via fastest path */
          mp->paths[best].current_weight -= total_w;
          best = fast;
        } else {
          mp->paths[best].current_weight -= total_w;
        }
      } else {
        mp->paths[best].current_weight -= total_w;
      }
      out_path_ids[0] = best;
      out_entropy[0] =
          rudp_calc_entropy(mp->session_seed, channel_id, mp->paths[best].flow_hint);
      mp->paths[best].tx_bytes += packet_bytes;
      mp->paths[best].tx_since_feedback++;
      return 1;
    }
  }

  /* PRIMARY_FAILOVER (or fallback if all healthy paths exhausted) */
  uint8_t chosen = mp->primary_path;
  if (chosen >= RUDP_MAX_PATHS || !mp->paths[chosen].active ||
      mp->paths[chosen].degraded) {
    chosen = multipath_best_healthy(mp, false);
    if (chosen >= RUDP_MAX_PATHS) {
      chosen = multipath_best_healthy(mp, true);
    }
  }
  if (chosen >= RUDP_MAX_PATHS) {
    return 0;
  }
  out_path_ids[0] = chosen;
  out_entropy[0] =
      rudp_calc_entropy(mp->session_seed, channel_id, mp->paths[chosen].flow_hint);
  mp->paths[chosen].tx_bytes += packet_bytes;
  mp->paths[chosen].tx_since_feedback++;
  return 1;
}

int rudp_multipath_check_blackhole(rudp_multipath_s *mp, uint32_t now_ms,
                                   uint32_t min_tx_packets,
                                   uint32_t no_reply_ms) {
  if (!mp || min_tx_packets == 0U || no_reply_ms == 0U) {
    return RUDP_ERR_INVALID_ARG;
  }
  int shifted = 0;
  for (uint8_t i = 0U; i < RUDP_MAX_PATHS; ++i) {
    rudp_path_state_s *p = &mp->paths[i];
    if (!p->active || p->degraded || p->tx_since_feedback == 0U) {
      continue;
    }
    if (!p->feedback_seen) {
      p->last_feedback_ms = now_ms;
      p->feedback_seen = 1U;
      continue;
    }
    if (p->tx_since_feedback >= min_tx_packets &&
        (uint32_t)(now_ms - p->last_feedback_ms) >= no_reply_ms) {
      p->flow_hint++;
      p->degraded = 1U;
      p->tx_since_feedback = 0U;
      p->last_feedback_ms = now_ms;
      shifted++;
    }
  }
  if (shifted > 0 && mp->mode == RUDP_MP_PRIMARY_FAILOVER &&
      (!mp->paths[mp->primary_path].active || mp->paths[mp->primary_path].degraded)) {
    uint8_t better = multipath_best_healthy(mp, false);
    if (better < RUDP_MAX_PATHS) {
      mp->primary_path = better;
    }
  }
  return shifted;
}

/* =========================================================================
 * 6. Generic-Buffer GF(2^8) Cauchy Coding & Unified NextGen Transport Driver
 * ========================================================================= */

int rudp_gf256_xor_scaled(uint8_t *dst, const uint8_t *src, size_t len,
                          uint8_t coeff) {
  if (!dst || !src) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (coeff == 0U || len == 0U) {
    return RUDP_OK;
  }
  if (coeff == 1U) {
    for (size_t i = 0U; i < len; ++i) {
      dst[i] ^= src[i];
    }
    return RUDP_OK;
  }

  /* Build 4-bit split-nibble tables (0 bytes .rodata, ~30 calls) */
  uint8_t lo[16];
  uint8_t hi[16];
  lo[0] = 0U;
  hi[0] = 0U;
  for (uint8_t n = 1U; n < 16U; ++n) {
    lo[n] = gf256_mul(n, coeff);
    hi[n] = gf256_mul((uint8_t)(n << 4U), coeff);
  }

  if (len < 64U) {
    for (size_t i = 0U; i < len; ++i) {
      uint8_t x = src[i];
      dst[i] ^= (uint8_t)(lo[x & 0x0FU] ^ hi[x >> 4U]);
    }
    return RUDP_OK;
  }

  /* Expand into 256-byte L1 stack LUT for 1-lookup/byte unrolled XOR */
  uint8_t tbl[256];
  for (unsigned h = 0U; h < 16U; ++h) {
    uint8_t hv = hi[h];
    uint8_t *row = &tbl[h << 4U];
    for (unsigned l = 0U; l < 16U; ++l) {
      row[l] = (uint8_t)(hv ^ lo[l]);
    }
  }

  size_t i = 0U;
  for (; i + 4U <= len; i += 4U) {
    dst[i + 0U] ^= tbl[src[i + 0U]];
    dst[i + 1U] ^= tbl[src[i + 1U]];
    dst[i + 2U] ^= tbl[src[i + 2U]];
    dst[i + 3U] ^= tbl[src[i + 3U]];
  }
  for (; i < len; ++i) {
    dst[i] ^= tbl[src[i]];
  }
  return RUDP_OK;
}

static void nextgen_write_u64_le(uint8_t out[8], uint64_t v) {
  for (unsigned i = 0U; i < 8U; ++i) {
    out[i] = (uint8_t)((v >> (i * 8U)) & 0xFFU);
  }
}

static uint64_t nextgen_read_u64_le(const uint8_t in[8]) {
  uint64_t v = 0ULL;
  for (unsigned i = 0U; i < 8U; ++i) {
    v |= ((uint64_t)in[i]) << (i * 8U);
  }
  return v;
}

int rudp_nextgen_transport_init(rudp_nextgen_transport_s *t,
                                rudp_session_s *session,
                                const rudp_nextgen_crypto_s *crypto,
                                uint64_t connection_id) {
  if (!t || !session) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (crypto && (!crypto->seal || !crypto->open)) {
    return RUDP_ERR_INVALID_ARG;
  }
  memset(t, 0, sizeof(*t));
  t->session = session;
  t->connection_id = connection_id;
  t->use_cid = (connection_id != 0ULL) ? 1U : 0U;
  if (crypto) {
    t->crypto = *crypto;
    t->use_crypto = 1U;
  }
  return RUDP_OK;
}

int rudp_nextgen_transport_enable_pacer(rudp_nextgen_transport_s *t,
                                        uint32_t initial_bps, uint32_t min_bps,
                                        uint32_t max_bps,
                                        uint32_t target_queue_us,
                                        uint64_t now_us) {
  if (!t) {
    return RUDP_ERR_INVALID_ARG;
  }
  int ret = rudp_pacer_init(&t->pacer, initial_bps, min_bps, max_bps,
                            target_queue_us, now_us);
  if (ret == RUDP_OK) {
    t->use_pacer = 1U;
  }
  return ret;
}

int rudp_nextgen_transport_enable_fec(rudp_nextgen_transport_s *t,
                                      uint8_t window_w) {
  if (!t) {
    return RUDP_ERR_INVALID_ARG;
  }
  int ret = rudp_sliding_fec_init(&t->fec_tx, window_w);
  if (ret != RUDP_OK) {
    return ret;
  }
  ret = rudp_sliding_fec_rx_init(&t->fec_rx, window_w);
  if (ret == RUDP_OK) {
    t->use_fec = 1U;
  }
  return ret;
}

int rudp_nextgen_transport_enable_multipath(rudp_nextgen_transport_s *t,
                                            uint16_t session_seed,
                                            uint8_t mode) {
  if (!t) {
    return RUDP_ERR_INVALID_ARG;
  }
  int ret = rudp_multipath_init(&t->multipath, session_seed, mode);
  if (ret == RUDP_OK) {
    t->use_multipath = 1U;
  }
  return ret;
}

int rudp_nextgen_transport_send_datagram(
    rudp_nextgen_transport_s *t, uint8_t primary_ack_channel, uint8_t *out_buf,
    size_t max_len, uint64_t now_us, uint32_t timeout_ms, uint8_t *out_path_id,
    uint32_t *out_entropy) {
  if (!t || !t->session || !out_buf) {
    return RUDP_ERR_INVALID_ARG;
  }
  size_t cid_len = t->use_cid ? RUDP_NEXTGEN_CID_WIRE_BYTES : 0U;
  if (max_len < cid_len + RUDP_WIRE_DATAGRAM_HEADER_SIZE) {
    return RUDP_ERR_BUFFER_FULL;
  }
  if (t->use_pacer &&
      !rudp_pacer_refill_and_can_send(&t->pacer, now_us, RUDP_NEXTGEN_MTU_BYTES)) {
    return 0;
  }

  uint8_t raw_buf[RUDP_NEXTGEN_MTU_BYTES + 64U];
  uint32_t now_ms = (uint32_t)(now_us / 1000ULL);
  int pt_len = rudp_session_build_datagram(
      t->session, primary_ack_channel, raw_buf, sizeof(raw_buf), now_ms, timeout_ms);
  if (pt_len <= 0) {
    return pt_len;
  }

  if (t->use_cid) {
    nextgen_write_u64_le(out_buf, t->connection_id);
  }

  size_t total_len = 0U;
  if (t->use_crypto) {
    const uint8_t *aad = t->use_cid ? out_buf : NULL;
    int sealed = t->crypto.seal(t->crypto.ctx, aad, cid_len, raw_buf,
                                (size_t)pt_len, out_buf + cid_len,
                                max_len - cid_len);
    if (sealed < 0) {
      return sealed;
    }
    total_len = cid_len + (size_t)sealed;
  } else {
    if (max_len - cid_len < (size_t)pt_len) {
      return RUDP_ERR_BUFFER_FULL;
    }
    memcpy(out_buf + cid_len, raw_buf, (size_t)pt_len);
    total_len = cid_len + (size_t)pt_len;
  }

  if (t->use_pacer) {
    (void)rudp_pacer_consume(&t->pacer, (uint32_t)total_len);
  }
  if (t->use_multipath) {
    uint8_t pids[RUDP_MAX_PATHS];
    uint32_t ents[RUDP_MAX_PATHS];
    int sel = rudp_multipath_select_paths(&t->multipath, primary_ack_channel,
                                          (uint32_t)total_len, pids, ents);
    if (sel > 0) {
      if (out_path_id) {
        *out_path_id = pids[0];
      }
      if (out_entropy) {
        *out_entropy = ents[0];
      }
    }
  }
  return (int)total_len;
}

int rudp_nextgen_transport_recv_datagram(
    rudp_nextgen_transport_s *t, const uint8_t *in_buf, size_t in_len,
    rudp_record_s *out_delivered, size_t max_delivered, uint64_t now_us,
    rudp_stats_s *stats, uint8_t rx_path_id) {
  if (!t || !t->session || !in_buf) {
    return RUDP_ERR_INVALID_ARG;
  }
  size_t cid_len = t->use_cid ? RUDP_NEXTGEN_CID_WIRE_BYTES : 0U;
  if (in_len < cid_len + RUDP_WIRE_DATAGRAM_HEADER_SIZE) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (t->use_cid) {
    uint64_t pkt_cid = nextgen_read_u64_le(in_buf);
    if (pkt_cid != t->connection_id) {
      return RUDP_ERR_AUTH;
    }
  }

  uint8_t pt_buf[RUDP_NEXTGEN_MTU_BYTES + 128U];
  const uint8_t *pt = in_buf + cid_len;
  size_t pt_len = in_len - cid_len;

  if (t->use_crypto) {
    const uint8_t *aad = t->use_cid ? in_buf : NULL;
    int opened = t->crypto.open(t->crypto.ctx, aad, cid_len, in_buf + cid_len,
                                in_len - cid_len, pt_buf, sizeof(pt_buf));
    if (opened < 0) {
      return opened;
    }
    pt = pt_buf;
    pt_len = (size_t)opened;
  }

  uint32_t now_ms = (uint32_t)(now_us / 1000ULL);
  int ret = rudp_session_process_datagram_ex(t->session, pt, pt_len, out_delivered,
                                             max_delivered, true, now_ms, stats);
  if (ret >= 0) {
    uint32_t srtt_scaled = t->session->channels[0].srtt_scaled;
    if (srtt_scaled > 0U) {
      uint32_t srtt_ms = (srtt_scaled + 7U) >> 3;
      uint32_t owd_us = (srtt_ms > 0U ? srtt_ms : 1U) * 500U;
      if (t->use_pacer) {
        (void)rudp_pacer_on_owd_sample(&t->pacer, owd_us, (uint32_t)in_len, 0U);
      }
      if (t->use_multipath && rx_path_id < RUDP_MAX_PATHS) {
        (void)rudp_multipath_on_feedback(&t->multipath, rx_path_id, srtt_ms,
                                         false, now_ms);
      }
      if (t->use_fec) {
        (void)rudp_sliding_fec_update_loss(&t->fec_tx, false);
      }
    }
  }
  return ret;
}

int rudp_nextgen_transport_tick(rudp_nextgen_transport_s *t, uint32_t now_ms,
                                uint32_t min_tx_packets,
                                uint32_t no_reply_ms) {
  if (!t || !t->session) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (t->use_multipath && min_tx_packets > 0U && no_reply_ms > 0U) {
    return rudp_multipath_check_blackhole(&t->multipath, now_ms, min_tx_packets,
                                          no_reply_ms);
  }
  return 0;
}
