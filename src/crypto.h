#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void triple_sha256(const uint8_t *in, size_t in_len, uint8_t out[32]);
uint16_t crc16_modbus(const uint8_t *data, size_t len);
void v0_derive_key(const char *sn, uint8_t key_out[16]);
void v0_derive_iv(uint16_t cmd, uint16_t tid, const char *sn, uint8_t iv_out[16]);
size_t v0_encrypt(const uint8_t *pt, size_t pt_len,
                  const uint8_t key[16], const uint8_t iv[16],
                  uint8_t *ct_out);
size_t v0_decrypt(const uint8_t *ct, size_t ct_len,
                  const uint8_t key[16], const uint8_t iv[16],
                  uint8_t *pt_out);
void v1_derive_key(const uint8_t enc_rand[16], uint8_t key_out[16]);
void v1_derive_nonce(uint16_t cmd, uint16_t tid,
                     const uint8_t enc_rand[16], uint8_t nonce_out[12]);
bool v1_encrypt(const uint8_t *pt, size_t pt_len,
                const uint8_t key[16], const uint8_t nonce[12],
                const uint8_t *aad, size_t aad_len,
                uint8_t *ct_out, uint8_t tag_out[16]);
bool v1_decrypt(const uint8_t *ct, size_t ct_len,
                const uint8_t key[16], const uint8_t nonce[12],
                const uint8_t *aad, size_t aad_len,
                const uint8_t tag[16], uint8_t *pt_out);

#ifdef __cplusplus
}
#endif
