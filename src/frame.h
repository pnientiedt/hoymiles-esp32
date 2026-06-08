#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define HM_MAGIC_0  0x48  // 'H'
#define HM_MAGIC_1  0x4D  // 'M'
#define HM_HEADER_LEN 10

#ifdef __cplusplus
extern "C" {
#endif

// Builds a complete wire frame into buf.
// ciphertext: encrypted payload (NOT including GCM tag)
// tag: 16-byte GCM auth tag, or NULL for V0 (no tag)
// Returns total frame length, or 0 if buf_len is too small.
size_t frame_build(uint8_t *buf, size_t buf_len,
                   uint16_t cmd, uint16_t tid,
                   const uint8_t *ciphertext, size_t ct_len,
                   const uint8_t *tag);

// Parses the fixed 10-byte header.
// Returns false if magic is wrong or buf_len < 10.
bool frame_parse_header(const uint8_t *buf, size_t buf_len,
                        uint16_t *cmd_out, uint16_t *tid_out,
                        uint16_t *crc_out, uint16_t *length_out);

// Returns pointer to the ciphertext payload (buf + 10).
// Returns NULL if buf_len < HM_HEADER_LEN.
const uint8_t *frame_payload(const uint8_t *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
