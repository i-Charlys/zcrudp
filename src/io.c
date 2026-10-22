#include "rudp_io.h"
#include <string.h>

int rudp_iovec_add(rudp_iovec_collection_s *col, const void *base, uint32_t len) {
  if (!col || !base) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (col->count >= RUDP_IOVEC_MAX) {
    return RUDP_ERR_BUFFER_FULL;
  }
  if (len > 65535U - (uint32_t)col->total_len) {
    return RUDP_ERR_BUFFER_FULL;
  }
  col->iovecs[col->count] = (rudp_iovec_s){
      .base = base,
      .len = len,
  };
  col->count++;
  col->total_len = (uint16_t)(col->total_len + len);
  return RUDP_OK;
}

int rudp_iovec_flatten(const rudp_iovec_collection_s *col, uint8_t *out_buf,
                       size_t max_len) {
  if (!col || !out_buf) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (col->count > RUDP_IOVEC_MAX) {
    return RUDP_ERR_INVALID_ARG;
  }
  /* Validate every segment before copying: total_len is a public field and
   * cannot be trusted as a bound. */
  size_t total = 0;
  for (uint8_t i = 0; i < col->count; i++) {
    if (col->iovecs[i].len > 0 && !col->iovecs[i].base) {
      return RUDP_ERR_INVALID_ARG;
    }
    if (col->iovecs[i].len > 65535U - total) {
      return RUDP_ERR_INVALID_ARG;
    }
    total += col->iovecs[i].len;
  }
  if (max_len < total) {
    return RUDP_ERR_BUFFER_FULL;
  }
  size_t offset = 0;
  for (uint8_t i = 0; i < col->count; i++) {
    if (col->iovecs[i].len > 0) {
      memcpy(out_buf + offset, col->iovecs[i].base, col->iovecs[i].len);
      offset += col->iovecs[i].len;
    }
  }
  return (int)offset;
}
