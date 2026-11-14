#ifndef ZCRUDP_HPP
#define ZCRUDP_HPP

/**
 * @file zcrudp.hpp
 * @brief Header-only C++11/17/20 RAII wrappers over the zcrudp C11 core library.
 *        Zero heap allocation; all storage stays caller-owned on stack or static arena.
 */

#if defined(__has_include)
#if __has_include("protocol_rudp.h")
#include "protocol_profiles.h"
#include "protocol_rudp.h"
#else
#include "../../include/protocol_profiles.h"
#include "../../include/protocol_rudp.h"
#endif
#else
#include "../../include/protocol_profiles.h"
#include "../../include/protocol_rudp.h"
#endif
#include <cstddef>
#include <cstdint>

namespace zcrudp {

class Session {
public:
  Session() { (void)rudp_session_init(&raw_); }

  int reset() { return rudp_session_init(&raw_); }

  int reset_channel(uint8_t channel_id) {
    return rudp_session_reset_channel(&raw_, channel_id);
  }

  int set_seed(uint16_t seed) { return rudp_session_set_seed(&raw_, seed); }

  int set_mtu(size_t mtu_bytes) { return rudp_session_set_mtu(&raw_, mtu_bytes); }

  int config_channel(uint8_t channel_id, uint8_t flags) {
    return rudp_session_config_channel(&raw_, channel_id, flags);
  }

  int set_qos(uint8_t channel_id, uint8_t priority, uint8_t dscp) {
    return rudp_session_set_qos(&raw_, channel_id, priority, dscp);
  }

  int config_recovery(uint8_t channel_id, uint32_t initial_ms, uint32_t min_ms,
                      uint32_t max_ms) {
    return rudp_session_config_recovery(&raw_, channel_id, initial_ms, min_ms,
                                        max_ms);
  }

  int send_reliable(uint8_t channel_id, tfv_packet_u payload, uint32_t now_ms) {
    return rudp_session_send_reliable(&raw_, channel_id, payload, now_ms);
  }

  int queue_unreliable(uint8_t channel_id, tfv_packet_u payload) {
    return rudp_session_queue_unreliable(&raw_, channel_id, payload);
  }

  int send_unreliable(uint8_t channel_id, tfv_packet_u payload, uint8_t ack_channel,
                      uint8_t *out_buf, size_t max_len) {
    return rudp_session_send_unreliable(&raw_, channel_id, payload, ack_channel,
                                        out_buf, max_len);
  }

  int build_datagram(uint8_t primary_ack_channel, uint8_t *out_buf, size_t max_len,
                     uint32_t now_ms, uint32_t fallback_timeout_ms) {
    return rudp_session_build_datagram(&raw_, primary_ack_channel, out_buf, max_len,
                                       now_ms, fallback_timeout_ms);
  }

  int prepare_datagram(uint32_t now_ms, uint32_t fallback_timeout_ms,
                       uint8_t *out_buf, size_t cap, rudp_tx_plan_s *out_plan) {
    return rudp_session_prepare_datagram(&raw_, now_ms, fallback_timeout_ms, out_buf,
                                         cap, out_plan);
  }

  int commit_datagram(const rudp_tx_plan_s *plan) {
    return rudp_session_commit_datagram(&raw_, plan);
  }

  int cancel_datagram(const rudp_tx_plan_s *plan) {
    return rudp_session_cancel_datagram(&raw_, plan);
  }

  int build_iovec(uint8_t primary_ack_channel, uint16_t flow_hint,
                  uint8_t *scratch_buf, size_t max_len, uint32_t now_ms,
                  uint32_t fallback_timeout_ms, rudp_iovec_collection_s *out_col) {
    return rudp_session_build_iovec(&raw_, primary_ack_channel, flow_hint, scratch_buf,
                                    max_len, now_ms, fallback_timeout_ms, out_col);
  }

  int prepare_iovec(uint16_t flow_hint, uint32_t now_ms, uint32_t fallback_timeout_ms,
                    uint8_t *scratch_buf, size_t max_len,
                    rudp_iovec_collection_s *out_col, rudp_tx_plan_s *out_plan) {
    return rudp_session_prepare_iovec(&raw_, flow_hint, now_ms, fallback_timeout_ms,
                                      scratch_buf, max_len, out_col, out_plan);
  }

  uint32_t next_wakeup_ms(uint32_t now_ms, uint32_t fallback_timeout_ms) const {
    return rudp_session_next_wakeup_ms(&raw_, now_ms, fallback_timeout_ms);
  }

  bool has_pending_work(uint32_t now_ms, uint32_t fallback_timeout_ms) const {
    return rudp_session_has_pending_work(&raw_, now_ms, fallback_timeout_ms);
  }

  int process_datagram(const uint8_t *in_buf, size_t in_len,
                       rudp_record_s *out_delivered, size_t max_delivered) {
    return rudp_session_process_datagram(&raw_, in_buf, in_len, out_delivered,
                                         max_delivered);
  }

  int process_datagram_at(const uint8_t *in_buf, size_t in_len,
                          rudp_record_s *out_delivered, size_t max_delivered,
                          uint32_t now_ms) {
    return rudp_session_process_datagram_at(&raw_, in_buf, in_len, out_delivered,
                                            max_delivered, now_ms);
  }

  int process_datagram_ex(const uint8_t *in_buf, size_t in_len,
                          rudp_record_s *out_delivered, size_t max_delivered,
                          bool timed, uint32_t now_ms, rudp_stats_s *stats) {
    return rudp_session_process_datagram_ex(&raw_, in_buf, in_len, out_delivered,
                                            max_delivered, timed, now_ms, stats);
  }

  int poll(rudp_record_s *out, size_t capacity) {
    return rudp_session_poll(&raw_, out, capacity);
  }

  rudp_session_s *raw() { return &raw_; }
  const rudp_session_s *raw() const { return &raw_; }

private:
  rudp_session_s raw_;
};

class CryptoContext {
public:
  CryptoContext() : raw_() {}
  ~CryptoContext() { wipe(); }

  CryptoContext(const CryptoContext &) = delete;
  CryptoContext &operator=(const CryptoContext &) = delete;

  int init(const uint8_t shared_key[RUDP_CRYPTO_KEY_BYTES], uint64_t session_id,
           uint8_t local_is_initiator) {
    return rudp_crypto_init(&raw_, shared_key, session_id, local_is_initiator);
  }

  int set_aead(const rudp_aead_ops_s *ops) { return rudp_crypto_set_aead(&raw_, ops); }

  void wipe() { rudp_crypto_wipe(&raw_); }

  int seal(const uint8_t *aad, size_t aad_len, const uint8_t *plaintext, size_t pt_len,
           uint8_t *out_wire, size_t max_out_len) {
    return rudp_crypto_seal(&raw_, aad, aad_len, plaintext, pt_len, out_wire,
                            max_out_len);
  }

  int open(const uint8_t *aad, size_t aad_len, const uint8_t *in_wire,
           size_t in_wire_len, uint8_t *out_plaintext, size_t max_pt_len) {
    return rudp_crypto_open(&raw_, aad, aad_len, in_wire, in_wire_len, out_plaintext,
                            max_pt_len);
  }

  rudp_crypto_ctx_s *raw() { return &raw_; }
  const rudp_crypto_ctx_s *raw() const { return &raw_; }

private:
  rudp_crypto_ctx_s raw_;
};

class StreamTx {
public:
  StreamTx() : raw_() { raw_.data = NULL; }

  int start(const void *data, size_t length) {
    return rudp_stream_start(&raw_, data, length);
  }

  int pump(Session &session, uint8_t channel, uint32_t now_ms) {
    return rudp_stream_pump(&raw_, session.raw(), channel, now_ms);
  }

  int reset() { return rudp_stream_tx_reset(&raw_); }

  rudp_stream_tx_s *raw() { return &raw_; }

private:
  rudp_stream_tx_s raw_;
};

class StreamRx {
public:
  StreamRx(uint8_t *buf, size_t capacity) : raw_() {
    raw_.data = buf;
    raw_.capacity = capacity;
  }

  int receive(tfv_packet_u payload) { return rudp_stream_receive(&raw_, payload); }

  int reset() { return rudp_stream_rx_reset(&raw_); }

  uint16_t length() const { return raw_.length; }

  rudp_stream_rx_s *raw() { return &raw_; }

private:
  rudp_stream_rx_s raw_;
};

} // namespace zcrudp

#endif /* ZCRUDP_HPP */
