#include "frame.h"
#include "crypto.h"
#include <string.h>

size_t frame_build(uint8_t *buf, size_t buf_len,
                   uint16_t cmd, uint16_t tid,
                   const uint8_t *ciphertext, size_t ct_len,
                   const uint8_t *tag) {
    size_t tag_len = tag ? 16 : 0;
    size_t total = HM_HEADER_LEN + ct_len + tag_len;
    if (buf_len < total) return 0;
    if (ct_len + HM_HEADER_LEN > 0xFFFF) return 0;

    uint16_t crc = (ct_len > 0) ? crc16_modbus(ciphertext, ct_len) : 0;
    uint16_t length = (uint16_t)(ct_len + HM_HEADER_LEN);

    buf[0] = HM_MAGIC_0;
    buf[1] = HM_MAGIC_1;
    buf[2] = (cmd >> 8) & 0xFF;
    buf[3] = cmd & 0xFF;
    buf[4] = (tid >> 8) & 0xFF;
    buf[5] = tid & 0xFF;
    buf[6] = (crc >> 8) & 0xFF;
    buf[7] = crc & 0xFF;
    buf[8] = (length >> 8) & 0xFF;
    buf[9] = length & 0xFF;

    if (ct_len > 0) memcpy(buf + HM_HEADER_LEN, ciphertext, ct_len);
    if (tag) memcpy(buf + HM_HEADER_LEN + ct_len, tag, 16);
    return total;
}

bool frame_parse_header(const uint8_t *buf, size_t buf_len,
                        uint16_t *cmd_out, uint16_t *tid_out,
                        uint16_t *crc_out, uint16_t *length_out) {
    if (buf_len < HM_HEADER_LEN) return false;
    if (buf[0] != HM_MAGIC_0 || buf[1] != HM_MAGIC_1) return false;
    *cmd_out    = ((uint16_t)buf[2] << 8) | buf[3];
    *tid_out    = ((uint16_t)buf[4] << 8) | buf[5];
    *crc_out    = ((uint16_t)buf[6] << 8) | buf[7];
    *length_out = ((uint16_t)buf[8] << 8) | buf[9];
    return true;
}

const uint8_t *frame_payload(const uint8_t *buf, size_t buf_len) {
    if (buf_len < HM_HEADER_LEN) return NULL;
    return buf + HM_HEADER_LEN;
}
