#include "crypto.h"
#include <string.h>

#ifdef NATIVE_TEST
#include <mbedtls/sha256.h>
#include <mbedtls/aes.h>
#include <mbedtls/gcm.h>
#else
#include "mbedtls/sha256.h"
#include "mbedtls/aes.h"
#include "mbedtls/gcm.h"
#endif

void triple_sha256(const uint8_t *in, size_t in_len, uint8_t out[32]) {
    uint8_t tmp[32];
    mbedtls_sha256_context ctx;

    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, in, in_len);
    mbedtls_sha256_finish(&ctx, tmp);

    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, tmp, 32);
    mbedtls_sha256_finish(&ctx, tmp);

    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, tmp, 32);
    mbedtls_sha256_finish(&ctx, out);

    mbedtls_sha256_free(&ctx);
}

uint16_t crc16_modbus(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }
    return crc;
}

void v0_derive_key(const char *sn, uint8_t key_out[16]) {
    static const uint8_t salt[] = "Hoymiles@#123456";
    size_t sn_len = strlen(sn);
    uint8_t buf[64];
    memcpy(buf, sn, sn_len);
    memcpy(buf + sn_len, salt, 16);
    uint8_t digest[32];
    triple_sha256(buf, sn_len + 16, digest);
    memcpy(key_out, digest, 16);
}

void v0_derive_iv(uint16_t cmd, uint16_t tid, const char *sn, uint8_t iv_out[16]) {
    size_t sn_len = strlen(sn);
    uint8_t buf[4 + 64];
    buf[0] = (cmd >> 8) & 0xFF;
    buf[1] = cmd & 0xFF;
    buf[2] = (tid >> 8) & 0xFF;
    buf[3] = tid & 0xFF;
    memcpy(buf + 4, sn, sn_len);
    uint8_t digest[32];
    triple_sha256(buf, 4 + sn_len, digest);
    memcpy(iv_out, digest + 16, 16);
}

size_t v0_encrypt(const uint8_t *pt, size_t pt_len,
                  const uint8_t key[16], const uint8_t iv[16],
                  uint8_t *ct_out) {
    size_t pad_len = 16 - (pt_len % 16);
    size_t padded_len = pt_len + pad_len;
    uint8_t padded[512];
    if (padded_len > sizeof(padded)) return 0;
    memcpy(padded, pt, pt_len);
    memset(padded + pt_len, (uint8_t)pad_len, pad_len);

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);
    mbedtls_aes_setkey_enc(&ctx, key, 128);
    mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, padded_len, iv_copy, padded, ct_out);
    mbedtls_aes_free(&ctx);
    return padded_len;
}

size_t v0_decrypt(const uint8_t *ct, size_t ct_len,
                  const uint8_t key[16], const uint8_t iv[16],
                  uint8_t *pt_out) {
    if (ct_len == 0 || ct_len % 16 != 0) return 0;
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);
    mbedtls_aes_setkey_dec(&ctx, key, 128);
    mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, ct_len, iv_copy, ct, pt_out);
    mbedtls_aes_free(&ctx);
    uint8_t pad = pt_out[ct_len - 1];
    if (pad == 0 || pad > 16) return 0;
    return ct_len - pad;
}

void v1_derive_key(const uint8_t enc_rand[16], uint8_t key_out[16]) {
    uint8_t digest[32];
    triple_sha256(enc_rand, 16, digest);
    memcpy(key_out, digest, 16);
}

void v1_derive_nonce(uint16_t cmd, uint16_t tid,
                     const uint8_t enc_rand[16], uint8_t nonce_out[12]) {
    uint8_t buf[20];
    buf[0] = cmd & 0xFF;
    buf[1] = (cmd >> 8) & 0xFF;
    buf[2] = tid & 0xFF;
    buf[3] = (tid >> 8) & 0xFF;
    memcpy(buf + 4, enc_rand, 16);
    uint8_t digest[32];
    triple_sha256(buf, 20, digest);
    memcpy(nonce_out, digest + 20, 12);
}

bool v1_encrypt(const uint8_t *pt, size_t pt_len,
                const uint8_t key[16], const uint8_t nonce[12],
                const uint8_t *aad, size_t aad_len,
                uint8_t *ct_out, uint8_t tag_out[16]) {
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    int ret = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (ret != 0) { mbedtls_gcm_free(&ctx); return false; }
    ret = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT,
                                     pt_len, nonce, 12, aad, aad_len,
                                     pt, ct_out, 16, tag_out);
    mbedtls_gcm_free(&ctx);
    return ret == 0;
}

bool v1_decrypt(const uint8_t *ct, size_t ct_len,
                const uint8_t key[16], const uint8_t nonce[12],
                const uint8_t *aad, size_t aad_len,
                const uint8_t tag[16], uint8_t *pt_out) {
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    int ret = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (ret != 0) { mbedtls_gcm_free(&ctx); return false; }
    ret = mbedtls_gcm_auth_decrypt(&ctx, ct_len, nonce, 12, aad, aad_len,
                                    tag, 16, ct, pt_out);
    mbedtls_gcm_free(&ctx);
    return ret == 0;
}
