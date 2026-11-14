#include "rudp_nextgen.h"
#include "rudp_codec.h"
#include <string.h>

#define RUDP_NEXTGEN_MTU_BYTES                                                         \
  (RUDP_DEFAULT_MTU + RUDP_WIRE_DATAGRAM_HEADER_SIZE)

static inline tfv_packet_u nextgen_payload_xor(tfv_packet_u a, tfv_packet_u b) {
  tfv_packet_u r;
  r.type = (uint8_t)(a.type ^ b.type);
  r.flags = (uint8_t)(a.flags ^ b.flags);
  r.value = (uint16_t)(a.value ^ b.value);
  return r;
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
    pacer->smooth_owd_us = owd_us;
    pacer->owd_initialized = 1U;
  } else {
    if (owd_us < pacer->base_owd_us) {
      pacer->base_owd_us = owd_us;
    }
    pacer->smooth_owd_us =
        (uint32_t)(((uint64_t)pacer->smooth_owd_us * 7ULL + owd_us) / 8ULL);
  }

  uint32_t qdelay = (pacer->smooth_owd_us > pacer->base_owd_us)
                        ? (pacer->smooth_owd_us - pacer->base_owd_us)
                        : 0U;
  if (ce_marked || qdelay > pacer->target_queue_us) {
    uint32_t drop = pacer->rate_bytes_per_sec / 8U;
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
    uint32_t c_drop = pacer->cwnd_bytes / 8U;
    pacer->cwnd_bytes =
        (pacer->cwnd_bytes > c_drop) ? (pacer->cwnd_bytes - c_drop) : cwnd_floor;
    if (pacer->cwnd_bytes < cwnd_floor) {
      pacer->cwnd_bytes = cwnd_floor;
    }
  } else if (qdelay <= (pacer->target_queue_us / 2U)) {
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

  /* Collect contiguous run ending at seq up to window_w */
  tfv_packet_u acc = {.raw = 0U};
  uint8_t span = 0U;
  uint16_t base = seq;
  for (uint8_t back = 0U; back < fec->window_w; ++back) {
    uint16_t cand_seq = (uint16_t)(seq - back);
    size_t idx = (size_t)(cand_seq % RUDP_SLIDING_FEC_MAX_W);
    if (!fec->present[idx] || fec->seqs[idx] != cand_seq) {
      break;
    }
    acc = nextgen_payload_xor(acc, fec->payloads[idx]);
    base = cand_seq;
    span++;
  }
  if (span < 2U) {
    *out_repair = (rudp_sliding_fec_repair_s){0};
    return 0;
  }
  fec->since_last_repair = 0U;
  *out_repair = (rudp_sliding_fec_repair_s){
      .base_seq = base,
      .span = span,
      .valid = 1U,
      .xor_payload = acc,
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

int rudp_sliding_fec_rx_repair(rudp_sliding_fec_rx_s *rx,
                               const rudp_sliding_fec_repair_s *repair,
                               uint16_t *out_recovered_seq,
                               tfv_packet_u *out_recovered_payload) {
  if (!rx || !repair || !repair->valid || repair->span < 2U ||
      repair->span > rx->window_w) {
    return RUDP_ERR_INVALID_ARG;
  }
  size_t mod = (size_t)(RUDP_SLIDING_FEC_MAX_W * 2U);
  uint8_t missing = 0U;
  uint16_t miss_seq = 0U;
  tfv_packet_u acc = repair->xor_payload;

  for (uint8_t k = 0U; k < repair->span; ++k) {
    uint16_t seq = (uint16_t)(repair->base_seq + k);
    size_t idx = (size_t)(seq % mod);
    if (rx->present[idx] && rx->seqs[idx] == seq) {
      acc = nextgen_payload_xor(acc, rx->payloads[idx]);
    } else {
      missing++;
      miss_seq = seq;
      if (missing > 1U) {
        return 0;
      }
    }
  }
  if (missing == 0U) {
    return 0;
  }
  size_t miss_idx = (size_t)(miss_seq % mod);
  rx->seqs[miss_idx] = miss_seq;
  rx->payloads[miss_idx] = acc;
  rx->present[miss_idx] = 1U;
  if (out_recovered_seq) {
    *out_recovered_seq = miss_seq;
  }
  if (out_recovered_payload) {
    *out_recovered_payload = acc;
  }
  return 1;
}

/* =========================================================================
 * 5. Multipath Transport Scheduler
 * ========================================================================= */

int rudp_multipath_init(rudp_multipath_s *mp, uint16_t session_seed, uint8_t mode) {
  if (!mp || mode > RUDP_MP_REDUNDANT) {
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
        count = 1;
      }
    }
    return count;
  }

  if (mp->mode == RUDP_MP_WEIGHTED_SPRAY) {
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
      mp->paths[best].current_weight -= total_w;
      out_path_ids[0] = best;
      out_entropy[0] =
          rudp_calc_entropy(mp->session_seed, channel_id, mp->paths[best].flow_hint);
      mp->paths[best].tx_bytes += packet_bytes;
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
  return 1;
}
