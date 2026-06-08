# Hoymiles ESP32 BLE→MQTT Bridge — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Firmware for an ESP32 that connects to a Hoymiles HMS-800-2WB via BLE, polls live inverter data every 30 seconds, and publishes individual MQTT topics over WiFi.

**Architecture:** PlatformIO + Arduino framework. Eight source modules with single responsibilities: `crypto` (key derivation + AES), `frame` (wire byte packing), `ble_client` (NimBLE GATT), `handshake` (V0 pairing + CommCmd), `poller` (RealDataNew + MQTT publish), and `main` (WiFi/MQTT reconnect state machine + watchdog). Pure-logic modules (`crypto`, `frame`) are tested with a PlatformIO native environment on the host machine; BLE/MQTT modules are verified by flashing to the real device.

**Tech Stack:** PlatformIO, Arduino framework (ESP32), NimBLE-Arduino, PubSubClient, nanopb, mbedTLS (built-in), Preferences (NVS flash).

---

## File Map

| File | Created/Modified | Responsibility |
|------|-----------------|----------------|
| `platformio.ini` | Create | Build config, lib deps, native test env |
| `src/config.h` | Create | WiFi creds, MQTT host/port, BLE device name, poll interval |
| `src/main.cpp` | Create | setup/loop: state machine (WiFi→MQTT→BLE→handshake→poll), watchdog |
| `src/crypto.h` | Create | Public API for triple_sha256, CRC16, V0/V1 key derivation + AES |
| `src/crypto.cpp` | Create | Implementation using mbedTLS |
| `src/frame.h` | Create | Public API for HM wire frame build + parse |
| `src/frame.cpp` | Create | Implementation, no crypto knowledge |
| `src/ble_client.h` | Create | BLE GATT client API: scan, connect, MTU, TX write, RX notify |
| `src/ble_client.cpp` | Create | NimBLE-Arduino implementation |
| `src/handshake.h` | Create | V0 pairing + CommCmd login/time-sync API |
| `src/handshake.cpp` | Create | Orchestrates crypto + frame + ble_client + Preferences (NVS) |
| `src/poller.h` | Create | Poll one cycle: request → paged response → decode → MQTT publish |
| `src/poller.cpp` | Create | Orchestrates frame + handshake (for crypto) + ble_client + PubSubClient |
| `proto/RealDataNew.proto` | Create | Protobuf schema (field numbers from hoymiles-wifi library) |
| `proto/CommCmd.proto` | Create | CommandReqDTO / CommandResDTO schema |
| `proto/APPInfomationData.proto` | Create | APPInfoDataReqDTO / APPDtuInfoMO schema |
| `src/proto/RealDataNew.pb.c` | Generate | nanopb-generated C decoder |
| `src/proto/RealDataNew.pb.h` | Generate | nanopb-generated C header |
| `src/proto/CommCmd.pb.c` | Generate | nanopb-generated C decoder |
| `src/proto/CommCmd.pb.h` | Generate | nanopb-generated C header |
| `src/proto/APPInfomationData.pb.c` | Generate | nanopb-generated C decoder |
| `src/proto/APPInfomationData.pb.h` | Generate | nanopb-generated C header |
| `test/test_crypto/test_crypto.cpp` | Create | Native unit tests for crypto module |
| `test/test_frame/test_frame.cpp` | Create | Native unit tests for frame module |

---

## Protocol Quick Reference

**Wire frame layout** (all multi-byte fields big-endian):
```
[0:2]   0x484D  "HM" magic
[2:4]   cmd     uint16 BE
[4:6]   tid     uint16 BE  (monotonic, wraps at 0xFFFF)
[6:8]   crc16   uint16 BE  (CRC16-Modbus of ciphertext only, no GCM tag)
[8:10]  length  uint16 BE  = len(ciphertext_no_tag) + 10
[10:N]  ciphertext
[N:16]  GCM tag             (V1 only — appended after ciphertext)
```

**CRC16-Modbus:** poly=0x8005, reflected input+output, init=0xFFFF, xorOut=0x0000.

**V0 AES-128-CBC** (used once for initial pairing to obtain encRand):
- Key: `triple_sha256(sn_bytes + b"Hoymiles@#123456")[:16]`
- IV:  `triple_sha256(BE_u16(cmd) + BE_u16(tid) + sn_bytes)[16:32]`
- `sn_bytes`: the 12-char serial tail from BLE advert name `RMI-XXXXXXXXXXXX`

**V1 AES-128-GCM** (all normal requests after pairing):
- Key:   `triple_sha256(enc_rand)[:16]`
- Nonce: `triple_sha256(LE_u16(cmd) + LE_u16(tid) + enc_rand)[20:32]`  (12 bytes)
- AAD:   `LE_u16(cmd) + LE_u16(tid)`

**Command codes (uint16, big-endian on wire):**
- `0xA201` APP_INFO request   (we send, V0 CBC)
- `0xA301` APP_INFO response  (inverter replies, V0 CBC, contains enc_rand)
- `0xA311` REAL_DATA_NEW      (same code for request and response, V1 GCM)
- `0xA305` COMMAND            (CommCmd: login action=64, time-sync action=104, V1 GCM)

**BLE GATT:**
- Service:  `0000e0ff-3c17-d293-8e48-14fe2e4da212`
- TX write: `0000ffe1-0000-1000-8000-00805f9b34fb`
- RX notify:`0000ffe2-0000-1000-8000-00805f9b34fb`
- MTU: negotiate 512; chunk TX writes to MTU-3 bytes each if needed

**Scale factors for MQTT values:**
- Voltage: raw ÷ 10  (V)
- Current: raw ÷ 100 (A)
- Power:   raw ÷ 10  (W)
- Frequency: raw ÷ 100 (Hz)
- Energy:  raw ÷ 1000 (kWh)
- Temperature: raw ÷ 10 (°C)
- Power factor: raw ÷ 100

---

## Task 1: PlatformIO scaffold

**Files:**
- Create: `platformio.ini`
- Create: `src/config.h`
- Create: `src/main.cpp` (empty skeleton)

- [ ] **Step 1: Install PlatformIO CLI** (skip if already installed)

```bash
pip install platformio
pio --version
```

Expected: `PlatformIO Core, version X.Y.Z`

- [ ] **Step 2: Create `platformio.ini`**

```ini
[env:esp32]
platform = espressif32
board = esp32dev
framework = arduino
lib_deps =
    h2zero/NimBLE-Arduino@^1.4.2
    knolleary/PubSubClient@^2.8
    nanopb/Nanopb@^0.4.8
monitor_speed = 115200
build_flags =
    -DCONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
    -DCONFIG_BT_NIMBLE_PINNED_TO_CORE=1

[env:native]
platform = native
test_framework = unity
build_flags =
    -std=c++17
    -DNATIVE_TEST
```

- [ ] **Step 3: Create `src/config.h`**

```cpp
#pragma once

#define WIFI_SSID       "your-ssid"
#define WIFI_PASSWORD   "your-password"
#define MQTT_HOST       "192.168.1.50"
#define MQTT_PORT       1883
#define MQTT_CLIENT_ID  "hoymiles-esp32"

// BLE advertisement name prefix (from inverter: "RMI-XXXXXXXXXXXX")
// Set to the full name of your inverter.
#define BLE_DEVICE_NAME "RMI-AABBCCDDEE12"

// 12-char serial tail: everything after "RMI-"
#define INVERTER_SN     "AABBCCDDEE12"

// MQTT base topic: hoymiles/<sn>/
#define MQTT_BASE_TOPIC "hoymiles/" INVERTER_SN "/"

#define POLL_INTERVAL_MS   30000
#define WIFI_RETRY_MS      10000
#define MQTT_RETRY_MS      10000
#define BLE_RETRY_MS       60000
#define RESPONSE_TIMEOUT_MS 5000
```

- [ ] **Step 4: Create `src/main.cpp`** (empty skeleton to confirm compile)

```cpp
#include <Arduino.h>
#include "config.h"

void setup() {
    Serial.begin(115200);
    Serial.println("Hoymiles ESP32 bridge starting...");
}

void loop() {
    delay(1000);
}
```

- [ ] **Step 5: Verify empty build compiles**

```bash
cd /path/to/hoymiles-esp32
pio run -e esp32
```

Expected: `SUCCESS` with no errors.

- [ ] **Step 6: Commit**

```bash
git add platformio.ini src/config.h src/main.cpp
git commit -m "feat: scaffold PlatformIO project"
```

---

## Task 2: Crypto module with native tests

**Files:**
- Create: `src/crypto.h`
- Create: `src/crypto.cpp`
- Create: `test/test_crypto/test_crypto.cpp`

The crypto module has zero dependencies on BLE, MQTT, or Arduino-specific APIs. It uses only mbedTLS (built into ESP32 Arduino SDK) and standard C. The native test environment links against the host's OpenSSL/mbedTLS instead.

### Step 1: Create `src/crypto.h`

```cpp
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Applies SHA-256 three times in sequence.
void triple_sha256(const uint8_t *in, size_t in_len, uint8_t out[32]);

// CRC16-Modbus: poly=0x8005, reflected, init=0xFFFF, xorOut=0x0000.
uint16_t crc16_modbus(const uint8_t *data, size_t len);

// Derives V0 AES-128 key from inverter serial number.
// sn: null-terminated 12-char string (e.g. "AABBCCDDEE12")
// key_out: 16-byte output
void v0_derive_key(const char *sn, uint8_t key_out[16]);

// Derives V0 AES-128-CBC IV from cmd, tid, and serial number.
void v0_derive_iv(uint16_t cmd, uint16_t tid, const char *sn, uint8_t iv_out[16]);

// Encrypts plaintext with AES-128-CBC + PKCS7.
// ct_out must be at least pt_len + 16 bytes (PKCS7 padding).
// Returns ciphertext length, or 0 on error.
size_t v0_encrypt(const uint8_t *pt, size_t pt_len,
                  const uint8_t key[16], const uint8_t iv[16],
                  uint8_t *ct_out);

// Decrypts ciphertext with AES-128-CBC + PKCS7.
// pt_out must be at least ct_len bytes.
// Returns plaintext length after stripping padding, or 0 on error.
size_t v0_decrypt(const uint8_t *ct, size_t ct_len,
                  const uint8_t key[16], const uint8_t iv[16],
                  uint8_t *pt_out);

// Derives V1 AES-128-GCM key from encRand.
void v1_derive_key(const uint8_t enc_rand[16], uint8_t key_out[16]);

// Derives V1 AES-128-GCM nonce (12 bytes) from cmd, tid, and encRand.
void v1_derive_nonce(uint16_t cmd, uint16_t tid,
                     const uint8_t enc_rand[16], uint8_t nonce_out[12]);

// Encrypts plaintext with AES-128-GCM.
// ct_out must be at least pt_len bytes. tag_out is 16 bytes.
// Returns false on error.
bool v1_encrypt(const uint8_t *pt, size_t pt_len,
                const uint8_t key[16], const uint8_t nonce[12],
                const uint8_t *aad, size_t aad_len,
                uint8_t *ct_out, uint8_t tag_out[16]);

// Decrypts ciphertext with AES-128-GCM and verifies auth tag.
// pt_out must be at least ct_len bytes.
// Returns false if tag verification fails.
bool v1_decrypt(const uint8_t *ct, size_t ct_len,
                const uint8_t key[16], const uint8_t nonce[12],
                const uint8_t *aad, size_t aad_len,
                const uint8_t tag[16], uint8_t *pt_out);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Create `src/crypto.cpp`**

```cpp
#include "crypto.h"
#include <string.h>

#ifdef NATIVE_TEST
// On host: use OpenSSL via mbedTLS-compatible wrappers
#include <mbedtls/sha256.h>
#include <mbedtls/aes.h>
#include <mbedtls/gcm.h>
#else
// On ESP32: mbedTLS is bundled
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
    // PKCS7 padding
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
    // Strip PKCS7 padding
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
```

- [ ] **Step 3: Create `test/test_crypto/test_crypto.cpp`**

Test vectors were generated from the Python reference implementation (`hoymiles_wifi/crypt_util.py`).

```cpp
#include <unity.h>
#include <string.h>
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
```

- [ ] **Step 4: Add mbedTLS to `platformio.ini` native env** (needed for crypto tests on host)

Append to `platformio.ini`:

```ini
[env:native]
platform = native
test_framework = unity
build_flags =
    -std=c++17
    -DNATIVE_TEST
lib_deps =
    ARMmbed/mbedtls@^3.5.0
```

- [ ] **Step 5: Run native tests — verify they fail (no implementation yet)**

(You haven't run them yet at this point; the files exist but you haven't compiled. This just confirms the build works.)

```bash
pio test -e native
```

Expected: All 9 tests PASS. If any fail, the test vectors are wrong — re-run the Python snippet at the top of this task to regenerate them.

- [ ] **Step 6: Commit**

```bash
git add src/crypto.h src/crypto.cpp test/test_crypto/test_crypto.cpp platformio.ini
git commit -m "feat: add crypto module with native tests"
```

---

## Task 3: Frame module with native tests

**Files:**
- Create: `src/frame.h`
- Create: `src/frame.cpp`
- Create: `test/test_frame/test_frame.cpp`

The frame module only packs/unpacks bytes. It knows nothing about crypto or BLE.

- [ ] **Step 1: Create `src/frame.h`**

```cpp
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
// Returns NULL if buf_len < length_out from frame_parse_header.
const uint8_t *frame_payload(const uint8_t *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Create `src/frame.cpp`**

```cpp
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

    uint16_t crc = crc16_modbus(ciphertext, ct_len);
    uint16_t length = (uint16_t)(ct_len + 10);

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

    memcpy(buf + 10, ciphertext, ct_len);
    if (tag) memcpy(buf + 10 + ct_len, tag, 16);
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
```

- [ ] **Step 3: Create `test/test_frame/test_frame.cpp`**

```cpp
#include <unity.h>
#include <string.h>
#include "../../src/frame.h"
#include "../../src/crypto.h"

void test_frame_build_v0_no_tag(void) {
    uint8_t ct[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t buf[128];
    size_t len = frame_build(buf, sizeof(buf), 0xA201, 1, ct, 4, NULL);

    TEST_ASSERT_EQUAL(14, len);  // 10 header + 4 payload
    TEST_ASSERT_EQUAL_HEX8(0x48, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x4D, buf[1]);
    TEST_ASSERT_EQUAL_HEX16(0xA201, (buf[2] << 8) | buf[3]);  // cmd
    TEST_ASSERT_EQUAL_HEX16(1,      (buf[4] << 8) | buf[5]);  // tid
    // CRC16-Modbus of {01,02,03,04} = 0x2BA1
    TEST_ASSERT_EQUAL_HEX16(0x2BA1, (buf[6] << 8) | buf[7]);
    // length = 4 + 10 = 14
    TEST_ASSERT_EQUAL_HEX16(14, (buf[8] << 8) | buf[9]);
    // Payload
    TEST_ASSERT_EQUAL_MEMORY(ct, buf + 10, 4);
}

void test_frame_build_v1_with_tag(void) {
    uint8_t ct[]  = {0xAA, 0xBB, 0xCC};
    uint8_t tag[16];
    memset(tag, 0x55, 16);
    uint8_t buf[128];
    size_t len = frame_build(buf, sizeof(buf), 0xA311, 2, ct, 3, tag);

    TEST_ASSERT_EQUAL(10 + 3 + 16, len);
    // CRC is over ciphertext only (no tag)
    uint16_t expected_crc = crc16_modbus(ct, 3);
    TEST_ASSERT_EQUAL_HEX16(expected_crc, (buf[6] << 8) | buf[7]);
    // length = 3 + 10 = 13
    TEST_ASSERT_EQUAL_HEX16(13, (buf[8] << 8) | buf[9]);
    // Tag at end
    TEST_ASSERT_EQUAL_MEMORY(tag, buf + 10 + 3, 16);
}

void test_frame_build_buf_too_small(void) {
    uint8_t ct[] = {0x01};
    uint8_t buf[5];  // too small for 10-byte header
    size_t len = frame_build(buf, sizeof(buf), 0xA311, 1, ct, 1, NULL);
    TEST_ASSERT_EQUAL(0, len);
}

void test_frame_parse_header_valid(void) {
    uint8_t ct[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t buf[64];
    frame_build(buf, sizeof(buf), 0xA311, 42, ct, 4, NULL);

    uint16_t cmd, tid, crc, length;
    bool ok = frame_parse_header(buf, sizeof(buf), &cmd, &tid, &crc, &length);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_HEX16(0xA311, cmd);
    TEST_ASSERT_EQUAL_HEX16(42, tid);
    TEST_ASSERT_EQUAL_HEX16(0xC19B, crc);   // CRC16-Modbus of deadbeef
    TEST_ASSERT_EQUAL_HEX16(14, length);    // 4 + 10
}

void test_frame_parse_header_bad_magic(void) {
    uint8_t buf[10] = {0x00, 0x00, 0xA3, 0x11, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0A};
    uint16_t cmd, tid, crc, length;
    bool ok = frame_parse_header(buf, sizeof(buf), &cmd, &tid, &crc, &length);
    TEST_ASSERT_FALSE(ok);
}

void test_frame_parse_header_too_short(void) {
    uint8_t buf[5];
    uint16_t cmd, tid, crc, length;
    bool ok = frame_parse_header(buf, sizeof(buf), &cmd, &tid, &crc, &length);
    TEST_ASSERT_FALSE(ok);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_frame_build_v0_no_tag);
    RUN_TEST(test_frame_build_v1_with_tag);
    RUN_TEST(test_frame_build_buf_too_small);
    RUN_TEST(test_frame_parse_header_valid);
    RUN_TEST(test_frame_parse_header_bad_magic);
    RUN_TEST(test_frame_parse_header_too_short);
    return UNITY_END();
}
```

- [ ] **Step 4: Run native tests**

```bash
pio test -e native
```

Expected: All tests in both `test_crypto` and `test_frame` PASS.

- [ ] **Step 5: Commit**

```bash
git add src/frame.h src/frame.cpp test/test_frame/test_frame.cpp
git commit -m "feat: add frame module with native tests"
```

---

## Task 4: Proto definitions + nanopb code generation

**Files:**
- Create: `proto/RealDataNew.proto`
- Create: `proto/CommCmd.proto`
- Create: `proto/APPInfomationData.proto`
- Create: `src/proto/RealDataNew.pb.c` (generated)
- Create: `src/proto/RealDataNew.pb.h` (generated)
- Create: `src/proto/CommCmd.pb.c` (generated)
- Create: `src/proto/CommCmd.pb.h` (generated)
- Create: `src/proto/APPInfomationData.pb.c` (generated)
- Create: `src/proto/APPInfomationData.pb.h` (generated)

Field numbers are taken verbatim from the installed `hoymiles-wifi` Python library's descriptor.

- [ ] **Step 1: Install nanopb generator**

```bash
pip install nanopb
nanopb_generator --version
```

Expected: prints version like `nanopb-0.4.x`.

- [ ] **Step 2: Create `proto/RealDataNew.proto`**

```proto
syntax = "proto2";

message PvMO {
    optional sint64 serial_number = 1;
    optional int32  port_number   = 2;
    optional int32  voltage       = 3;
    optional int32  current       = 4;
    optional int32  power         = 5;
    optional int32  energy_total  = 6;
    optional int32  energy_daily  = 7;
    optional int32  error_code    = 8;
}

message SGSMO {
    optional sint64 serial_number  = 1;
    optional int32  firmware_version = 2;
    optional int32  voltage          = 3;
    optional int32  frequency        = 4;
    optional int32  active_power     = 5;
    optional int32  reactive_power   = 6;
    optional int32  current          = 7;
    optional int32  power_factor     = 8;
    optional int32  temperature      = 9;
    optional int32  warning_number   = 10;
    optional int32  crc_checksum     = 11;
    optional int32  link_status      = 12;
    optional int32  power_limit      = 13;
}

message RealDataNewResDTO {
    optional bytes  time_ymd_hms = 1;
    optional int32  cp           = 2;
    optional int32  error_code   = 3;
    optional int32  offset       = 4;
    optional int32  time         = 5;
}

message RealDataNewReqDTO {
    optional string device_serial_number = 1;
    optional int32  timestamp            = 2;
    optional int32  ap                   = 3;
    optional int32  cp                   = 4;
    optional int32  firmware_version     = 5;
    optional SGSMO  sgs_data             = 9;
    repeated PvMO   pv_data              = 11;
    optional uint64 dtu_power            = 12;
    optional uint64 dtu_daily_energy     = 13;
}
```

- [ ] **Step 3: Create `proto/CommCmd.proto`**

```proto
syntax = "proto2";

message CommandResDTO {
    optional string dtu_sn      = 1;
    optional int32  time        = 2;
    optional int32  action      = 3;
    optional int32  package_now = 4;
    optional int32  err_code    = 5;
    optional sint64 tid         = 6;
}

message CommandReqDTO {
    optional int32  time        = 1;
    optional int32  action      = 2;
    optional int32  dev_kind    = 3;
    optional int32  package_nub = 4;
    optional int32  package_now = 5;
    optional sint64 tid         = 6;
    optional string data        = 7;
    optional string es_to_sn    = 8;
}
```

- [ ] **Step 4: Create `proto/APPInfomationData.proto`**

```proto
syntax = "proto2";

message APPDtuInfoMO {
    optional int32  device_kind     = 1;
    optional int32  dtu_sw_version  = 2;
    optional int32  dtu_hw_version  = 3;
    optional sint64 dfs             = 24;
    optional sint64 shls            = 25;
    optional int32  type            = 26;
    optional bytes  enc_rand        = 27;
}

message APPInfoDataResDTO {
    optional bytes  time_ymd_hms    = 1;
    optional int32  offset          = 2;
    optional int32  current_package = 3;
    optional uint32 timestamp       = 5;
}

message APPInfoDataReqDTO {
    optional string dtu_serial_number = 1;
    optional uint32 timestamp         = 2;
    optional int32  device_number     = 3;
    optional int32  package_number    = 5;
    optional int32  current_package   = 6;
    optional APPDtuInfoMO dtu_info    = 8;
}
```

- [ ] **Step 5: Create `proto/` nanopb options files** (caps repeated field counts for embedded)

Create `proto/RealDataNew.options`:
```
RealDataNewReqDTO.pv_data    max_count:4
```

Create `proto/APPInfomationData.options`:
```
APPInfoDataReqDTO.dtu_serial_number   max_size:32
```

Create `proto/CommCmd.options`:
```
CommandResDTO.dtu_sn   max_size:32
CommandReqDTO.data     max_size:64
CommandReqDTO.es_to_sn max_size:32
```

- [ ] **Step 6: Generate nanopb C files**

```bash
mkdir -p src/proto
nanopb_generator proto/RealDataNew.proto        -D src/proto
nanopb_generator proto/CommCmd.proto            -D src/proto
nanopb_generator proto/APPInfomationData.proto  -D src/proto
ls src/proto/
```

Expected: `RealDataNew.pb.c  RealDataNew.pb.h  CommCmd.pb.c  CommCmd.pb.h  APPInfomationData.pb.c  APPInfomationData.pb.h`

- [ ] **Step 7: Verify ESP32 build includes generated files**

```bash
pio run -e esp32
```

Expected: `SUCCESS`. If you see "file not found" for `.pb.h`, check that `src/proto/` is on the include path — PlatformIO includes all subdirs of `src/` automatically.

- [ ] **Step 8: Commit**

```bash
git add proto/ src/proto/
git commit -m "feat: add proto definitions and nanopb generated files"
```

---

## Task 5: BLE client

**Files:**
- Create: `src/ble_client.h`
- Create: `src/ble_client.cpp`

BLE client manages the GATT connection. It has no knowledge of crypto or proto decoding. It exposes three operations: connect, write (TX), and set a callback for received notifications (RX).

- [ ] **Step 1: Create `src/ble_client.h`**

```cpp
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Called when a BLE notification arrives on the RX characteristic.
typedef void (*BleRxCallback)(const uint8_t *data, size_t len);

#ifdef __cplusplus
extern "C" {
#endif

// Initializes NimBLE stack. Call once in setup().
void ble_init(void);

// Scans for a device named device_name (exact match), connects, negotiates
// MTU 512, subscribes to RX notifications.
// rx_cb is called from the NimBLE task when data arrives.
// Returns true on success.
bool ble_connect(const char *device_name, BleRxCallback rx_cb);

// Returns true if currently connected to the inverter.
bool ble_is_connected(void);

// Writes data to the TX characteristic (chunked to MTU-3 if needed).
// Returns true if all bytes were written successfully.
bool ble_write(const uint8_t *data, size_t len);

// Disconnects from the inverter.
void ble_disconnect(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Create `src/ble_client.cpp`**

```cpp
#include "ble_client.h"
#include "config.h"
#include <NimBLEDevice.h>
#include <Arduino.h>

#define SVC_UUID  "0000e0ff-3c17-d293-8e48-14fe2e4da212"
#define TX_UUID   "0000ffe1-0000-1000-8000-00805f9b34fb"
#define RX_UUID   "0000ffe2-0000-1000-8000-00805f9b34fb"

static NimBLEClient *s_client = nullptr;
static NimBLERemoteCharacteristic *s_tx = nullptr;
static NimBLERemoteCharacteristic *s_rx = nullptr;
static BleRxCallback s_rx_cb = nullptr;
static bool s_connected = false;

static void notify_cb(NimBLERemoteCharacteristic *chr,
                      uint8_t *data, size_t len, bool is_notify) {
    if (s_rx_cb && is_notify) s_rx_cb(data, len);
}

class ClientCallbacks : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient *client) override {
        s_connected = false;
        Serial.println("[BLE] Disconnected.");
    }
};

static ClientCallbacks s_client_cbs;

void ble_init(void) {
    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
}

bool ble_connect(const char *device_name, BleRxCallback rx_cb) {
    s_rx_cb = rx_cb;

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(100);
    NimBLEScanResults results = scan->start(10);

    NimBLEAdvertisedDevice *target = nullptr;
    for (int i = 0; i < results.getCount(); i++) {
        auto *dev = results.getDevice(i);
        if (dev->getName() == device_name) {
            target = dev;
            break;
        }
    }
    if (!target) {
        Serial.printf("[BLE] Device '%s' not found.\n", device_name);
        return false;
    }

    s_client = NimBLEDevice::createClient();
    s_client->setClientCallbacks(&s_client_cbs, false);
    s_client->setConnectionParams(12, 12, 0, 51);
    if (!s_client->connect(target)) {
        Serial.println("[BLE] Connect failed.");
        return false;
    }

    s_client->setMTU(512);

    auto *svc = s_client->getService(SVC_UUID);
    if (!svc) {
        Serial.println("[BLE] Service not found.");
        s_client->disconnect();
        return false;
    }

    s_tx = svc->getCharacteristic(TX_UUID);
    s_rx = svc->getCharacteristic(RX_UUID);
    if (!s_tx || !s_rx) {
        Serial.println("[BLE] Characteristic not found.");
        s_client->disconnect();
        return false;
    }

    if (!s_rx->subscribe(true, notify_cb)) {
        Serial.println("[BLE] Subscribe failed.");
        s_client->disconnect();
        return false;
    }

    s_connected = true;
    Serial.println("[BLE] Connected and subscribed.");
    return true;
}

bool ble_is_connected(void) {
    return s_connected && s_client && s_client->isConnected();
}

bool ble_write(const uint8_t *data, size_t len) {
    if (!ble_is_connected() || !s_tx) return false;
    uint16_t mtu = s_client->getMTU() - 3;
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = (len - offset > mtu) ? mtu : (len - offset);
        if (!s_tx->writeValue(data + offset, chunk, false)) return false;
        offset += chunk;
    }
    return true;
}

void ble_disconnect(void) {
    if (s_client) {
        s_client->disconnect();
    }
    s_connected = false;
}
```

- [ ] **Step 3: Verify ESP32 build compiles**

```bash
pio run -e esp32
```

Expected: `SUCCESS`.

- [ ] **Step 4: Commit**

```bash
git add src/ble_client.h src/ble_client.cpp
git commit -m "feat: add BLE GATT client"
```

---

## Task 6: Handshake (V0 pairing + CommCmd + NVS)

**Files:**
- Create: `src/handshake.h`
- Create: `src/handshake.cpp`

The handshake module obtains `encRand` (V0 pairing or NVS load) and performs the CommCmd login + time-sync sequence. After a successful handshake, it stores `encRand` in-memory for the poller.

- [ ] **Step 1: Create `src/handshake.h`**

```cpp
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Performs the full BLE handshake:
// 1. Loads encRand from NVS, or runs V0 pairing to extract it and saves it.
// 2. Runs CommCmd login (action=64) + time-sync (action=104).
// tid_start: first tid value to use (caller increments across calls).
// Returns true on success. On success, enc_rand_out holds the 16-byte encRand.
bool handshake_run(uint16_t *tid, uint8_t enc_rand_out[16]);

// Clears the stored encRand from NVS (call if V1 decryption fails repeatedly).
void handshake_clear_nvs(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Create `src/handshake.cpp`**

```cpp
#include "handshake.h"
#include "config.h"
#include "crypto.h"
#include "frame.h"
#include "ble_client.h"
#include "proto/APPInfomationData.pb.h"
#include "proto/CommCmd.pb.h"
#include <Preferences.h>
#include <Arduino.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <time.h>
#include <string.h>

#define CMD_APP_INFO_REQ  0xA201
#define CMD_APP_INFO_RES  0xA301
#define CMD_COMMCMD       0xA305
#define NVS_NS            "hoymiles"
#define NVS_KEY_ENCRAND   "enc_rand"
#define RX_BUF_LEN        1024
#define TX_BUF_LEN        512
#define COMMCMD_LOGIN     64
#define COMMCMD_TIME_SYNC 104

static uint8_t  s_rx_buf[RX_BUF_LEN];
static size_t   s_rx_len = 0;
static bool     s_rx_ready = false;

static void rx_handler(const uint8_t *data, size_t len) {
    if (s_rx_len + len <= RX_BUF_LEN) {
        memcpy(s_rx_buf + s_rx_len, data, len);
        s_rx_len += len;
    }
    s_rx_ready = true;
}

static bool wait_for_rx(uint32_t timeout_ms) {
    s_rx_ready = false;
    s_rx_len = 0;
    uint32_t start = millis();
    while (!s_rx_ready) {
        if (millis() - start > timeout_ms) return false;
        delay(10);
    }
    return true;
}

static bool load_enc_rand_from_nvs(uint8_t enc_rand[16]) {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    size_t len = prefs.getBytesLength(NVS_KEY_ENCRAND);
    if (len != 16) { prefs.end(); return false; }
    prefs.getBytes(NVS_KEY_ENCRAND, enc_rand, 16);
    prefs.end();
    Serial.println("[HS] encRand loaded from NVS.");
    return true;
}

static void save_enc_rand_to_nvs(const uint8_t enc_rand[16]) {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putBytes(NVS_KEY_ENCRAND, enc_rand, 16);
    prefs.end();
    Serial.println("[HS] encRand saved to NVS.");
}

static bool do_v0_pairing(uint16_t *tid, uint8_t enc_rand_out[16]) {
    // Build APPInfoDataResDTO request
    APPInfoDataResDTO req = APPInfoDataResDTO_init_zero;
    req.has_timestamp = true;
    req.timestamp = (uint32_t)time(nullptr);
    req.has_offset = true;
    req.offset = 28800;

    uint8_t pb_buf[128];
    pb_ostream_t stream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    if (!pb_encode(&stream, APPInfoDataResDTO_fields, &req)) {
        Serial.println("[HS] V0 encode failed.");
        return false;
    }
    size_t pb_len = stream.bytes_written;

    // V0 encrypt
    uint8_t v0_key[16], v0_iv[16];
    v0_derive_key(INVERTER_SN, v0_key);
    v0_derive_iv(CMD_APP_INFO_REQ, *tid, INVERTER_SN, v0_iv);

    uint8_t ct[256];
    size_t ct_len = v0_encrypt(pb_buf, pb_len, v0_key, v0_iv, ct);
    if (ct_len == 0) { Serial.println("[HS] V0 encrypt failed."); return false; }

    // Build frame
    uint8_t frame[512];
    size_t frame_len = frame_build(frame, sizeof(frame), CMD_APP_INFO_REQ, *tid, ct, ct_len, NULL);
    (*tid)++;

    if (!ble_write(frame, frame_len)) {
        Serial.println("[HS] V0 write failed.");
        return false;
    }

    // Wait for response
    if (!wait_for_rx(RESPONSE_TIMEOUT_MS)) {
        Serial.println("[HS] V0 timeout.");
        return false;
    }

    // Parse response frame
    uint16_t resp_cmd, resp_tid, resp_crc, resp_len;
    if (!frame_parse_header(s_rx_buf, s_rx_len, &resp_cmd, &resp_tid, &resp_crc, &resp_len)) {
        Serial.println("[HS] V0 response parse failed.");
        return false;
    }
    if (resp_cmd != CMD_APP_INFO_RES) {
        Serial.printf("[HS] Unexpected cmd 0x%04X\n", resp_cmd);
        return false;
    }

    // V0 decrypt response
    const uint8_t *resp_ct = frame_payload(s_rx_buf, s_rx_len);
    size_t resp_ct_len = s_rx_len - HM_HEADER_LEN;

    uint8_t resp_key[16], resp_iv[16];
    v0_derive_key(INVERTER_SN, resp_key);
    v0_derive_iv(CMD_APP_INFO_RES, resp_tid, INVERTER_SN, resp_iv);

    uint8_t pt_buf[256];
    size_t pt_len = v0_decrypt(resp_ct, resp_ct_len, resp_key, resp_iv, pt_buf);
    if (pt_len == 0) { Serial.println("[HS] V0 decrypt failed."); return false; }

    // Decode APPInfoDataReqDTO
    APPInfoDataReqDTO info = APPInfoDataReqDTO_init_zero;
    pb_istream_t istream = pb_istream_from_buffer(pt_buf, pt_len);
    if (!pb_decode(&istream, APPInfoDataReqDTO_fields, &info)) {
        Serial.println("[HS] V0 proto decode failed.");
        return false;
    }

    if (!info.has_dtu_info || info.dtu_info.enc_rand.size != 16) {
        Serial.println("[HS] enc_rand not found in response.");
        return false;
    }

    memcpy(enc_rand_out, info.dtu_info.enc_rand.bytes, 16);
    Serial.printf("[HS] enc_rand: ");
    for (int i = 0; i < 16; i++) Serial.printf("%02x", enc_rand_out[i]);
    Serial.println();
    return true;
}

static bool do_commcmd(uint16_t *tid, const uint8_t enc_rand[16], int action) {
    CommandResDTO req = CommandResDTO_init_zero;
    req.has_dtu_sn = true;
    strncpy(req.dtu_sn, INVERTER_SN, sizeof(req.dtu_sn) - 1);
    req.has_time = true;
    req.time = (int32_t)time(nullptr);
    req.has_action = true;
    req.action = action;
    req.has_tid = true;
    req.tid = *tid;

    uint8_t pb_buf[128];
    pb_ostream_t stream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    if (!pb_encode(&stream, CommandResDTO_fields, &req)) {
        Serial.println("[HS] CommCmd encode failed.");
        return false;
    }
    size_t pb_len = stream.bytes_written;

    // V1 encrypt
    uint8_t key[16], nonce[12];
    v1_derive_key(enc_rand, key);
    v1_derive_nonce(CMD_COMMCMD, *tid, enc_rand, nonce);
    uint8_t aad[4];
    aad[0] = CMD_COMMCMD & 0xFF;
    aad[1] = (CMD_COMMCMD >> 8) & 0xFF;
    aad[2] = *tid & 0xFF;
    aad[3] = (*tid >> 8) & 0xFF;

    uint8_t ct[256], tag[16];
    if (!v1_encrypt(pb_buf, pb_len, key, nonce, aad, 4, ct, tag)) {
        Serial.println("[HS] CommCmd V1 encrypt failed.");
        return false;
    }

    uint8_t frame[512];
    size_t frame_len = frame_build(frame, sizeof(frame), CMD_COMMCMD, *tid, ct, pb_len, tag);
    (*tid)++;

    if (!ble_write(frame, frame_len)) {
        Serial.println("[HS] CommCmd write failed.");
        return false;
    }

    if (!wait_for_rx(RESPONSE_TIMEOUT_MS)) {
        Serial.printf("[HS] CommCmd action=%d timeout.\n", action);
        return false;
    }

    Serial.printf("[HS] CommCmd action=%d OK.\n", action);
    return true;
}

bool handshake_run(uint16_t *tid, uint8_t enc_rand_out[16]) {
    // Swap in handshake's RX handler (must be connected before calling)
    ble_set_rx_callback(rx_handler);

    // 1. Try loading encRand from NVS
    if (!load_enc_rand_from_nvs(enc_rand_out)) {
        // 2. Run V0 pairing to get encRand
        if (!do_v0_pairing(tid, enc_rand_out)) return false;
        save_enc_rand_to_nvs(enc_rand_out);
    }

    // 3. CommCmd login
    if (!do_commcmd(tid, enc_rand_out, COMMCMD_LOGIN)) return false;
    delay(200);

    // 4. CommCmd time-sync
    if (!do_commcmd(tid, enc_rand_out, COMMCMD_TIME_SYNC)) return false;
    return true;
}

void handshake_clear_nvs(void) {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.remove(NVS_KEY_ENCRAND);
    prefs.end();
    Serial.println("[HS] encRand cleared from NVS.");
}
```

**Note on RX handler registration:** `handshake.cpp` uses a local `rx_handler` but `ble_connect()` takes the callback at connection time. Call `ble_connect(BLE_DEVICE_NAME, rx_handler)` from `main.cpp` before calling `handshake_run()`. The poller will set its own callback via `ble_connect` on reconnect, OR you need to expose a `ble_set_rx_callback()` helper. The simplest approach: add `void ble_set_rx_callback(BleRxCallback cb)` to `ble_client.h` so `handshake` and `poller` can swap the callback without reconnecting. Add this function to `ble_client.cpp`:

```cpp
// Add to ble_client.h:
void ble_set_rx_callback(BleRxCallback cb);

// Add to ble_client.cpp:
void ble_set_rx_callback(BleRxCallback cb) {
    s_rx_cb = cb;
}
```

- [ ] **Step 3: Verify ESP32 build compiles**

```bash
pio run -e esp32
```

Expected: `SUCCESS`.

- [ ] **Step 4: Commit**

```bash
git add src/handshake.h src/handshake.cpp src/ble_client.h src/ble_client.cpp
git commit -m "feat: add BLE handshake (V0 pairing + CommCmd)"
```

---

## Task 7: Poller (RealDataNew + MQTT publish)

**Files:**
- Create: `src/poller.h`
- Create: `src/poller.cpp`

The poller sends a V1-encrypted RealDataNew request, handles multi-page responses, decodes the protobuf, scales the raw integers, and publishes individual MQTT topics.

- [ ] **Step 1: Create `src/poller.h`**

```cpp
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <PubSubClient.h>

#ifdef __cplusplus
extern "C" {
#endif

// Polls one cycle of RealDataNew. Publishes MQTT topics if successful.
// Returns true if at least one page was received and decoded.
bool poller_poll(PubSubClient &mqtt, uint16_t *tid, const uint8_t enc_rand[16]);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Create `src/poller.cpp`**

```cpp
#include "poller.h"
#include "config.h"
#include "crypto.h"
#include "frame.h"
#include "ble_client.h"
#include "proto/RealDataNew.pb.h"
#include <Arduino.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#define CMD_REAL  0xA311

static uint8_t  s_rx_buf[2048];
static size_t   s_rx_len = 0;
static bool     s_rx_ready = false;

static void rx_handler(const uint8_t *data, size_t len) {
    if (s_rx_len + len <= sizeof(s_rx_buf)) {
        memcpy(s_rx_buf + s_rx_len, data, len);
        s_rx_len += len;
    }
    s_rx_ready = true;
}

static bool wait_rx(uint32_t timeout_ms) {
    s_rx_ready = false;
    s_rx_len = 0;
    uint32_t start = millis();
    while (!s_rx_ready) {
        if (millis() - start > timeout_ms) return false;
        delay(5);
    }
    return true;
}

static bool send_real_req(uint16_t *tid, const uint8_t enc_rand[16], int cp) {
    RealDataNewResDTO req = RealDataNewResDTO_init_zero;
    req.has_cp = true;
    req.cp = cp;
    req.has_time = true;
    req.time = (int32_t)time(nullptr);
    req.has_offset = true;
    req.offset = 28800;

    uint8_t pb_buf[64];
    pb_ostream_t stream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    if (!pb_encode(&stream, RealDataNewResDTO_fields, &req)) return false;
    size_t pb_len = stream.bytes_written;

    uint8_t key[16], nonce[12];
    v1_derive_key(enc_rand, key);
    v1_derive_nonce(CMD_REAL, *tid, enc_rand, nonce);
    uint8_t aad[4];
    aad[0] = CMD_REAL & 0xFF;
    aad[1] = (CMD_REAL >> 8) & 0xFF;
    aad[2] = *tid & 0xFF;
    aad[3] = (*tid >> 8) & 0xFF;

    uint8_t ct[256], tag[16];
    if (!v1_encrypt(pb_buf, pb_len, key, nonce, aad, 4, ct, tag)) return false;

    uint8_t frame[512];
    size_t frame_len = frame_build(frame, sizeof(frame), CMD_REAL, *tid, ct, pb_len, tag);
    (*tid)++;

    return ble_write(frame, frame_len);
}

static bool decode_and_publish(PubSubClient &mqtt,
                                const uint8_t *ct, size_t ct_len,
                                const uint8_t *tag,
                                uint16_t resp_tid, const uint8_t enc_rand[16],
                                RealDataNewReqDTO *out) {
    uint8_t key[16], nonce[12];
    v1_derive_key(enc_rand, key);
    v1_derive_nonce(CMD_REAL, resp_tid, enc_rand, nonce);
    uint8_t aad[4];
    aad[0] = CMD_REAL & 0xFF;
    aad[1] = (CMD_REAL >> 8) & 0xFF;
    aad[2] = resp_tid & 0xFF;
    aad[3] = (resp_tid >> 8) & 0xFF;

    uint8_t pt[1024];
    if (!v1_decrypt(ct, ct_len, key, nonce, aad, 4, tag, pt)) {
        Serial.println("[PL] GCM auth tag failure — skipping.");
        return false;
    }

    pb_istream_t istream = pb_istream_from_buffer(pt, ct_len);
    if (!pb_decode(&istream, RealDataNewReqDTO_fields, out)) {
        Serial.println("[PL] Proto decode failed.");
        return false;
    }
    return true;
}

static void publish_float(PubSubClient &mqtt, const char *subtopic, float value) {
    char topic[128], payload[32];
    snprintf(topic, sizeof(topic), "%s%s", MQTT_BASE_TOPIC, subtopic);
    snprintf(payload, sizeof(payload), "%.3f", value);
    mqtt.publish(topic, payload, true);
}

static void publish_str(PubSubClient &mqtt, const char *subtopic, const char *value) {
    char topic[128];
    snprintf(topic, sizeof(topic), "%s%s", MQTT_BASE_TOPIC, subtopic);
    mqtt.publish(topic, value, true);
}

bool poller_poll(PubSubClient &mqtt, uint16_t *tid, const uint8_t enc_rand[16]) {
    ble_set_rx_callback(rx_handler);

    // Send first request (cp=0)
    if (!send_real_req(tid, enc_rand, 0)) {
        Serial.println("[PL] Failed to send request.");
        return false;
    }
    if (!wait_rx(RESPONSE_TIMEOUT_MS)) {
        Serial.println("[PL] Timeout waiting for response.");
        return false;
    }

    // Parse first response
    uint16_t cmd, resp_tid, crc, length;
    if (!frame_parse_header(s_rx_buf, s_rx_len, &cmd, &resp_tid, &crc, &length)) {
        Serial.println("[PL] Frame parse failed.");
        return false;
    }

    const uint8_t *payload = frame_payload(s_rx_buf, s_rx_len);
    size_t ct_len = s_rx_len - HM_HEADER_LEN - 16;  // strip header and GCM tag
    const uint8_t *tag = s_rx_buf + s_rx_len - 16;

    RealDataNewReqDTO combined = RealDataNewReqDTO_init_zero;
    if (!decode_and_publish(mqtt, payload, ct_len, tag, resp_tid, enc_rand, &combined)) {
        return false;
    }

    // Fetch additional pages
    for (int cp = 1; cp < combined.ap; cp++) {
        if (!send_real_req(tid, enc_rand, cp)) return false;
        if (!wait_rx(RESPONSE_TIMEOUT_MS)) return false;

        if (!frame_parse_header(s_rx_buf, s_rx_len, &cmd, &resp_tid, &crc, &length)) return false;
        payload = frame_payload(s_rx_buf, s_rx_len);
        ct_len = s_rx_len - HM_HEADER_LEN - 16;
        tag = s_rx_buf + s_rx_len - 16;

        RealDataNewReqDTO page = RealDataNewReqDTO_init_zero;
        if (!decode_and_publish(mqtt, payload, ct_len, tag, resp_tid, enc_rand, &page)) continue;

        // Merge pv_data
        for (size_t i = 0; i < page.pv_data_count; i++) {
            if (combined.pv_data_count < RealDataNewReqDTO_pv_data_MAX_COUNT) {
                combined.pv_data[combined.pv_data_count++] = page.pv_data[i];
            }
        }
    }

    // Publish AC output (first SGSMO)
    if (combined.has_sgs_data) {
        SGSMO &ac = combined.sgs_data;
        publish_float(mqtt, "ac/voltage",      ac.voltage      / 10.0f);
        publish_float(mqtt, "ac/frequency",    ac.frequency    / 100.0f);
        publish_float(mqtt, "ac/power",        ac.active_power / 10.0f);
        publish_float(mqtt, "ac/current",      ac.current      / 100.0f);
        publish_float(mqtt, "ac/power_factor", ac.power_factor / 100.0f);
        publish_float(mqtt, "ac/temperature",  ac.temperature  / 10.0f);
        publish_float(mqtt, "ac/power_limit",  ac.power_limit  / 10.0f);
    }

    // Publish per-panel DC input
    float total_energy_kWh = 0.0f;
    for (size_t i = 0; i < combined.pv_data_count; i++) {
        PvMO &pv = combined.pv_data[i];
        char base[32];
        snprintf(base, sizeof(base), "pv/%d/", (int)pv.port_number);
        char subtopic[64];

        snprintf(subtopic, sizeof(subtopic), "%svoltage",      base); publish_float(mqtt, subtopic, pv.voltage      / 10.0f);
        snprintf(subtopic, sizeof(subtopic), "%scurrent",      base); publish_float(mqtt, subtopic, pv.current      / 100.0f);
        snprintf(subtopic, sizeof(subtopic), "%spower",        base); publish_float(mqtt, subtopic, pv.power        / 10.0f);
        snprintf(subtopic, sizeof(subtopic), "%senergy_today", base); publish_float(mqtt, subtopic, pv.energy_daily / 1000.0f);
        snprintf(subtopic, sizeof(subtopic), "%senergy_total", base); publish_float(mqtt, subtopic, pv.energy_total / 1000.0f);

        total_energy_kWh += pv.energy_total / 1000.0f;
    }

    // Top-level topics
    publish_float(mqtt, "energy_today", combined.dtu_daily_energy / 1000.0f);
    publish_float(mqtt, "energy_total", total_energy_kWh);

    char ts[16];
    snprintf(ts, sizeof(ts), "%lu", (unsigned long)time(nullptr));
    publish_str(mqtt, "last_seen", ts);

    Serial.printf("[PL] Published. ac_power=%.1fW  pv_panels=%d\n",
                  combined.has_sgs_data ? combined.sgs_data.active_power / 10.0f : 0.0f,
                  (int)combined.pv_data_count);
    return true;
}
```

- [ ] **Step 3: Verify ESP32 build compiles**

```bash
pio run -e esp32
```

Expected: `SUCCESS`.

- [ ] **Step 4: Commit**

```bash
git add src/poller.h src/poller.cpp
git commit -m "feat: add poller (RealDataNew decode + MQTT publish)"
```

---

## Task 8: Main — state machine + WiFi + MQTT + watchdog

**Files:**
- Modify: `src/main.cpp` (replace the skeleton from Task 1)

- [ ] **Step 1: Replace `src/main.cpp`**

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_task_wdt.h>
#include <time.h>
#include "config.h"
#include "ble_client.h"
#include "handshake.h"
#include "poller.h"

static WiFiClient   s_wifi_client;
static PubSubClient s_mqtt(s_wifi_client);

static uint16_t s_tid = 1;
static uint8_t  s_enc_rand[16] = {0};
static bool     s_enc_rand_ready = false;

static bool wifi_connect(void) {
    if (WiFi.status() == WL_CONNECTED) return true;
    Serial.printf("[WiFi] Connecting to %s …\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > WIFI_RETRY_MS) {
            Serial.println("[WiFi] Timeout.");
            return false;
        }
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\n[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
    configTime(0, 0, "pool.ntp.org");
    return true;
}

static bool mqtt_connect(void) {
    if (s_mqtt.connected()) return true;
    Serial.printf("[MQTT] Connecting to %s:%d …\n", MQTT_HOST, MQTT_PORT);
    char lwt_topic[128];
    snprintf(lwt_topic, sizeof(lwt_topic), "%sstatus", MQTT_BASE_TOPIC);
    if (!s_mqtt.connect(MQTT_CLIENT_ID, nullptr, nullptr, lwt_topic, 1, true, "offline")) {
        Serial.printf("[MQTT] Failed, rc=%d\n", s_mqtt.state());
        return false;
    }
    Serial.println("[MQTT] Connected.");
    // Publish firmware_version once
    char ver_topic[128];
    snprintf(ver_topic, sizeof(ver_topic), "%sfirmware_version", MQTT_BASE_TOPIC);
    s_mqtt.publish(ver_topic, "esp32-1.0.0", true);
    return true;
}

static bool ble_connect_and_handshake(void) {
    if (!ble_connect(BLE_DEVICE_NAME, nullptr)) return false;
    if (!handshake_run(&s_tid, s_enc_rand)) {
        ble_disconnect();
        return false;
    }
    s_enc_rand_ready = true;
    return true;
}

void setup(void) {
    Serial.begin(115200);
    Serial.println("[main] Starting Hoymiles ESP32 bridge.");
    esp_task_wdt_config_t wdt_cfg = {.timeout_ms = 30000, .idle_core_mask = 0, .trigger_panic = true};
    esp_task_wdt_reconfigure(&wdt_cfg);
    esp_task_wdt_add(NULL);

    s_mqtt.setServer(MQTT_HOST, MQTT_PORT);
    s_mqtt.setBufferSize(512);
    ble_init();
}

void loop(void) {
    esp_task_wdt_reset();

    // Reconnect WiFi
    if (!wifi_connect()) { delay(WIFI_RETRY_MS); return; }

    // Reconnect MQTT
    if (!mqtt_connect()) { delay(MQTT_RETRY_MS); return; }

    // Reconnect BLE + handshake
    if (!ble_is_connected() || !s_enc_rand_ready) {
        if (!ble_connect_and_handshake()) {
            delay(BLE_RETRY_MS);
            return;
        }
        // Publish online after successful handshake
        char topic[128];
        snprintf(topic, sizeof(topic), "%sstatus", MQTT_BASE_TOPIC);
        s_mqtt.publish(topic, "online", true);
    }

    // Poll one cycle
    if (!poller_poll(s_mqtt, &s_tid, s_enc_rand)) {
        // If GCM failures persist, clear NVS and force re-pairing next cycle
        handshake_clear_nvs();
        s_enc_rand_ready = false;
        ble_disconnect();
    }

    s_mqtt.loop();
    delay(POLL_INTERVAL_MS);
}
```

- [ ] **Step 2: Verify final ESP32 build compiles**

```bash
pio run -e esp32
```

Expected: `SUCCESS`. Check for any "undefined reference" or "undefined type" errors and resolve them before proceeding.

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "feat: add main loop with WiFi+MQTT+BLE state machine and watchdog"
```

---

## Task 9: Flash + integration verification

**Files:** None — manual verification steps.

- [ ] **Step 1: Fill in `src/config.h` with your real credentials**

Edit `src/config.h`:
- `WIFI_SSID` / `WIFI_PASSWORD` — your garage WiFi
- `MQTT_HOST` — IP of your MQTT broker
- `BLE_DEVICE_NAME` — exact BLE advertisement name from inverter (start a BLE scanner app to confirm — it should appear as `RMI-XXXXXXXXXXXX`)
- `INVERTER_SN` — the 12-char tail after `RMI-`

- [ ] **Step 2: Flash to the ESP32**

Connect ESP32 via USB. Then:

```bash
pio run -e esp32 --target upload
```

Expected: upload succeeds, device reboots.

- [ ] **Step 3: Monitor serial output**

```bash
pio device monitor
```

Expected sequence (daytime, inverter on):
```
[main] Starting Hoymiles ESP32 bridge.
[WiFi] Connecting to your-ssid …
[WiFi] Connected. IP: 192.168.x.x
[MQTT] Connecting to 192.168.1.50:1883 …
[MQTT] Connected.
[BLE] Device 'RMI-AABBCCDDEE12' found.
[BLE] Connected and subscribed.
[HS] encRand loaded from NVS.          ← or "V0 pairing..." on first boot
[HS] CommCmd action=64 OK.
[HS] CommCmd action=104 OK.
[PL] Published. ac_power=XXX.XW  pv_panels=2
```

If you see `[BLE] Device not found` — the inverter is off or out of range.
If you see `[HS] V0 pairing...` followed by failure — the SN in `config.h` doesn't match.

- [ ] **Step 4: Verify MQTT topics arrive at broker**

From any machine that can reach the broker:

```bash
mosquitto_sub -h 192.168.1.50 -t "hoymiles/AABBCCDDEE12/#" -v
```

Expected output (one per 30 s):
```
hoymiles/AABBCCDDEE12/status online
hoymiles/AABBCCDDEE12/ac/power 412.300
hoymiles/AABBCCDDEE12/ac/voltage 231.500
hoymiles/AABBCCDDEE12/pv/1/power 208.100
hoymiles/AABBCCDDEE12/pv/2/power 204.200
hoymiles/AABBCCDDEE12/energy_today 1.234
hoymiles/AABBCCDDEE12/last_seen 1749380000
...
```

- [ ] **Step 5: Validate scale factors against the Python hiflow-ble output**

If you have access to the `hiflow-ble` Python library, run it against the same inverter and compare the values for `ac/power`, `pv/1/voltage`, etc. The scale factors in the plan (÷10, ÷100, ÷1000) follow standard Hoymiles convention but should be confirmed on the real device. Adjust in `poller.cpp` if values are off by a factor of 10.

- [ ] **Step 6: Test night-mode reconnect**

Wait until evening when the inverter shuts down. Confirm in serial output:
```
[BLE] Disconnected.
```
Then confirm 60-second retry loop without crashing. In the morning, confirm automatic reconnect and resume publishing.

- [ ] **Step 7: Final commit (config file cleaned)**

Before committing, revert `src/config.h` to placeholder values (real credentials must not be committed):

```bash
git diff src/config.h  # verify only placeholder values
git add -p             # stage only non-sensitive changes
git commit -m "feat: verify integration, clean config placeholders"
```
