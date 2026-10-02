#ifndef RUDP_IO_H
#define RUDP_IO_H

/**
 * @file rudp_io.h
 * @brief Single, 100% portable and OS-agnostic vectored I/O API (Zero-Copy).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RUDP_IOVEC_MAX
#define RUDP_IOVEC_MAX 8
#endif

/**
 * @brief OS-agnostic scatter-gather memory slice.
 */
typedef struct {
  const void *base; /**< Pointer to contiguous memory buffer */
  uint32_t len;     /**< Length in bytes of the buffer */
} rudp_iovec_s;

/**
 * @brief Ordered collection of scatter-gather slices forming a datagram.
 */
typedef struct {
  rudp_iovec_s iovecs[RUDP_IOVEC_MAX]; /**< Memory slices */
  uint32_t path_entropy;               /**< Path entropy identifier (implementation defined) */
  uint16_t total_len;                  /**< Total aggregated datagram payload length */
  uint8_t count;                       /**< Number of active slices (<= RUDP_IOVEC_MAX) */
  uint8_t dscp;                        /**< DSCP value (0-63) */
} rudp_iovec_collection_s;

/**
 * @brief Computes a constant-time O(1) flow entropy hash for multipath diversity.
 *
 * Combines session identity, channel ID, and path flow hints to produce
 * a deterministic hash. This allows the host socket layer to dynamically
 * rotate UDP source ports or select network interfaces (Wi-Fi/cellular)
 * without inducing intra-flow packet reordering.
 *
 * @param session_seed Initial pseudo-random session identifier.
 * @param channel_id Channel identifier (0 to RUDP_MAX_CHANNELS - 1).
 * @param flow_hint Explicit path generation or failover counter (e.g., 0).
 * @return 32-bit flow entropy hash.
 */
static inline uint32_t rudp_calc_entropy(uint16_t session_seed,
                                           uint8_t channel_id,
                                           uint16_t flow_hint) {
  uint32_t h = (uint32_t)session_seed ^ ((uint32_t)channel_id << 8) ^
               (uint32_t)flow_hint;

  // lowbias32 32-bit Hash
  h ^= h >> 16;
  h *= 0x7feb352dU;
  h ^= h >> 15;
  h *= 0x846ca68bU;
  h ^= h >> 16;
  return h;
}

/**
 * @brief Flattens a scatter-gather collection into a contiguous destination buffer.
 *
 * @param col Pointer to the source collection.
 * @param out_buf Destination byte buffer.
 * @param max_len Maximum writable capacity of out_buf.
 * @return Total bytes written on success, RUDP_ERR_BUFFER_FULL if max_len is too small,
 *         or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_iovec_flatten(const rudp_iovec_collection_s *col, uint8_t *out_buf,
                       size_t max_len);

/**
 * @brief Appends a memory slice to a scatter-gather collection.
 *
 * @param col Pointer to the destination collection.
 * @param base Pointer to contiguous memory buffer.
 * @param len Length in bytes of the buffer slice.
 * @return RUDP_OK on success, RUDP_ERR_BUFFER_FULL if the collection is full,
 *         or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_iovec_add(rudp_iovec_collection_s *col, const void *base,
                   uint32_t len);


#ifdef __cplusplus
}
#endif

#endif /* RUDP_IO_H */