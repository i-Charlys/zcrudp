#include "rudp_crypto.h"
#include <limits.h>
#include <string.h>

static void secure_zero(void *ptr, size_t len) {
  volatile uint8_t *p = (volatile uint8_t *)ptr;
  while (len--) {
    *p++ = 0U;
  }
}

static uint32_t load32_le(const uint8_t b[4]) {
  return ((uint32_t)b[0]) | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
}

static void store32_le(uint8_t b[4], uint32_t v) {
  b[0] = (uint8_t)(v & 0xFFU);
  b[1] = (uint8_t)((v >> 8) & 0xFFU);
  b[2] = (uint8_t)((v >> 16) & 0xFFU);
  b[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static void store64_le(uint8_t b[8], uint64_t v) {
  store32_le(b, (uint32_t)(v & 0xFFFFFFFFULL));
  store32_le(b + 4, (uint32_t)(v >> 32));
}

static uint64_t load64_be(const uint8_t b[8]) {
  return ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) | ((uint64_t)b[2] << 40) |
         ((uint64_t)b[3] << 32) | ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
         ((uint64_t)b[6] << 8) | ((uint64_t)b[7]);
}

static void store64_be(uint8_t b[8], uint64_t v) {
  b[0] = (uint8_t)((v >> 56) & 0xFFU);
  b[1] = (uint8_t)((v >> 48) & 0xFFU);
  b[2] = (uint8_t)((v >> 40) & 0xFFU);
  b[3] = (uint8_t)((v >> 32) & 0xFFU);
  b[4] = (uint8_t)((v >> 24) & 0xFFU);
  b[5] = (uint8_t)((v >> 16) & 0xFFU);
  b[6] = (uint8_t)((v >> 8) & 0xFFU);
  b[7] = (uint8_t)(v & 0xFFU);
}

static uint32_t rotl32(uint32_t x, unsigned n) {
  return (x << n) | (x >> (32U - n));
}

#define CHACHA_QR(a, b, c, d)                                                          \
  do {                                                                                 \
    (a) += (b);                                                                        \
    (d) ^= (a);                                                                        \
    (d) = rotl32((d), 16U);                                                            \
    (c) += (d);                                                                        \
    (b) ^= (c);                                                                        \
    (b) = rotl32((b), 12U);                                                            \
    (a) += (b);                                                                        \
    (d) ^= (a);                                                                        \
    (d) = rotl32((d), 8U);                                                             \
    (c) += (d);                                                                        \
    (b) ^= (c);                                                                        \
    (b) = rotl32((b), 7U);                                                             \
  } while (0)

static void chacha20_block(const uint8_t key[32], uint32_t counter,
                           const uint8_t nonce[12], uint8_t out[64]) {
  uint32_t st[16];
  uint32_t w[16];

  st[0] = 0x61707865U;
  st[1] = 0x3320646eU;
  st[2] = 0x79622d32U;
  st[3] = 0x6b206574U;
  for (unsigned i = 0; i < 8U; ++i) {
    st[4U + i] = load32_le(key + (i * 4U));
  }
  st[12] = counter;
  st[13] = load32_le(nonce + 0);
  st[14] = load32_le(nonce + 4);
  st[15] = load32_le(nonce + 8);

  for (unsigned i = 0; i < 16U; ++i) {
    w[i] = st[i];
  }

  for (unsigned round = 0; round < 10U; ++round) {
    CHACHA_QR(w[0], w[4], w[8], w[12]);
    CHACHA_QR(w[1], w[5], w[9], w[13]);
    CHACHA_QR(w[2], w[6], w[10], w[14]);
    CHACHA_QR(w[3], w[7], w[11], w[15]);

    CHACHA_QR(w[0], w[5], w[10], w[15]);
    CHACHA_QR(w[1], w[6], w[11], w[12]);
    CHACHA_QR(w[2], w[7], w[8], w[13]);
    CHACHA_QR(w[3], w[4], w[9], w[14]);
  }

  for (unsigned i = 0; i < 16U; ++i) {
    store32_le(out + (i * 4U), w[i] + st[i]);
  }
  secure_zero(w, sizeof(w));
  secure_zero(st, sizeof(st));
}

static void chacha20_xor_stream(const uint8_t key[32], uint32_t initial_counter,
                                const uint8_t nonce[12], const uint8_t *in,
                                uint8_t *out, size_t len) {
  uint8_t block[64];
  uint32_t counter = initial_counter;
  size_t offset = 0;

  while (offset < len) {
    chacha20_block(key, counter, nonce, block);
    counter++;
    size_t chunk = (len - offset < 64U) ? (len - offset) : 64U;
    for (size_t i = 0; i < chunk; ++i) {
      out[offset + i] = (uint8_t)(in[offset + i] ^ block[i]);
    }
    offset += chunk;
  }
  secure_zero(block, sizeof(block));
}

typedef struct {
  uint32_t r[5];
  uint32_t s[4];
  uint32_t h[5];
  uint32_t pad[4];
  uint8_t buf[16];
  size_t leftover;
} poly1305_state_s;

static void poly1305_init(poly1305_state_s *st, const uint8_t otk[32]) {
  uint8_t clamped[16];
  memcpy(clamped, otk, 16U);
  clamped[3] &= 15U;
  clamped[7] &= 15U;
  clamped[11] &= 15U;
  clamped[15] &= 15U;
  clamped[4] &= 252U;
  clamped[8] &= 252U;
  clamped[12] &= 252U;

  st->r[0] = load32_le(clamped + 0) & 0x3ffffffU;
  st->r[1] = (load32_le(clamped + 3) >> 2) & 0x3ffff03U;
  st->r[2] = (load32_le(clamped + 6) >> 4) & 0x3ffc0ffU;
  st->r[3] = (load32_le(clamped + 9) >> 6) & 0x3f03fffU;
  st->r[4] = (load32_le(clamped + 12) >> 8) & 0x00fffffU;

  st->s[0] = st->r[1] * 5U;
  st->s[1] = st->r[2] * 5U;
  st->s[2] = st->r[3] * 5U;
  st->s[3] = st->r[4] * 5U;

  st->h[0] = 0U;
  st->h[1] = 0U;
  st->h[2] = 0U;
  st->h[3] = 0U;
  st->h[4] = 0U;

  st->pad[0] = load32_le(otk + 16);
  st->pad[1] = load32_le(otk + 20);
  st->pad[2] = load32_le(otk + 24);
  st->pad[3] = load32_le(otk + 28);
  st->leftover = 0U;
  secure_zero(clamped, sizeof(clamped));
}

static void poly1305_blocks(poly1305_state_s *st, const uint8_t *m, size_t bytes,
                            uint32_t hibit) {
  uint32_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2], r3 = st->r[3], r4 = st->r[4];
  uint32_t s1 = st->s[0], s2 = st->s[1], s3 = st->s[2], s4 = st->s[3];
  uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4];

  while (bytes >= 16U) {
    h0 += load32_le(m + 0) & 0x3ffffffU;
    h1 += (load32_le(m + 3) >> 2) & 0x3ffffffU;
    h2 += (load32_le(m + 6) >> 4) & 0x3ffffffU;
    h3 += (load32_le(m + 9) >> 6) & 0x3ffffffU;
    h4 += (load32_le(m + 12) >> 8) | hibit;

    uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 +
                  (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
    uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 +
                  (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
    uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 +
                  (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
    uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 +
                  (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
    uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 +
                  (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

    uint32_t c = (uint32_t)(d0 >> 26);
    h0 = (uint32_t)d0 & 0x3ffffffU;
    d1 += c;
    c = (uint32_t)(d1 >> 26);
    h1 = (uint32_t)d1 & 0x3ffffffU;
    d2 += c;
    c = (uint32_t)(d2 >> 26);
    h2 = (uint32_t)d2 & 0x3ffffffU;
    d3 += c;
    c = (uint32_t)(d3 >> 26);
    h3 = (uint32_t)d3 & 0x3ffffffU;
    d4 += c;
    c = (uint32_t)(d4 >> 26);
    h4 = (uint32_t)d4 & 0x3ffffffU;
    h0 += c * 5U;
    c = h0 >> 26;
    h0 &= 0x3ffffffU;
    h1 += c;

    m += 16U;
    bytes -= 16U;
  }

  st->h[0] = h0;
  st->h[1] = h1;
  st->h[2] = h2;
  st->h[3] = h3;
  st->h[4] = h4;
}

static void poly1305_update(poly1305_state_s *st, const uint8_t *m, size_t bytes) {
  if (bytes == 0U) {
    return;
  }
  if (st->leftover > 0U) {
    size_t want = 16U - st->leftover;
    if (want > bytes) {
      want = bytes;
    }
    memcpy(st->buf + st->leftover, m, want);
    bytes -= want;
    m += want;
    st->leftover += want;
    if (st->leftover < 16U) {
      return;
    }
    poly1305_blocks(st, st->buf, 16U, (1U << 24));
    st->leftover = 0U;
  }
  if (bytes >= 16U) {
    size_t full = bytes & ~(size_t)15U;
    poly1305_blocks(st, m, full, (1U << 24));
    m += full;
    bytes -= full;
  }
  if (bytes > 0U) {
    memcpy(st->buf, m, bytes);
    st->leftover = bytes;
  }
}

static void poly1305_pad16(poly1305_state_s *st, size_t section_len) {
  static const uint8_t zeros[16] = {0};
  size_t rem = section_len & 15U;
  if (rem != 0U) {
    poly1305_update(st, zeros, 16U - rem);
  }
}

static void poly1305_finish(poly1305_state_s *st, uint8_t tag[16]) {
  if (st->leftover > 0U) {
    size_t i = st->leftover;
    st->buf[i++] = 1U;
    while (i < 16U) {
      st->buf[i++] = 0U;
    }
    poly1305_blocks(st, st->buf, 16U, 0U);
  }

  uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4];
  uint32_t c = h1 >> 26;
  h1 &= 0x3ffffffU;
  h2 += c;
  c = h2 >> 26;
  h2 &= 0x3ffffffU;
  h3 += c;
  c = h3 >> 26;
  h3 &= 0x3ffffffU;
  h4 += c;
  c = h4 >> 26;
  h4 &= 0x3ffffffU;
  h0 += c * 5U;
  c = h0 >> 26;
  h0 &= 0x3ffffffU;
  h1 += c;

  uint32_t g0 = h0 + 5U;
  c = g0 >> 26;
  g0 &= 0x3ffffffU;
  uint32_t g1 = h1 + c;
  c = g1 >> 26;
  g1 &= 0x3ffffffU;
  uint32_t g2 = h2 + c;
  c = g2 >> 26;
  g2 &= 0x3ffffffU;
  uint32_t g3 = h3 + c;
  c = g3 >> 26;
  g3 &= 0x3ffffffU;
  uint32_t g4 = h4 + c - (1U << 26);

  uint32_t mask = (g4 >> 31) - 1U;
  g0 &= mask;
  g1 &= mask;
  g2 &= mask;
  g3 &= mask;
  g4 &= mask;
  mask = ~mask;
  h0 = (h0 & mask) | g0;
  h1 = (h1 & mask) | g1;
  h2 = (h2 & mask) | g2;
  h3 = (h3 & mask) | g3;
  h4 = (h4 & mask) | g4;

  uint32_t w0 = (h0) | (h1 << 26);
  uint32_t w1 = (h1 >> 6) | (h2 << 20);
  uint32_t w2 = (h2 >> 12) | (h3 << 14);
  uint32_t w3 = (h3 >> 18) | (h4 << 8);

  uint64_t f = (uint64_t)w0 + st->pad[0];
  w0 = (uint32_t)f;
  f = (uint64_t)w1 + st->pad[1] + (f >> 32);
  w1 = (uint32_t)f;
  f = (uint64_t)w2 + st->pad[2] + (f >> 32);
  w2 = (uint32_t)f;
  f = (uint64_t)w3 + st->pad[3] + (f >> 32);
  w3 = (uint32_t)f;

  store32_le(tag + 0, w0);
  store32_le(tag + 4, w1);
  store32_le(tag + 8, w2);
  store32_le(tag + 12, w3);
  secure_zero(st, sizeof(*st));
}

static void compute_aead_tag(const uint8_t otk[32], const uint8_t *aad, size_t aad_len,
                             const uint8_t *ct, size_t ct_len, uint8_t tag[16]) {
  poly1305_state_s st;
  uint8_t lengths[16];

  poly1305_init(&st, otk);
  if (aad_len > 0U) {
    poly1305_update(&st, aad, aad_len);
    poly1305_pad16(&st, aad_len);
  }
  if (ct_len > 0U) {
    poly1305_update(&st, ct, ct_len);
    poly1305_pad16(&st, ct_len);
  }
  store64_le(lengths + 0, (uint64_t)aad_len);
  store64_le(lengths + 8, (uint64_t)ct_len);
  poly1305_update(&st, lengths, 16U);
  poly1305_finish(&st, tag);
}

static uint32_t ct_tag_equal(const uint8_t a[16], const uint8_t b[16]) {
  uint32_t diff = 0U;
  for (size_t i = 0; i < 16U; ++i) {
    diff |= (uint32_t)(a[i] ^ b[i]);
  }
  return (diff - 1U) >> 31;
}

int rudp_chacha20_poly1305_encrypt(const uint8_t key[RUDP_CRYPTO_KEY_BYTES],
                                   const uint8_t nonce[RUDP_CRYPTO_NONCE_BYTES],
                                   const uint8_t *aad, size_t aad_len,
                                   const uint8_t *plaintext, size_t pt_len,
                                   uint8_t *ciphertext,
                                   uint8_t tag[RUDP_CRYPTO_TAG_BYTES]) {
  if (!key || !nonce || !tag) {
    return RUDP_ERR_INVALID_ARG;
  }
  if ((aad_len > 0U && !aad) || (pt_len > 0U && (!plaintext || !ciphertext))) {
    return RUDP_ERR_INVALID_ARG;
  }
  /* RFC 8439 32-bit block counter limit (~256 GiB per message). */
  if ((uint64_t)pt_len > (UINT64_C(0xFFFFFFFF) - 1ULL) * 64ULL) {
    return RUDP_ERR_INVALID_ARG;
  }

  uint8_t otk_block[64];
  chacha20_block(key, 0U, nonce, otk_block);
  if (pt_len > 0U) {
    chacha20_xor_stream(key, 1U, nonce, plaintext, ciphertext, pt_len);
  }
  compute_aead_tag(otk_block, aad, aad_len, ciphertext, pt_len, tag);
  secure_zero(otk_block, sizeof(otk_block));
  return RUDP_OK;
}

int rudp_chacha20_poly1305_decrypt(const uint8_t key[RUDP_CRYPTO_KEY_BYTES],
                                   const uint8_t nonce[RUDP_CRYPTO_NONCE_BYTES],
                                   const uint8_t *aad, size_t aad_len,
                                   const uint8_t *ciphertext, size_t ct_len,
                                   const uint8_t tag[RUDP_CRYPTO_TAG_BYTES],
                                   uint8_t *plaintext) {
  if (!key || !nonce || !tag) {
    return RUDP_ERR_INVALID_ARG;
  }
  if ((aad_len > 0U && !aad) || (ct_len > 0U && (!ciphertext || !plaintext))) {
    return RUDP_ERR_INVALID_ARG;
  }
  if ((uint64_t)ct_len > (UINT64_C(0xFFFFFFFF) - 1ULL) * 64ULL) {
    return RUDP_ERR_INVALID_ARG;
  }

  uint8_t otk_block[64];
  uint8_t computed_tag[16];
  chacha20_block(key, 0U, nonce, otk_block);
  compute_aead_tag(otk_block, aad, aad_len, ciphertext, ct_len, computed_tag);

  uint32_t ok = ct_tag_equal(computed_tag, tag);
  secure_zero(otk_block, sizeof(otk_block));
  secure_zero(computed_tag, sizeof(computed_tag));

  if (!ok) {
    return RUDP_ERR_AUTH;
  }
  if (ct_len > 0U) {
    chacha20_xor_stream(key, 1U, nonce, ciphertext, plaintext, ct_len);
  }
  return RUDP_OK;
}

static void derive_directional_subkey(const uint8_t shared_key[32], uint64_t session_id,
                                      uint8_t direction, uint8_t out_subkey[32]) {
  uint8_t prf_nonce[12];
  uint8_t block[64];
  /* Domain-separated 96-bit PRF input: "ZR5" || direction || session_id_be64 */
  prf_nonce[0] = (uint8_t)'Z';
  prf_nonce[1] = (uint8_t)'R';
  prf_nonce[2] = (uint8_t)'5';
  prf_nonce[3] = direction;
  store64_be(prf_nonce + 4, session_id);

  chacha20_block(shared_key, 0x7A637275U, prf_nonce, block);
  memcpy(out_subkey, block, 32U);
  secure_zero(block, sizeof(block));
}

static void build_packet_nonce(uint64_t session_id, uint8_t direction,
                               uint64_t packet_seq, uint8_t out_nonce[12]) {
  uint32_t prefix =
      (uint32_t)(session_id >> 32) ^ (((uint32_t)direction ^ 0x5AU) << 24);
  out_nonce[0] = (uint8_t)((prefix >> 24) & 0xFFU);
  out_nonce[1] = (uint8_t)((prefix >> 16) & 0xFFU);
  out_nonce[2] = (uint8_t)((prefix >> 8) & 0xFFU);
  out_nonce[3] = (uint8_t)(prefix & 0xFFU);
  store64_be(out_nonce + 4, packet_seq);
}

int rudp_crypto_init(rudp_crypto_ctx_s *ctx,
                     const uint8_t shared_key[RUDP_CRYPTO_KEY_BYTES],
                     uint64_t session_id, uint8_t local_is_initiator) {
  if (!ctx || !shared_key) {
    return RUDP_ERR_INVALID_ARG;
  }
  secure_zero(ctx, sizeof(*ctx));
  ctx->session_id = session_id;
  ctx->tx_direction =
      local_is_initiator ? RUDP_CRYPTO_DIR_INITIATOR : RUDP_CRYPTO_DIR_RESPONDER;
  ctx->rx_direction =
      local_is_initiator ? RUDP_CRYPTO_DIR_RESPONDER : RUDP_CRYPTO_DIR_INITIATOR;
  derive_directional_subkey(shared_key, session_id, ctx->tx_direction, ctx->tx_key);
  derive_directional_subkey(shared_key, session_id, ctx->rx_direction, ctx->rx_key);
  ctx->initialized = 1U;
  return RUDP_OK;
}

int rudp_crypto_set_aead(rudp_crypto_ctx_s *ctx, const rudp_aead_ops_s *ops) {
  if (!ctx || !ctx->initialized) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (!ops) {
    ctx->custom_ops = (rudp_aead_ops_s){0};
    return RUDP_OK;
  }
  if (!ops->encrypt || !ops->decrypt) {
    return RUDP_ERR_INVALID_ARG;
  }
  ctx->custom_ops = *ops;
  return RUDP_OK;
}

void rudp_crypto_wipe(rudp_crypto_ctx_s *ctx) {
  if (ctx) {
    secure_zero(ctx, sizeof(*ctx));
  }
}

int rudp_crypto_seal(rudp_crypto_ctx_s *ctx, const uint8_t *aad, size_t aad_len,
                     const uint8_t *plaintext, size_t pt_len, uint8_t *out_wire,
                     size_t max_out_len) {
  if (!ctx || !ctx->initialized || !out_wire) {
    return RUDP_ERR_INVALID_ARG;
  }
  if ((aad_len > 0U && !aad) || (pt_len > 0U && !plaintext)) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (pt_len > (size_t)INT_MAX - RUDP_CRYPTO_OVERHEAD_BYTES) {
    return RUDP_ERR_INVALID_ARG;
  }
  size_t total_len = pt_len + RUDP_CRYPTO_OVERHEAD_BYTES;
  if (max_out_len < total_len) {
    return RUDP_ERR_BUFFER_FULL;
  }
  if (ctx->tx_nonce == UINT64_MAX) {
    return RUDP_ERR_DISCONNECTED;
  }

  uint64_t pkt_seq = ctx->tx_nonce;
  uint8_t nonce[RUDP_CRYPTO_NONCE_BYTES];
  build_packet_nonce(ctx->session_id, ctx->tx_direction, pkt_seq, nonce);

  /* Support aliasing between plaintext and out_wire safely via memmove before XOR. */
  if (pt_len > 0U && plaintext != out_wire + RUDP_CRYPTO_WIRE_NONCE_BYTES) {
    memmove(out_wire + RUDP_CRYPTO_WIRE_NONCE_BYTES, plaintext, pt_len);
  }
  store64_be(out_wire, pkt_seq);

  uint8_t *ct_dst = out_wire + RUDP_CRYPTO_WIRE_NONCE_BYTES;
  uint8_t *tag_dst = ct_dst + pt_len;
  int ret;
  if (ctx->custom_ops.encrypt) {
    ret = ctx->custom_ops.encrypt(ctx->custom_ops.user_ctx, ctx->tx_key, nonce, aad,
                                  aad_len, ct_dst, pt_len, ct_dst, tag_dst);
  } else {
    ret = rudp_chacha20_poly1305_encrypt(ctx->tx_key, nonce, aad, aad_len, ct_dst,
                                         pt_len, ct_dst, tag_dst);
  }
  secure_zero(nonce, sizeof(nonce));
  if (ret != RUDP_OK) {
    return ret;
  }

  ctx->tx_nonce++;
  return (int)total_len;
}

int rudp_crypto_open(rudp_crypto_ctx_s *ctx, const uint8_t *aad, size_t aad_len,
                     const uint8_t *in_wire, size_t in_wire_len,
                     uint8_t *out_plaintext, size_t max_pt_len) {
  if (!ctx || !ctx->initialized || !in_wire) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (aad_len > 0U && !aad) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (in_wire_len < RUDP_CRYPTO_OVERHEAD_BYTES) {
    return RUDP_ERR_AUTH;
  }
  size_t ct_len = in_wire_len - RUDP_CRYPTO_OVERHEAD_BYTES;
  if (ct_len > (size_t)INT_MAX) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (ct_len > 0U && !out_plaintext) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (max_pt_len < ct_len) {
    return RUDP_ERR_BUFFER_FULL;
  }

  /* 1. Anti-replay pre-check (read-only: zero state mutation on unverified input). */
  uint64_t pkt_seq = load64_be(in_wire);
  if (ctx->rx_initialized && pkt_seq <= ctx->rx_highest_nonce) {
    uint64_t diff = ctx->rx_highest_nonce - pkt_seq;
    if (diff >= 64U) {
      return RUDP_ERR_AUTH;
    }
    if ((ctx->rx_replay_bitmap >> diff) & UINT64_C(1)) {
      return RUDP_ERR_AUTH;
    }
  }

  /* 2. Constant-time AEAD tag verification BEFORE mutating replay state or output. */
  uint8_t nonce[RUDP_CRYPTO_NONCE_BYTES];
  build_packet_nonce(ctx->session_id, ctx->rx_direction, pkt_seq, nonce);
  const uint8_t *ct_src = in_wire + RUDP_CRYPTO_WIRE_NONCE_BYTES;
  const uint8_t *tag_src = ct_src + ct_len;

  int ret;
  if (ctx->custom_ops.decrypt) {
    ret = ctx->custom_ops.decrypt(ctx->custom_ops.user_ctx, ctx->rx_key, nonce, aad,
                                  aad_len, ct_src, ct_len, tag_src, out_plaintext);
  } else {
    ret = rudp_chacha20_poly1305_decrypt(ctx->rx_key, nonce, aad, aad_len, ct_src,
                                         ct_len, tag_src, out_plaintext);
  }
  secure_zero(nonce, sizeof(nonce));
  if (ret != RUDP_OK) {
    return RUDP_ERR_AUTH;
  }

  /* 3. Commit 64-packet sliding anti-replay window only after authentic decryption. */
  if (!ctx->rx_initialized) {
    ctx->rx_initialized = 1U;
    ctx->rx_highest_nonce = pkt_seq;
    ctx->rx_replay_bitmap = UINT64_C(1);
  } else if (pkt_seq > ctx->rx_highest_nonce) {
    uint64_t shift = pkt_seq - ctx->rx_highest_nonce;
    ctx->rx_replay_bitmap =
        (shift >= 64U) ? UINT64_C(1) : ((ctx->rx_replay_bitmap << shift) | UINT64_C(1));
    ctx->rx_highest_nonce = pkt_seq;
  } else {
    uint64_t diff = ctx->rx_highest_nonce - pkt_seq;
    ctx->rx_replay_bitmap |= (UINT64_C(1) << diff);
  }

  return (int)ct_len;
}
