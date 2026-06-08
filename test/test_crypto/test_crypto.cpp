#include <unity.h>
#include <string.h>
#include <stdio.h>
#include "../../src/crypto.h"

static void hex_to_bytes(const char *hex, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned int b;
        sscanf(hex + 2 * i, "%02x", &b);
        out[i] = (uint8_t)b;
    }
}

void test_triple_sha256(void) {
    uint8_t input[16];
    hex_to_bytes("000102030405060708090a0b0c0d0e0f", input, 16);
    uint8_t out[32];
    triple_sha256(input, 16, out);
    uint8_t expected[32];
    hex_to_bytes("e5477bfc94b8a12bb7655873908fd2c2e9135187168737fede367a81674c4a2e", expected, 32);
    TEST_ASSERT_EQUAL_MEMORY(expected, out, 32);
}

void test_crc16_modbus(void) {
    uint8_t data1[] = {0x01, 0x02, 0x03, 0x04};
    TEST_ASSERT_EQUAL_HEX16(0x2BA1, crc16_modbus(data1, 4));

    uint8_t data2[] = {0xDE, 0xAD, 0xBE, 0xEF};
    TEST_ASSERT_EQUAL_HEX16(0xC19B, crc16_modbus(data2, 4));
}

void test_v0_derive_key(void) {
    uint8_t key[16];
    v0_derive_key("AABBCCDDEE12", key);
    uint8_t expected[16];
    hex_to_bytes("4c3f5d3bbf452b9fe337146ae214b0ee", expected, 16);
    TEST_ASSERT_EQUAL_MEMORY(expected, key, 16);
}

void test_v0_derive_iv(void) {
    uint8_t iv[16];
    v0_derive_iv(0xA201, 1, "AABBCCDDEE12", iv);
    uint8_t expected[16];
    hex_to_bytes("31e25c9a5aaf687e3767a3551fd58395", expected, 16);
    TEST_ASSERT_EQUAL_MEMORY(expected, iv, 16);
}

void test_v0_encrypt_decrypt_roundtrip(void) {
    uint8_t key[16];
    uint8_t iv[16];
    hex_to_bytes("4c3f5d3bbf452b9fe337146ae214b0ee", key, 16);
    hex_to_bytes("31e25c9a5aaf687e3767a3551fd58395", iv, 16);

    uint8_t plaintext[] = "test plaintext!!";  // 16 bytes
    uint8_t ct[32];
    size_t ct_len = v0_encrypt(plaintext, 16, key, iv, ct);
    TEST_ASSERT_EQUAL(32, ct_len);  // PKCS7: 16 bytes + 16-byte padding block

    uint8_t pt_out[32];
    size_t pt_len = v0_decrypt(ct, ct_len, key, iv, pt_out);
    TEST_ASSERT_EQUAL(16, pt_len);
    TEST_ASSERT_EQUAL_MEMORY(plaintext, pt_out, 16);
}

void test_v1_derive_key(void) {
    uint8_t enc_rand[16];
    hex_to_bytes("000102030405060708090a0b0c0d0e0f", enc_rand, 16);
    uint8_t key[16];
    v1_derive_key(enc_rand, key);
    uint8_t expected[16];
    hex_to_bytes("e5477bfc94b8a12bb7655873908fd2c2", expected, 16);
    TEST_ASSERT_EQUAL_MEMORY(expected, key, 16);
}

void test_v1_derive_nonce(void) {
    uint8_t enc_rand[16];
    hex_to_bytes("000102030405060708090a0b0c0d0e0f", enc_rand, 16);
    uint8_t nonce[12];
    v1_derive_nonce(0xA311, 1, enc_rand, nonce);
    uint8_t expected[12];
    hex_to_bytes("2a5cb5c478c37530871767bc", expected, 12);
    TEST_ASSERT_EQUAL_MEMORY(expected, nonce, 12);
}

void test_v1_encrypt_decrypt_roundtrip(void) {
    uint8_t enc_rand[16];
    hex_to_bytes("000102030405060708090a0b0c0d0e0f", enc_rand, 16);
    uint8_t key[16];
    uint8_t nonce[12];
    v1_derive_key(enc_rand, key);
    v1_derive_nonce(0xA311, 1, enc_rand, nonce);
    uint8_t aad[] = {0x11, 0xa3, 0x01, 0x00};  // LE_u16(0xA311) + LE_u16(1)

    uint8_t plaintext[] = "hello world!1234";  // 16 bytes
    uint8_t ct[16];
    uint8_t tag[16];
    bool ok = v1_encrypt(plaintext, 16, key, nonce, aad, 4, ct, tag);
    TEST_ASSERT_TRUE(ok);

    uint8_t pt_out[16];
    ok = v1_decrypt(ct, 16, key, nonce, aad, 4, tag, pt_out);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_MEMORY(plaintext, pt_out, 16);
}

void test_v1_decrypt_bad_tag_fails(void) {
    uint8_t enc_rand[16];
    hex_to_bytes("000102030405060708090a0b0c0d0e0f", enc_rand, 16);
    uint8_t key[16];
    uint8_t nonce[12];
    v1_derive_key(enc_rand, key);
    v1_derive_nonce(0xA311, 1, enc_rand, nonce);
    uint8_t aad[] = {0x11, 0xa3, 0x01, 0x00};

    uint8_t plaintext[] = "hello world!1234";
    uint8_t ct[16];
    uint8_t tag[16];
    v1_encrypt(plaintext, 16, key, nonce, aad, 4, ct, tag);
    tag[0] ^= 0xFF;  // corrupt tag

    uint8_t pt_out[16];
    bool ok = v1_decrypt(ct, 16, key, nonce, aad, 4, tag, pt_out);
    TEST_ASSERT_FALSE(ok);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_triple_sha256);
    RUN_TEST(test_crc16_modbus);
    RUN_TEST(test_v0_derive_key);
    RUN_TEST(test_v0_derive_iv);
    RUN_TEST(test_v0_encrypt_decrypt_roundtrip);
    RUN_TEST(test_v1_derive_key);
    RUN_TEST(test_v1_derive_nonce);
    RUN_TEST(test_v1_encrypt_decrypt_roundtrip);
    RUN_TEST(test_v1_decrypt_bad_tag_fails);
    return UNITY_END();
}
