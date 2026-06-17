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
#include <esp_task_wdt.h>

// App->DTU commands start 0xA3; the DTU's replies start 0xA2 (per the
// hoymiles-wifi const.py: "App -> DTU start with 0xa3, responses start 0xa2").
// For app-info pairing the app SENDS 0xA301 and the DTU REPLIES with 0xA201.
#define CMD_APP_INFO_SEND   0xA301   // CMD_APP_INFO_DATA_RES_DTO (app -> DTU)
#define CMD_APP_INFO_REPLY  0xA201   // CMD_APP_INFO_DATA_REQ_DTO (DTU -> app)
#define CMD_COMMCMD       0xA305
#define NVS_NS            "hoymiles"
#define NVS_KEY_ENCRAND   "enc_rand"
#define RX_BUF_LEN        1024
#define TX_BUF_LEN        512
#define COMMCMD_LOGIN     64
#define COMMCMD_TIME_SYNC 104

static volatile uint8_t  s_rx_buf[RX_BUF_LEN];
static volatile size_t   s_rx_len   = 0;
static volatile bool     s_rx_ready = false;

// Diagnostic: print up to `n` bytes of `buf` as hex with a label.
static void hexdump(const char *label, const uint8_t *buf, size_t n) {
    Serial.printf("[HS] %s (%u bytes):", label, (unsigned)n);
    for (size_t i = 0; i < n; i++) Serial.printf(" %02X", buf[i]);
    Serial.println();
}

static void rx_handler(const uint8_t *data, size_t len) {
    if (s_rx_ready) return;
    if (s_rx_len + len <= RX_BUF_LEN) {
        memcpy((uint8_t *)s_rx_buf + s_rx_len, data, len);
        s_rx_len += len;
    }
    // Only signal complete when the full frame has arrived.
    // Need at least 10 bytes to read the header.
    if (s_rx_len >= HM_HEADER_LEN) {
        uint16_t wire_len = ((uint16_t)s_rx_buf[8] << 8) | s_rx_buf[9];
        uint16_t cmd      = ((uint16_t)s_rx_buf[2] << 8) | s_rx_buf[3];
        // V1 frames (CommCmd=0xA305, RealDataNew=0xA311) append a 16-byte GCM tag
        size_t tag_len = (cmd == 0xA305 || cmd == 0xA311) ? 16 : 0;
        size_t expected = (size_t)wire_len + tag_len;
        if (s_rx_len >= expected) {
            s_rx_ready = true;
        }
    }
}

// Arm RX accumulation. MUST be called before ble_write() so a fast response
// notification arriving before wait_for_rx() runs cannot be dropped.
static void rx_reset(void) {
    s_rx_len = 0;
    s_rx_ready = false;
}

static bool wait_for_rx(uint32_t timeout_ms) {
    uint32_t start = millis();
    while (!s_rx_ready) {
        if (millis() - start > timeout_ms) return false;
        esp_task_wdt_reset();
        delay(10);
    }
    return true;
}

static bool load_enc_rand_from_nvs(uint8_t enc_rand[16]) {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    size_t len = prefs.getBytesLength(NVS_KEY_ENCRAND);
    if (len != 16) {
        prefs.end();
        return false;
    }
    prefs.getBytes(NVS_KEY_ENCRAND, enc_rand, 16);
    prefs.end();
    return true;
}

static void save_enc_rand_to_nvs(const uint8_t enc_rand[16]) {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putBytes(NVS_KEY_ENCRAND, enc_rand, 16);
    prefs.end();
}

static bool do_v0_pairing(const char *sn, uint16_t *tid, uint8_t enc_rand_out[16]) {
    // Encode APPInfoDataResDTO
    // NOTE: the DTU acts as the "server", so the ESP32 SENDS the library's
    // ...ResDTO and DECODES the ...ReqDTO. This inversion is intentional and
    // mirrors the hoymiles-wifi reference. Verify against a device capture.
    APPInfoDataResDTO req = APPInfoDataResDTO_init_default;
    // The reference (hiflow-ble) sets three fields: time_ymd_hms, offset, time.
    // Omitting time_ymd_hms left the DTU silent, so set all three.
    time_t now = time(nullptr);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    req.has_time_ymd_hms = true;
    req.time_ymd_hms.size = strftime((char *)req.time_ymd_hms.bytes,
                                     sizeof(req.time_ymd_hms.bytes),
                                     "%Y-%m-%d %H:%M:%S", &tmv);
    req.has_timestamp = true;
    req.timestamp = (uint32_t)now;
    req.has_offset = true;
    req.offset = 28800;

    uint8_t pb_buf[TX_BUF_LEN];
    pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    if (!pb_encode(&ostream, APPInfoDataResDTO_fields, &req)) {
        Serial.println("[HS] V0: pb_encode failed");
        return false;
    }
    size_t pb_len = ostream.bytes_written;

    // Derive V0 key and IV
    uint8_t key[16], iv[16];
    v0_derive_key(sn, key);
    v0_derive_iv(CMD_APP_INFO_SEND, *tid, sn, iv);

    // V0 encrypt (PKCS7 padding adds up to 16 bytes)
    uint8_t ct[TX_BUF_LEN + 16];
    size_t ct_len = v0_encrypt(pb_buf, pb_len, key, iv, ct);
    if (ct_len == 0) {
        Serial.println("[HS] V0: encrypt failed");
        return false;
    }

    // Build frame (no tag for V0)
    uint8_t frame[TX_BUF_LEN + 64];
    size_t frame_len = frame_build(frame, sizeof(frame),
                                   CMD_APP_INFO_SEND, *tid, ct, ct_len, NULL);
    if (frame_len == 0) {
        Serial.println("[HS] V0: frame_build failed");
        return false;
    }

    // Register callback and arm RX before writing, so a fast response cannot
    // land in the window between the write and wait_for_rx().
    ble_set_rx_callback(rx_handler);
    rx_reset();

    hexdump("V0 TX frame", frame, frame_len < 48 ? frame_len : 48);
    if (!ble_write(frame, frame_len)) {
        Serial.println("[HS] V0: ble_write failed");
        return false;
    }

    // Wait for response
    if (!wait_for_rx(RESPONSE_TIMEOUT_MS)) {
        Serial.printf("[HS] V0: timeout waiting for response (rx_len=%u)\n",
                      (unsigned)s_rx_len);
        if (s_rx_len > 0) hexdump("V0 partial RX", (const uint8_t *)s_rx_buf,
                                  s_rx_len < 48 ? s_rx_len : 48);
        return false;
    }

    hexdump("V0 RX frame", (const uint8_t *)s_rx_buf, s_rx_len < 48 ? s_rx_len : 48);

    // Parse response header
    uint16_t rcmd, rtid, rcrc, rlen;
    if (!frame_parse_header((const uint8_t *)s_rx_buf, s_rx_len, &rcmd, &rtid, &rcrc, &rlen)) {
        Serial.println("[HS] V0: frame_parse_header failed");
        return false;
    }
    if (rcmd != CMD_APP_INFO_REPLY) {
        Serial.printf("[HS] V0: unexpected cmd 0x%04X\n", rcmd);
        return false;
    }
    if (rlen < HM_HEADER_LEN) {
        Serial.println("[HS] V0: rlen too small");
        return false;
    }
    size_t payload_ct_len = rlen - HM_HEADER_LEN;
    if ((size_t)rlen > s_rx_len || payload_ct_len == 0) {
        Serial.println("[HS] V0: declared length exceeds received / empty.");
        return false;
    }

    const uint8_t *payload = frame_payload((const uint8_t *)s_rx_buf, s_rx_len);
    if (!payload) {
        Serial.println("[HS] V0: frame_payload failed");
        return false;
    }
    if (crc16_modbus(payload, payload_ct_len) != rcrc) {
        Serial.println("[HS] V0: CRC mismatch.");
        return false;
    }

    // V0 decrypt with response cmd and tid
    uint8_t rkey[16], riv[16];
    v0_derive_key(sn, rkey);
    v0_derive_iv(rcmd, rtid, sn, riv);

    uint8_t pt[RX_BUF_LEN];
    if (payload_ct_len > sizeof(pt)) return false;
    size_t pt_len = v0_decrypt(payload, payload_ct_len, rkey, riv, pt);
    if (pt_len == 0) {
        Serial.println("[HS] V0: decrypt failed");
        return false;
    }

    // Decode APPInfoDataReqDTO
    APPInfoDataReqDTO info = APPInfoDataReqDTO_init_default;
    pb_istream_t istream = pb_istream_from_buffer(pt, pt_len);
    if (!pb_decode(&istream, APPInfoDataReqDTO_fields, &info)) {
        Serial.println("[HS] V0: pb_decode failed");
        return false;
    }

    // Extract encRand (field 27 in APPDtuInfoMO)
    if (!info.has_dtu_info || !info.dtu_info.has_enc_rand) {
        Serial.println("[HS] V0: enc_rand missing");
        return false;
    }
    if (info.dtu_info.enc_rand.size != 16) {
        Serial.printf("[HS] V0: enc_rand size %d != 16\n", (int)info.dtu_info.enc_rand.size);
        return false;
    }
    memcpy(enc_rand_out, info.dtu_info.enc_rand.bytes, 16);

    // Persist to NVS
    save_enc_rand_to_nvs(enc_rand_out);

    (*tid)++;
    return true;
}

static bool do_commcmd(const char *sn, uint16_t *tid, const uint8_t enc_rand[16], int32_t action) {
    uint16_t req_tid = *tid;
    // Encode CommandResDTO
    CommandResDTO cmd_msg = CommandResDTO_init_default;
    cmd_msg.has_dtu_sn = true;
    strncpy(cmd_msg.dtu_sn, sn, sizeof(cmd_msg.dtu_sn) - 1);
    cmd_msg.dtu_sn[sizeof(cmd_msg.dtu_sn) - 1] = '\0';
    cmd_msg.has_time = true;
    cmd_msg.time = (int32_t)time(nullptr);
    cmd_msg.has_action = true;
    cmd_msg.action = action;
    cmd_msg.has_tid = true;
    cmd_msg.tid = (int64_t)(*tid);

    uint8_t pb_buf[TX_BUF_LEN];
    pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    if (!pb_encode(&ostream, CommandResDTO_fields, &cmd_msg)) {
        Serial.println("[HS] CommCmd: pb_encode failed");
        return false;
    }
    size_t pb_len = ostream.bytes_written;

    // Derive V1 key and nonce
    uint8_t key[16], nonce[12], tag[16];
    v1_derive_key(enc_rand, key);
    v1_derive_nonce(CMD_COMMCMD, *tid, enc_rand, nonce);

    // AAD = LE_u16(cmd) + LE_u16(tid) = {cmd_lo, cmd_hi, tid_lo, tid_hi}
    uint8_t aad[4] = {
        (uint8_t)(CMD_COMMCMD & 0xFF),
        (uint8_t)((CMD_COMMCMD >> 8) & 0xFF),
        (uint8_t)(*tid & 0xFF),
        (uint8_t)((*tid >> 8) & 0xFF)
    };

    // V1 encrypt (GCM: ciphertext same size as plaintext)
    uint8_t ct[TX_BUF_LEN];
    if (!v1_encrypt(pb_buf, pb_len, key, nonce, aad, sizeof(aad), ct, tag)) {
        Serial.println("[HS] CommCmd: v1_encrypt failed");
        return false;
    }

    // Build frame WITH tag; ct_len = pb_len (GCM doesn't pad)
    uint8_t frame[TX_BUF_LEN + 64];
    size_t frame_len = frame_build(frame, sizeof(frame),
                                   CMD_COMMCMD, *tid, ct, pb_len, tag);
    if (frame_len == 0) {
        Serial.println("[HS] CommCmd: frame_build failed");
        return false;
    }

    // Increment tid after building frame
    (*tid)++;

    // Register callback and arm RX before writing, so a fast response cannot
    // land in the window between the write and wait_for_rx().
    ble_set_rx_callback(rx_handler);
    rx_reset();

    if (!ble_write(frame, frame_len)) {
        Serial.println("[HS] CommCmd: ble_write failed");
        return false;
    }

    // Wait for any response (success if we receive anything)
    if (!wait_for_rx(RESPONSE_TIMEOUT_MS)) {
        Serial.printf("[HS] CommCmd action=%d: timeout\n", (int)action);
        return false;
    }

    uint16_t rcmd, rtid, rcrc, rlen;
    if (!frame_parse_header((const uint8_t *)s_rx_buf, s_rx_len, &rcmd, &rtid, &rcrc, &rlen)) {
        Serial.println("[HS] CommCmd: frame_parse_header failed");
        return false;
    }
    if (rcmd != CMD_COMMCMD) {
        Serial.printf("[HS] CommCmd: unexpected cmd 0x%04X\n", rcmd);
        return false;
    }
    if (rtid != req_tid) {
        Serial.printf("[HS] CommCmd: tid mismatch (req=%u resp=%u)\n", req_tid, rtid);
        return false;
    }
    if (rlen < HM_HEADER_LEN || (size_t)rlen + 16 > s_rx_len) {
        Serial.println("[HS] CommCmd: bad frame length");
        return false;
    }
    size_t resp_ct_len = (size_t)rlen - HM_HEADER_LEN;
    if (resp_ct_len == 0) { Serial.println("[HS] CommCmd: empty ct"); return false; }
    const uint8_t *resp_ct = frame_payload((const uint8_t *)s_rx_buf, s_rx_len);
    const uint8_t *resp_tag = (const uint8_t *)s_rx_buf + rlen;
    if (crc16_modbus(resp_ct, resp_ct_len) != rcrc) {
        Serial.println("[HS] CommCmd: CRC mismatch.");
        return false;
    }
    uint8_t rkey[16], rnonce[12];
    v1_derive_key(enc_rand, rkey);
    v1_derive_nonce(CMD_COMMCMD, rtid, enc_rand, rnonce);
    uint8_t raad[4] = {
        (uint8_t)(CMD_COMMCMD & 0xFF), (uint8_t)((CMD_COMMCMD >> 8) & 0xFF),
        (uint8_t)(rtid & 0xFF), (uint8_t)((rtid >> 8) & 0xFF)
    };
    uint8_t rpt[TX_BUF_LEN];
    if (resp_ct_len > sizeof(rpt)) return false;
    if (!v1_decrypt(resp_ct, resp_ct_len, rkey, rnonce, raad, sizeof(raad), resp_tag, rpt)) {
        Serial.println("[HS] CommCmd: GCM auth failure.");
        return false;
    }
    (void)rpt;  // decrypt output unused; GCM tag verification is the point
    // The device's CommCmd reply decodes as CommandReqDTO, which carries no
    // err_code in this proto (err_code lives on CommandResDTO, which we SEND).
    // GCM auth + cmd/tid match already prove this is an authentic response to
    // our exact request — far stronger than the old "any frame = success".
    return true;
}

bool handshake_run(const char *sn, uint16_t *tid, uint8_t enc_rand_out[16]) {
    // Register callback first
    ble_set_rx_callback(rx_handler);

    // Try to load encRand from NVS; fall back to V0 pairing
    if (!load_enc_rand_from_nvs(enc_rand_out)) {
        Serial.println("[HS] No encRand in NVS, running V0 pairing...");
        if (!do_v0_pairing(sn, tid, enc_rand_out)) {
            Serial.println("[HS] V0 pairing failed");
            return false;
        }
        Serial.println("[HS] V0 pairing OK, encRand saved to NVS");
    } else {
        Serial.println("[HS] encRand loaded from NVS");
    }

    // CommCmd login (action=64)
    Serial.println("[HS] CommCmd login...");
    if (!do_commcmd(sn, tid, enc_rand_out, COMMCMD_LOGIN)) {
        Serial.println("[HS] CommCmd login failed");
        return false;
    }
    delay(200);

    // CommCmd time-sync (action=104)
    Serial.println("[HS] CommCmd time-sync...");
    if (!do_commcmd(sn, tid, enc_rand_out, COMMCMD_TIME_SYNC)) {
        Serial.println("[HS] CommCmd time-sync failed");
        return false;
    }

    Serial.println("[HS] Handshake complete");
    return true;
}

void handshake_clear_nvs(void) {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.remove(NVS_KEY_ENCRAND);
    prefs.end();
    Serial.println("[HS] encRand cleared from NVS");
}
