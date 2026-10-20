#include "rudp_codec.h"
#include <string.h>

int rudp_pack_datagram_header(const rudp_datagram_header_s *header, uint8_t *out_buf,
                              size_t max_len) {
  if (!header || !out_buf || max_len < RUDP_WIRE_DATAGRAM_HEADER_SIZE) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (header->ack_channel >= RUDP_MAX_CHANNELS) {
    return RUDP_ERR_INVALID_ARG;
  }

  out_buf[0] = (uint8_t)(header->ack >> 8);
  out_buf[1] = (uint8_t)(header->ack & 0xFF);
  out_buf[2] = header->ack_channel;
  out_buf[3] = header->count;

  return RUDP_WIRE_DATAGRAM_HEADER_SIZE;
}

int rudp_unpack_datagram_header(const uint8_t *in_buf, size_t in_len,
                                rudp_datagram_header_s *out_header) {
  if (!in_buf || !out_header || in_len < RUDP_WIRE_DATAGRAM_HEADER_SIZE) {
    return RUDP_ERR_INVALID_ARG;
  }

  *out_header = (rudp_datagram_header_s){
      .ack = ((uint16_t)in_buf[0] << 8) | in_buf[1],
      .ack_channel = in_buf[2],
      .count = in_buf[3],
  };

  return RUDP_OK;
}

int rudp_pack_record(const rudp_record_s *record, uint8_t *out_buf, size_t max_len) {
  if (!record || !out_buf || max_len < RUDP_WIRE_RECORD_SIZE) {
    return RUDP_ERR_INVALID_ARG;
  }
  if (record->channel_id >= RUDP_MAX_CHANNELS) {
    return RUDP_ERR_INVALID_ARG;
  }

  out_buf[0] = record->channel_id;
  out_buf[1] = record->flags;
  out_buf[2] = (uint8_t)(record->seq_num >> 8);
  out_buf[3] = (uint8_t)(record->seq_num & 0xFF);

  out_buf[4] = record->payload.type;
  out_buf[5] = record->payload.flags;
  out_buf[6] = (uint8_t)(record->payload.value >> 8);
  out_buf[7] = (uint8_t)(record->payload.value & 0xFF);

  return RUDP_WIRE_RECORD_SIZE;
}

int rudp_unpack_record(const uint8_t *in_buf, size_t in_len,
                       rudp_record_s *out_record) {
  if (!in_buf || !out_record || in_len < RUDP_WIRE_RECORD_SIZE) {
    return RUDP_ERR_INVALID_ARG;
  }

  *out_record = (rudp_record_s){
      .channel_id = in_buf[0],
      .flags = in_buf[1],
      .seq_num = ((uint16_t)in_buf[2] << 8) | in_buf[3],
      .payload =
          {
              .type = in_buf[4],
              .flags = in_buf[5],
              .value = ((uint16_t)in_buf[6] << 8) | in_buf[7],
          },
  };

  return RUDP_OK;
}

int rudp_unpack_datagram(const uint8_t *in_buf, size_t in_len,
                         rudp_datagram_header_s *out_header, rudp_record_s *out_records,
                         size_t max_records) {
  if (!in_buf || !out_header || in_len < RUDP_WIRE_DATAGRAM_HEADER_SIZE) {
    return RUDP_ERR_INVALID_ARG;
  }

  int ret = rudp_unpack_datagram_header(in_buf, in_len, out_header);
  if (ret != RUDP_OK) {
    return ret;
  }

  size_t expected_len = RUDP_WIRE_DATAGRAM_HEADER_SIZE +
                        ((size_t)out_header->count * RUDP_WIRE_RECORD_SIZE);
  if (in_len != expected_len) {
    return RUDP_ERR_INVALID_ARG;
  }

  if (out_header->count > 0) {
    if (!out_records || max_records < (size_t)out_header->count) {
      return RUDP_ERR_INVALID_ARG;
    }

    for (uint8_t i = 0; i < out_header->count; i++) {
      size_t offset =
          RUDP_WIRE_DATAGRAM_HEADER_SIZE + ((size_t)i * RUDP_WIRE_RECORD_SIZE);
      ret = rudp_unpack_record(in_buf + offset, in_len - offset, &out_records[i]);
      if (ret != RUDP_OK) {
        return ret;
      }
    }
  }

  return RUDP_OK;
}

int rudp_pack_frame(const rudp_frame_s *frame, uint8_t *out_buf, size_t max_len) {
  if (!frame || !out_buf || max_len < RUDP_WIRE_FRAME_SIZE) {
    return RUDP_ERR_INVALID_ARG;
  }

  // 1. Pack header (bytes 0..3)
  rudp_pack_header(&frame->header, out_buf, max_len);

  // 2. Pack payload (bytes 4..7)
  rudp_pack_payload(&frame->packet, out_buf + RUDP_WIRE_HEADER_SIZE,
                    max_len - RUDP_WIRE_HEADER_SIZE);

  return RUDP_WIRE_FRAME_SIZE; // Success: 8 bytes written
}

int rudp_unpack_frame(const uint8_t *in_buf, size_t in_len, rudp_frame_s *out_frame) {
  if (!in_buf || !out_frame || in_len < RUDP_WIRE_FRAME_SIZE) {
    return RUDP_ERR_INVALID_ARG;
  }

  // 1. Unpack header via modular header decoder
  rudp_unpack_header(in_buf, in_len, &out_frame->header);

  // 2. Unpack payload via modular payload decoder
  rudp_unpack_payload(in_buf + RUDP_WIRE_HEADER_SIZE, in_len - RUDP_WIRE_HEADER_SIZE,
                      &out_frame->packet);

  return RUDP_OK;
}

int rudp_pack_header(const rudp_header_s *header, uint8_t *out_buf, size_t max_len) {
  if (!header || !out_buf || max_len < RUDP_WIRE_HEADER_SIZE) {
    return RUDP_ERR_INVALID_ARG;
  }

  out_buf[0] = (uint8_t)(header->seq_num >> 8);
  out_buf[1] = (uint8_t)(header->seq_num & 0xFF);
  out_buf[2] = (uint8_t)(header->ack >> 8);
  out_buf[3] = (uint8_t)(header->ack & 0xFF);

  return RUDP_WIRE_HEADER_SIZE;
}

int rudp_unpack_header(const uint8_t *in_buf, size_t in_len,
                       rudp_header_s *out_header) {
  if (!in_buf || !out_header || in_len < RUDP_WIRE_HEADER_SIZE) {
    return RUDP_ERR_INVALID_ARG;
  }

  *out_header = (rudp_header_s){
      .seq_num = ((uint16_t)in_buf[0] << 8) | in_buf[1],
      .ack = ((uint16_t)in_buf[2] << 8) | in_buf[3],
  };

  return RUDP_OK;
}

int rudp_pack_payload(const tfv_packet_u *packet, uint8_t *out_buf, size_t max_len) {
  if (!packet || !out_buf || max_len < sizeof(tfv_packet_u)) {
    return RUDP_ERR_INVALID_ARG;
  }

  out_buf[0] = packet->type;
  out_buf[1] = packet->flags;
  out_buf[2] = (uint8_t)(packet->value >> 8);
  out_buf[3] = (uint8_t)(packet->value & 0xFF);

  return (int)sizeof(tfv_packet_u);
}

int rudp_unpack_payload(const uint8_t *in_buf, size_t in_len,
                        tfv_packet_u *out_packet) {
  if (!in_buf || !out_packet || in_len < sizeof(tfv_packet_u)) {
    return RUDP_ERR_INVALID_ARG;
  }

  *out_packet = (tfv_packet_u){
      .type = in_buf[0],
      .flags = in_buf[1],
      .value = ((uint16_t)in_buf[2] << 8) | in_buf[3],
  };

  return RUDP_OK;
}

int rudp_pack_ack(uint16_t ack_num, uint8_t *out_buf, size_t max_len) {
  if (!out_buf || max_len < RUDP_WIRE_HEADER_SIZE) {
    return RUDP_ERR_INVALID_ARG;
  }

  rudp_header_s header = {
      .seq_num = 0, /* Unused for standalone cumulative ACK. */
      .ack = ack_num,
  };

  return rudp_pack_header(&header, out_buf, max_len);
}

int rudp_unpack_ack(const uint8_t *in_buf, size_t in_len, uint16_t *out_ack) {
  if (!in_buf || !out_ack) {
    return RUDP_ERR_INVALID_ARG;
  }

  rudp_header_s header;
  int res = rudp_unpack_header(in_buf, in_len, &header);
  if (res != RUDP_OK) {
    return res;
  }

  *out_ack = header.ack;
  return RUDP_OK;
}
