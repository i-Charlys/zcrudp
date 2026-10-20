#ifndef RUDP_CODEC_H
#define RUDP_CODEC_H

#include "rudp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Serializes a 4-byte datagram header into Big-Endian network format.
 *
 * @param header Pointer to the source datagram header.
 * @param out_buf Destination byte buffer.
 * @param max_len Maximum capacity of destination buffer (must be >=
 * RUDP_WIRE_DATAGRAM_HEADER_SIZE).
 * @return Number of bytes written (4 on success), or RUDP_ERR_INVALID_ARG on
 * error.
 */
int rudp_pack_datagram_header(const rudp_datagram_header_s *header, uint8_t *out_buf,
                              size_t max_len);

/**
 * @brief Deserializes a 4-byte datagram header from Big-Endian network format.
 *
 * @param in_buf Raw network bytes.
 * @param in_len Length of input buffer (must be >=
 * RUDP_WIRE_DATAGRAM_HEADER_SIZE).
 * @param out_header Pointer to destination datagram header.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_unpack_datagram_header(const uint8_t *in_buf, size_t in_len,
                                rudp_datagram_header_s *out_header);

/**
 * @brief Serializes an 8-byte message record into Big-Endian network format.
 *
 * @param record Pointer to the source record.
 * @param out_buf Destination byte buffer.
 * @param max_len Maximum capacity of destination buffer (must be >=
 * RUDP_WIRE_RECORD_SIZE).
 * @return Number of bytes written (8 on success), or RUDP_ERR_INVALID_ARG on
 * error.
 */
int rudp_pack_record(const rudp_record_s *record, uint8_t *out_buf, size_t max_len);

/**
 * @brief Deserializes an 8-byte message record from Big-Endian network format.
 *
 * @param in_buf Raw network bytes.
 * @param in_len Length of input buffer (must be >= RUDP_WIRE_RECORD_SIZE).
 * @param out_record Pointer to destination record.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_unpack_record(const uint8_t *in_buf, size_t in_len, rudp_record_s *out_record);

/**
 * @brief Deserializes and validates an entire bundled datagram (header + count
 * * records). Strictly checks exact length: in_len == 4 + count * 8.
 *
 * @param in_buf Raw network bytes.
 * @param in_len Length of input buffer.
 * @param out_header Pointer to destination datagram header.
 * @param out_records Pointer to array to receive unpacked records.
 * @param max_records Capacity of out_records array.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_unpack_datagram(const uint8_t *in_buf, size_t in_len,
                         rudp_datagram_header_s *out_header, rudp_record_s *out_records,
                         size_t max_records);

/**
 * @brief Serializes a 4-byte RUDP header into Big-Endian network format.
 *
 * @param header Pointer to the source header struct.
 * @param out_buf Destination byte buffer.
 * @param max_len Maximum capacity of destination buffer (must be >=
 * RUDP_WIRE_HEADER_SIZE).
 * @return Number of bytes written (4 on success), or RUDP_ERR_INVALID_ARG on
 * error.
 */
int rudp_pack_header(const rudp_header_s *header, uint8_t *out_buf, size_t max_len);

/**
 * @brief Serializes a 4-byte TFV packet payload into Big-Endian network format.
 *
 * @param packet Pointer to the source TFV packet.
 * @param out_buf Destination byte buffer.
 * @param max_len Maximum capacity of destination buffer (must be >=
 * sizeof(tfv_packet_u)).
 * @return Number of bytes written (4 on success), or RUDP_ERR_INVALID_ARG on
 * error.
 */
int rudp_pack_payload(const tfv_packet_u *packet, uint8_t *out_buf, size_t max_len);

/**
 * @brief Serializes a RUDP frame (Tier 2: 8 bytes) into a network-ready
 * Big-Endian byte buffer.
 *
 * @param frame Pointer to the source frame.
 * @param out_buf Destination byte buffer.
 * @param max_len Maximum writable capacity of out_buf (must be >=
 * RUDP_WIRE_FRAME_SIZE).
 * @return Number of bytes written (8 on success), or RUDP_ERR_INVALID_ARG on
 * error.
 */
int rudp_pack_frame(const rudp_frame_s *frame, uint8_t *out_buf, size_t max_len);

/**
 * @brief Serializes a standalone cumulative ACK (Tier 1: 4 bytes) into network
 * Big-Endian format.
 *
 * @param ack_num The cumulative sequence number being acknowledged.
 * @param out_buf Destination byte buffer.
 * @param max_len Maximum writable capacity of out_buf (must be >=
 * RUDP_WIRE_HEADER_SIZE).
 * @return Number of bytes written (4 on success), or RUDP_ERR_INVALID_ARG on
 * error.
 */
int rudp_pack_ack(uint16_t ack_num, uint8_t *out_buf, size_t max_len);

/**
 * @brief Decodes a 4-byte RUDP header from network Big-Endian format.
 *
 * @param in_buf Raw network bytes.
 * @param in_len Length of input buffer (must be >= RUDP_WIRE_HEADER_SIZE).
 * @param out_header Pointer to store the decoded header.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_unpack_header(const uint8_t *in_buf, size_t in_len, rudp_header_s *out_header);

/**
 * @brief Decodes a 4-byte TFV packet payload from Big-Endian network format.
 *
 * @param in_buf Raw network bytes.
 * @param in_len Length of input buffer (must be >= sizeof(tfv_packet_u)).
 * @param out_packet Pointer to store the extracted TFV packet.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_unpack_payload(const uint8_t *in_buf, size_t in_len, tfv_packet_u *out_packet);

/**
 * @brief Decodes a standalone ACK (Tier 1: 4 bytes) from network format.
 *
 * @param in_buf Raw network bytes.
 * @param in_len Length of input buffer (must be >= RUDP_WIRE_HEADER_SIZE).
 * @param out_ack Pointer to store the extracted ACK number.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_unpack_ack(const uint8_t *in_buf, size_t in_len, uint16_t *out_ack);

/**
 * @brief Deserializes a network byte buffer into a full RUDP frame struct (Tier
 * 2: 8 bytes).
 *
 * @param in_buf Source byte buffer received from network.
 * @param in_len Number of bytes received (must be >= RUDP_WIRE_FRAME_SIZE).
 * @param out_frame Destination frame pointer.
 * @return RUDP_OK on success, or RUDP_ERR_INVALID_ARG on error.
 */
int rudp_unpack_frame(const uint8_t *in_buf, size_t in_len, rudp_frame_s *out_frame);

#ifdef __cplusplus
}
#endif

#endif /* RUDP_CODEC_H */
