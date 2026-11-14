#include "protocol_rudp.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int rudp_fuzz_one_input(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check_session_invariants(const rudp_session_s *s) {
  for (uint8_t c = 0; c < RUDP_MAX_CHANNELS; ++c) {
    const rudp_channel_s *ch = &s->channels[c];
    assert(ch->ctx.head < RUDP_WINDOW_SIZE);
    assert(ch->ctx.tail < RUDP_WINDOW_SIZE);
    uint16_t cur = ch->ctx.tail;
    size_t steps = 0U;
    while (cur != ch->ctx.head) {
      assert(steps < RUDP_WINDOW_SIZE);
      uint8_t st = ch->ctx.tx_buffer[cur].state;
      assert(st == RUDP_SLOT_FREE || st == RUDP_SLOT_IN_FLIGHT ||
             st == RUDP_SLOT_SACKED || st == RUDP_SLOT_UNRELIABLE);
      cur = (uint16_t)((cur + 1U) & (RUDP_WINDOW_SIZE - 1U));
      steps++;
    }
  }
}

int rudp_fuzz_one_input(const uint8_t *data, size_t size) {
  if (!data || size == 0U) {
    rudp_session_s empty;
    assert(rudp_session_init(&empty) == RUDP_OK);
    rudp_record_s out[4];
    rudp_stats_s stats = {0};
    rudp_session_s snap = empty;
    int ret = rudp_session_process_datagram_ex(&empty, data, size, out, 4, false, 0U,
                                               &stats);
    assert(ret < 0);
    assert(memcmp(&empty, &snap, sizeof(empty)) == 0);
    return 0;
  }

  /* 1. Raw wire codec & Pass-1 RX atomicity on pre-seeded multi-channel session */
  {
    rudp_session_s s;
    assert(rudp_session_init(&s) == RUDP_OK);
    assert(rudp_session_config_channel(
               &s, 1,
               RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED |
                   RUDP_CHANNEL_FLAG_FEC_K2) == RUDP_OK);
    assert(rudp_session_config_channel(&s, 2, RUDP_CHANNEL_FLAG_UNRELIABLE) ==
           RUDP_OK);
    assert(rudp_session_config_channel(&s, 3, RUDP_CHANNEL_FLAG_RELIABLE) == RUDP_OK);
    assert(rudp_session_config_recovery(&s, 0, 100U, 10U, 1000U) == RUDP_OK);
    assert(rudp_session_config_recovery(&s, 1, 100U, 10U, 1000U) == RUDP_OK);

    /* Seed reliable + unreliable TX slots so ACK/SACK fuzz inputs exercise holes */
    tfv_packet_u p = {.type = 1U, .flags = 0U, .value = 77U};
    assert(rudp_session_send_reliable(&s, 0, p, 10U) == RUDP_OK);
    assert(rudp_session_queue_unreliable(&s, 0, p) == RUDP_OK);
    assert(rudp_session_send_reliable(&s, 0, p, 10U) == RUDP_OK);
    assert(rudp_session_send_reliable(&s, 1, p, 10U) == RUDP_OK);
    assert(rudp_session_send_reliable(&s, 1, p, 10U) == RUDP_OK);
    uint8_t wire[1404U];
    int n = rudp_session_build_datagram(&s, 0, wire, sizeof(wire), 10U, 100U);
    assert(n > RUDP_WIRE_DATAGRAM_HEADER_SIZE);

    rudp_session_s snap = s;
    rudp_record_s delivered[64];
    rudp_stats_s stats = {0};
    int rx = rudp_session_process_datagram_ex(&s, data, size, delivered, 64, true,
                                              50U, &stats);
    if (rx < 0) {
      assert(memcmp(&s, &snap, sizeof(s)) == 0);
    }
    check_session_invariants(&s);

    rudp_datagram_header_s hdr;
    int hret = rudp_unpack_datagram_header(data, size, &hdr);
    if (hret == RUDP_OK) {
      uint8_t hbuf[RUDP_WIRE_DATAGRAM_HEADER_SIZE];
      int pret = rudp_pack_datagram_header(&hdr, hbuf, sizeof(hbuf));
      if (hdr.ack_channel < RUDP_MAX_CHANNELS) {
        assert(pret == RUDP_WIRE_DATAGRAM_HEADER_SIZE);
        assert(memcmp(hbuf, data, sizeof(hbuf)) == 0);
      } else {
        assert(pret == RUDP_ERR_INVALID_ARG);
      }
    }
    rudp_record_s rec;
    if (rudp_unpack_record(data, size, &rec) == RUDP_OK) {
      uint8_t rbuf[RUDP_WIRE_RECORD_SIZE];
      int pret = rudp_pack_record(&rec, rbuf, sizeof(rbuf));
      if (rec.channel_id < RUDP_MAX_CHANNELS) {
        assert(pret == RUDP_WIRE_RECORD_SIZE);
        assert(memcmp(rbuf, data, sizeof(rbuf)) == 0);
      } else {
        assert(pret == RUDP_ERR_INVALID_ARG);
      }
    }
  }

  /* 2. Stateful two-peer TX/RX + prepare/cancel + ChaCha20-Poly1305 AEAD fuzz loop */
  {
    static const uint8_t key[RUDP_CRYPTO_KEY_BYTES] = {
        0x10, 0x20, 0x30, 0x40, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
        0xcc, 0xdd, 0xee, 0xff, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x11, 0x22};
    rudp_session_s a, b;
    assert(rudp_session_init(&a) == RUDP_OK);
    assert(rudp_session_init(&b) == RUDP_OK);
    assert(rudp_session_config_channel(
               &a, 1,
               RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED |
                   RUDP_CHANNEL_FLAG_FEC_K2) == RUDP_OK);
    assert(rudp_session_config_channel(
               &b, 1,
               RUDP_CHANNEL_FLAG_RELIABLE | RUDP_CHANNEL_FLAG_ORDERED |
                   RUDP_CHANNEL_FLAG_FEC_K2) == RUDP_OK);
    assert(rudp_session_config_channel(&a, 2, RUDP_CHANNEL_FLAG_RELIABLE) == RUDP_OK);
    assert(rudp_session_config_channel(&b, 2, RUDP_CHANNEL_FLAG_RELIABLE) == RUDP_OK);
    assert(rudp_session_config_recovery(&a, 0, 80U, 10U, 800U) == RUDP_OK);
    assert(rudp_session_config_recovery(&b, 0, 80U, 10U, 800U) == RUDP_OK);

    rudp_crypto_ctx_s ca, cb;
    assert(rudp_crypto_init(&ca, key, 0xCAFEBABE12345678ULL, 1U) == RUDP_OK);
    assert(rudp_crypto_init(&cb, key, 0xCAFEBABE12345678ULL, 0U) == RUDP_OK);

    uint32_t now = 100U;
    size_t steps = size < 48U ? size : 48U;
    for (size_t i = 0; i < steps; ++i) {
      uint8_t op = data[i];
      uint8_t ch = (uint8_t)(op % 3U);
      now += (uint32_t)((op >> 2) & 0x1FU);
      tfv_packet_u pkt = {.type = (uint8_t)(op & 0x3FU),
                          .flags = (uint8_t)(op >> 6),
                          .value = (uint16_t)(i * 131U + op)};

      switch ((op >> 4) % 6U) {
      case 0:
        (void)rudp_session_send_reliable(&a, ch, pkt, now);
        break;
      case 1:
        (void)rudp_session_queue_unreliable(&a, ch, pkt);
        break;
      case 2: {
        uint8_t buf[1404U];
        uint8_t sealed[1404U + RUDP_CRYPTO_OVERHEAD_BYTES];
        uint8_t plain[1404U];
        rudp_tx_plan_s plan;
        rudp_session_s a_snap = a;
        int blen =
            rudp_session_prepare_datagram(&a, now, 100U, buf, sizeof(buf), &plan);
        if (blen > 0) {
          if (op & 1U) {
            assert(rudp_session_cancel_datagram(&a, &plan) == RUDP_OK);
            assert(memcmp(&a, &a_snap, sizeof(a)) == 0);
          } else {
            assert(rudp_session_commit_datagram(&a, &plan) == RUDP_OK);
            int slen = rudp_crypto_seal(&ca, NULL, 0U, buf, (size_t)blen, sealed,
                                        sizeof(sealed));
            assert(slen == blen + (int)RUDP_CRYPTO_OVERHEAD_BYTES);
            if ((op & 2U) && slen > 8) {
              /* Flip a byte and verify AEAD rejects without mutating cb */
              rudp_crypto_ctx_s cb_snap = cb;
              sealed[(size_t)op % (size_t)slen] ^= 0x5AU;
              assert(rudp_crypto_open(&cb, NULL, 0U, sealed, (size_t)slen, plain,
                                      sizeof(plain)) == RUDP_ERR_AUTH);
              assert(memcmp(&cb, &cb_snap, sizeof(cb)) == 0);
            } else {
              int olen = rudp_crypto_open(&cb, NULL, 0U, sealed, (size_t)slen, plain,
                                          sizeof(plain));
              assert(olen == blen);
              rudp_record_s del[64];
              int d = rudp_session_process_datagram_at(&b, plain, (size_t)olen, del, 64,
                                                       now + 5U);
              assert(d >= 0);
            }
          }
        }
        break;
      }
      case 3: {
        uint8_t buf[1404U];
        int blen = rudp_session_build_datagram(&b, ch, buf, sizeof(buf), now, 100U);
        if (blen > 0) {
          if ((op & 4U) && blen > 4) {
            size_t flip_idx = (size_t)(op % (uint8_t)blen);
            buf[flip_idx] ^= 0xFFU;
          }
          rudp_session_s a_snap = a;
          rudp_record_s del[64];
          rudp_stats_s st = {0};
          int rx = rudp_session_process_datagram_ex(&a, buf, (size_t)blen, del, 64,
                                                    true, now + 5U, &st);
          if (rx < 0) {
            assert(memcmp(&a, &a_snap, sizeof(a)) == 0);
          }
        }
        break;
      }
      case 4: {
        rudp_record_s del[16];
        int p = rudp_session_poll(&b, del, 16);
        assert(p >= 0);
        break;
      }
      default: {
        uint8_t small_buf[16];
        rudp_record_s del[4];
        rudp_session_s b_snap = b;
        size_t take = size - i;
        if (take > sizeof(small_buf)) {
          take = sizeof(small_buf);
        }
        memcpy(small_buf, data + i, take);
        int rx = rudp_session_process_datagram_at(&b, small_buf, take, del, 4, now);
        if (rx < 0) {
          assert(memcmp(&b, &b_snap, sizeof(b)) == 0);
        }
        break;
      }
      }
      check_session_invariants(&a);
      check_session_invariants(&b);
    }
    rudp_crypto_wipe(&ca);
    rudp_crypto_wipe(&cb);
  }
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  return rudp_fuzz_one_input(data, size);
}
