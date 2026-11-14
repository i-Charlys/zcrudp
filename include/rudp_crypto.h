#ifndef RUDP_CRYPTO_H
#define RUDP_CRYPTO_H

/**
 * @file rudp_crypto.h
 * @brief Zero-dependency ChaCha20-Poly1305 IETF AEAD (RFC 8439) datagram envelope,
 *        directional subkey derivation, and 64-packet sliding anti-replay window.
 *
 * Designed for low-latency UDP transports:
 * - Zero dynamic allocation (`malloc`/`free` never called).
 * - Strict verify-before-mutate: invalid tags, replays, or reflected packets
 *   return `RUDP_ERR_AUTH` without mutating anti-replay or session state.
 * - Optional `rudp_aead_ops_s` vtable allows hardware AES-GCM / HSM offload
 *   while defaulting (`ops == NULL`) to built-in constant-time ChaCha20-Poly1305.
 */

#include "rudp_config.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 256-bit symmetric key size in bytes */
#define RUDP_CRYPTO_KEY_BYTES 32U
/** 96-bit IETF AEAD nonce size in bytes */
#define RUDP_CRYPTO_NONCE_BYTES 12U
/** 128-bit Poly1305 authentication tag size in bytes */
#define RUDP_CRYPTO_TAG_BYTES 16U
/** Explicit 64-bit monotonic packet counter carried on wire */
#define RUDP_CRYPTO_WIRE_NONCE_BYTES 8U
/** Total wire overhead added by rudp_crypto_seal(): 8B nonce + 16B tag = 24B */
#define RUDP_CRYPTO_OVERHEAD_BYTES                                                     \
  (RUDP_CRYPTO_WIRE_NONCE_BYTES + RUDP_CRYPTO_TAG_BYTES)

/** Direction identifier for initiator -> responder traffic */
#define RUDP_CRYPTO_DIR_INITIATOR 0U
/** Direction identifier for responder -> initiator traffic */
#define RUDP_CRYPTO_DIR_RESPONDER 1U

/**
 * @brief Optional pluggable AEAD backend (e.g., hardware AES-256-GCM or MCU CRYP).
 */
typedef struct {
  int (*encrypt)(void *user_ctx, const uint8_t key[RUDP_CRYPTO_KEY_BYTES],
                 const uint8_t nonce[RUDP_CRYPTO_NONCE_BYTES], const uint8_t *aad,
                 size_t aad_len, const uint8_t *plaintext, size_t pt_len,
                 uint8_t *ciphertext, uint8_t tag[RUDP_CRYPTO_TAG_BYTES]);
  int (*decrypt)(void *user_ctx, const uint8_t key[RUDP_CRYPTO_KEY_BYTES],
                 const uint8_t nonce[RUDP_CRYPTO_NONCE_BYTES], const uint8_t *aad,
                 size_t aad_len, const uint8_t *ciphertext, size_t ct_len,
                 const uint8_t tag[RUDP_CRYPTO_TAG_BYTES], uint8_t *plaintext);
  void *user_ctx;
} rudp_aead_ops_s;

/**
 * @brief Bidirectional AEAD + 64-packet anti-replay context for one peer session.
 */
typedef struct {
  uint8_t tx_key[RUDP_CRYPTO_KEY_BYTES];
  uint8_t rx_key[RUDP_CRYPTO_KEY_BYTES];
  uint64_t session_id;
  uint64_t tx_nonce;
  uint64_t rx_highest_nonce;
  uint64_t rx_replay_bitmap;
  rudp_aead_ops_s custom_ops;
  uint8_t tx_direction;
  uint8_t rx_direction;
  uint8_t rx_initialized;
  uint8_t initialized;
} rudp_crypto_ctx_s;

/**
 * @brief Standalone RFC 8439 ChaCha20-Poly1305 IETF AEAD encryption.
 *        Supports in-place (`ciphertext == plaintext`) and separate buffers.
 */
RUDP_EXPORT int rudp_chacha20_poly1305_encrypt(
    const uint8_t key[RUDP_CRYPTO_KEY_BYTES],
    const uint8_t nonce[RUDP_CRYPTO_NONCE_BYTES], const uint8_t *aad, size_t aad_len,
    const uint8_t *plaintext, size_t pt_len, uint8_t *ciphertext,
    uint8_t tag[RUDP_CRYPTO_TAG_BYTES]);

/**
 * @brief Standalone RFC 8439 ChaCha20-Poly1305 IETF AEAD decryption.
 *        Verifies the 16-byte tag in constant time BEFORE writing any output byte.
 *
 * @return RUDP_OK on authentic ciphertext, RUDP_ERR_AUTH on tag mismatch, or
 *         RUDP_ERR_INVALID_ARG on NULL arguments.
 */
RUDP_EXPORT int rudp_chacha20_poly1305_decrypt(
    const uint8_t key[RUDP_CRYPTO_KEY_BYTES],
    const uint8_t nonce[RUDP_CRYPTO_NONCE_BYTES], const uint8_t *aad, size_t aad_len,
    const uint8_t *ciphertext, size_t ct_len, const uint8_t tag[RUDP_CRYPTO_TAG_BYTES],
    uint8_t *plaintext);

/**
 * @brief Initializes a bidirectional datagram crypto context from a 32-byte shared key.
 *        Derives distinct directional TX/RX subkeys bound to @p session_id to prevent
 *        cross-direction reflection and key-reuse attacks.
 *
 * @param ctx Pointer to caller-allocated crypto context.
 * @param shared_key 32-byte pre-shared or handshake-derived secret key.
 * @param session_id 64-bit session identifier bound into subkeys and AAD.
 * @param local_is_initiator Non-zero on client/initiator, 0 on server/responder.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG.
 */
RUDP_EXPORT int rudp_crypto_init(rudp_crypto_ctx_s *ctx,
                                 const uint8_t shared_key[RUDP_CRYPTO_KEY_BYTES],
                                 uint64_t session_id, uint8_t local_is_initiator);

/**
 * @brief Installs or clears (`ops == NULL`) a custom hardware/OS AEAD backend.
 */
RUDP_EXPORT int rudp_crypto_set_aead(rudp_crypto_ctx_s *ctx,
                                     const rudp_aead_ops_s *ops);

/**
 * @brief Zeroizes all key material and counters in @p ctx against dead-store removal.
 */
RUDP_EXPORT void rudp_crypto_wipe(rudp_crypto_ctx_s *ctx);

/**
 * @brief Seals a raw zcrudp datagram into `[8B nonce][ciphertext][16B tag]`.
 *        `out_wire` may alias `plaintext` only if non-overlapping; distinct buffers
 *        are recommended because an 8-byte nonce prefix is prepended.
 *
 * @return Total wire bytes written (`pt_len + RUDP_CRYPTO_OVERHEAD_BYTES`),
 *         RUDP_ERR_BUFFER_FULL if `max_out_len < pt_len + 24`,
 *         RUDP_ERR_DISCONNECTED on nonce exhaustion, or RUDP_ERR_INVALID_ARG.
 */
RUDP_EXPORT int rudp_crypto_seal(rudp_crypto_ctx_s *ctx, const uint8_t *aad,
                                 size_t aad_len, const uint8_t *plaintext,
                                 size_t pt_len, uint8_t *out_wire, size_t max_out_len);

/**
 * @brief Authenticates, checks the 64-packet anti-replay window, and decrypts a
 *        sealed wire datagram. Never mutates @p ctx or @p out_plaintext on failure.
 *
 * @return Decrypted plaintext length (`in_wire_len - 24`), RUDP_ERR_AUTH on
 *         tampering/replay/reflection/truncation, RUDP_ERR_BUFFER_FULL if
 *         `max_pt_len` is too small, or RUDP_ERR_INVALID_ARG.
 */
RUDP_EXPORT int rudp_crypto_open(rudp_crypto_ctx_s *ctx, const uint8_t *aad,
                                 size_t aad_len, const uint8_t *in_wire,
                                 size_t in_wire_len, uint8_t *out_plaintext,
                                 size_t max_pt_len);

#ifdef __cplusplus
}
#endif

#endif /* RUDP_CRYPTO_H */
