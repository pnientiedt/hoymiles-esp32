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
// CommCmd application-layer handshake (V1, encRand-keyed). The DTU replies on
// (sent_cmd - 0x0100), e.g. 0xA318 -> 0xA218, just like 0xA301 -> 0xA201.
#define CMD_COMMCMD_SEND   0xA318   // CommCmdResDTO        (app -> DTU)
#define CMD_COMMCMD_REPLY  0xA218   // CommCmdReqDTO        (DTU -> app)
#define CMD_COMMSTS_SEND   0xA319   // CommCmdStatusResDTO  (app -> DTU)
#define CMD_COMMSTS_REPLY  0xA219   // CommCmdStatusReqDTO  (DTU -> app)
#define NVS_NS            "hoymiles"
#define NVS_KEY_ENCRAND   "enc_rand"
#define RX_BUF_LEN        1024
#define TX_BUF_LEN        512
#define COMMCMD_LOGIN     64
#define COMMCMD_PIN       82
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
        // Only the V0 pairing reply (0xA201) is plaintext-CBC with no tag; every
        // V1 reply (CommCmd 0xA218/0xA219, RealData 0xA211, ...) appends a
        // 16-byte GCM tag.
        size_t tag_len = (cmd == CMD_APP_INFO_REPLY) ? 0 : 16;
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

// ---- Minimal protobuf encoders (match the hiflow-ble reference exactly) ----
// The BLE CommCmd message types aren't covered by our compiled nanopb structs
// (and the committed CommCmd.proto has the wrong field numbers), so we hand-roll
// the few fields we need — identical to the Python reference's _build_* helpers.

static size_t pb_varint(uint8_t *out, uint32_t n) {
    size_t i = 0;
    while (n > 0x7F) { out[i++] = (uint8_t)((n & 0x7F) | 0x80); n >>= 7; }
    out[i++] = (uint8_t)(n & 0x7F);
    return i;
}
static size_t pb_field_varint(uint8_t *out, uint8_t field, uint32_t n) {
    size_t i = 0;
    out[i++] = (uint8_t)((field << 3) | 0);  // wire type 0
    i += pb_varint(out + i, n);
    return i;
}
static size_t pb_field_string(uint8_t *out, uint8_t field, const char *s) {
    size_t slen = strlen(s);
    size_t i = 0;
    out[i++] = (uint8_t)((field << 3) | 2);  // wire type 2
    i += pb_varint(out + i, (uint32_t)slen);
    memcpy(out + i, s, slen);
    return i + slen;
}

// CommCmdResDTO: field 1=time, 2=action, 5=tid, 6=data
static size_t build_commcmd_res(uint8_t *out, int32_t action, const char *data) {
    uint32_t t = (uint32_t)time(nullptr);
    size_t i = 0;
    i += pb_field_varint(out + i, 1, t);
    i += pb_field_varint(out + i, 2, (uint32_t)action);
    i += pb_field_varint(out + i, 5, t);
    i += pb_field_string(out + i, 6, data);
    return i;
}
// CommCmdStatusResDTO: field 1=time, 2=action, 4=tid
static size_t build_commcmd_status_res(uint8_t *out, int32_t action) {
    uint32_t t = (uint32_t)time(nullptr);
    size_t i = 0;
    i += pb_field_varint(out + i, 1, t);
    i += pb_field_varint(out + i, 2, (uint32_t)action);
    i += pb_field_varint(out + i, 4, t);
    return i;
}

// Walk a flat protobuf message for a single varint field (returns false if
// absent). Good enough for the CommCmdStatusReqDTO reply (action=3, sts=11).
static bool pb_get_varint_field(const uint8_t *pt, size_t len, uint8_t want_field,
                                uint32_t *out) {
    size_t off = 0;
    while (off < len) {
        uint32_t tag = 0; int shift = 0;
        while (off < len) { uint8_t b = pt[off++]; tag |= (uint32_t)(b & 0x7F) << shift;
                            if (!(b & 0x80)) break; shift += 7; }
        uint8_t field = tag >> 3, wire = tag & 7;
        if (wire == 0) {
            uint32_t v = 0; shift = 0;
            while (off < len) { uint8_t b = pt[off++]; v |= (uint32_t)(b & 0x7F) << shift;
                                if (!(b & 0x80)) break; shift += 7; }
            if (field == want_field) { *out = v; return true; }
        } else if (wire == 2) {
            uint32_t n = 0; shift = 0;
            while (off < len) { uint8_t b = pt[off++]; n |= (uint32_t)(b & 0x7F) << shift;
                                if (!(b & 0x80)) break; shift += 7; }
            off += n;
        } else if (wire == 1) { off += 8; }
        else if (wire == 5) { off += 4; }
        else return false;
    }
    return false;
}

// Send one V1 (encRand-keyed GCM) CommCmd frame and await its reply. On success
// copies the decrypted reply plaintext into pt_out and returns its length; 0 on
// any transport/crypto failure. send_cmd is 0xA318 or 0xA319; the DTU replies on
// send_cmd - 0x0100.
static size_t commcmd_request(uint16_t *tid, const uint8_t enc_rand[16],
                              uint16_t send_cmd, const uint8_t *pb_buf, size_t pb_len,
                              uint8_t *pt_out, size_t pt_out_cap) {
    uint16_t req_tid = *tid;
    uint16_t reply_cmd = send_cmd - 0x0100;

    uint8_t key[16], nonce[12], tag[16];
    v1_derive_key(enc_rand, key);
    v1_derive_nonce(send_cmd, *tid, enc_rand, nonce);
    uint8_t aad[4] = { (uint8_t)(send_cmd & 0xFF), (uint8_t)((send_cmd >> 8) & 0xFF),
                       (uint8_t)(*tid & 0xFF), (uint8_t)((*tid >> 8) & 0xFF) };

    uint8_t ct[TX_BUF_LEN];
    if (!v1_encrypt(pb_buf, pb_len, key, nonce, aad, sizeof(aad), ct, tag)) {
        Serial.println("[HS] CommCmd: v1_encrypt failed");
        return 0;
    }
    uint8_t frame[TX_BUF_LEN + 64];
    size_t frame_len = frame_build(frame, sizeof(frame), send_cmd, *tid, ct, pb_len, tag);
    (*tid)++;
    if (frame_len == 0) { Serial.println("[HS] CommCmd: frame_build failed"); return 0; }

    ble_set_rx_callback(rx_handler);
    rx_reset();
    if (!ble_write(frame, frame_len)) { Serial.println("[HS] CommCmd: ble_write failed"); return 0; }
    if (!wait_for_rx(RESPONSE_TIMEOUT_MS)) {
        Serial.printf("[HS] CommCmd cmd=0x%04X: timeout\n", send_cmd);
        return 0;
    }

    uint16_t rcmd, rtid, rcrc, rlen;
    if (!frame_parse_header((const uint8_t *)s_rx_buf, s_rx_len, &rcmd, &rtid, &rcrc, &rlen)) {
        Serial.println("[HS] CommCmd: parse header failed"); return 0;
    }
    if (rcmd != reply_cmd) Serial.printf("[HS] CommCmd: cmd 0x%04X (expected 0x%04X)\n", rcmd, reply_cmd);
    if (rlen < HM_HEADER_LEN || (size_t)rlen + 16 > s_rx_len) {
        Serial.println("[HS] CommCmd: bad frame length"); return 0;
    }
    size_t resp_ct_len = (size_t)rlen - HM_HEADER_LEN;
    if (resp_ct_len == 0 || resp_ct_len > pt_out_cap) return 0;
    const uint8_t *resp_ct = frame_payload((const uint8_t *)s_rx_buf, s_rx_len);
    const uint8_t *resp_tag = (const uint8_t *)s_rx_buf + rlen;
    if (crc16_modbus(resp_ct, resp_ct_len) != rcrc) {
        Serial.println("[HS] CommCmd: CRC mismatch"); return 0;
    }
    uint8_t rkey[16], rnonce[12];
    v1_derive_key(enc_rand, rkey);
    v1_derive_nonce(rcmd, rtid, enc_rand, rnonce);
    uint8_t raad[4] = { (uint8_t)(rcmd & 0xFF), (uint8_t)((rcmd >> 8) & 0xFF),
                        (uint8_t)(rtid & 0xFF), (uint8_t)((rtid >> 8) & 0xFF) };
    if (!v1_decrypt(resp_ct, resp_ct_len, rkey, rnonce, raad, sizeof(raad), resp_tag, pt_out)) {
        Serial.println("[HS] CommCmd: GCM auth failure"); return 0;
    }
    (void)req_tid;
    return resp_ct_len;
}

// Run the CommCmd application-layer handshake (reference async_do_comm_cmd_handshake):
//   action=64 login (data=bleId) -> poll status; if sts=3, action=82 PIN -> poll;
//   then action=104 time-sync -> poll. Returns true once time-sync is sent OK.
static bool do_commcmd_handshake(uint16_t *tid, const uint8_t enc_rand[16],
                                 const char *ble_id, const char *pin) {
    uint8_t pb[TX_BUF_LEN], pt[TX_BUF_LEN];
    uint32_t action, sts;
    int login_sts = -1;

    // ---- Step 1: action=64 login (data = bleId) ----
    Serial.printf("[HS] CommCmd login (action=64, bleId=%s)\n", ble_id);
    size_t n = build_commcmd_res(pb, COMMCMD_LOGIN, ble_id);
    if (commcmd_request(tid, enc_rand, CMD_COMMCMD_SEND, pb, n, pt, sizeof(pt)) == 0) {
        Serial.println("[HS] CommCmd: login send failed (dormant or encRand stale)");
    } else {
        for (int i = 0; i < 5; i++) {
            n = build_commcmd_status_res(pb, COMMCMD_LOGIN);
            size_t rn = commcmd_request(tid, enc_rand, CMD_COMMSTS_SEND, pb, n, pt, sizeof(pt));
            if (rn == 0) break;
            sts = 0; pb_get_varint_field(pt, rn, 11, &sts);
            action = 0; pb_get_varint_field(pt, rn, 3, &action);
            Serial.printf("[HS] login poll: action=%u sts=%u\n", action, sts);
            if (sts == 0) { delay(1000); esp_task_wdt_reset(); continue; }  // in-progress
            login_sts = (int)sts;
            break;
        }
    }

    // ---- Step 2: action=82 PIN if bleId not whitelisted (sts=3) ----
    if (login_sts == 3) {
        if (!pin || pin[0] == '\0') {
            Serial.println("[HS] CommCmd: device wants a PIN (sts=3) but BLE_PIN is empty. "
                           "Set BLE_PIN in config.h to the S-Miles app PIN.");
        } else {
            Serial.println("[HS] CommCmd: sts=3 -> sending PIN (action=82)");
            n = build_commcmd_res(pb, COMMCMD_PIN, pin);
            if (commcmd_request(tid, enc_rand, CMD_COMMCMD_SEND, pb, n, pt, sizeof(pt)) != 0) {
                for (int i = 0; i < 8; i++) {
                    n = build_commcmd_status_res(pb, COMMCMD_PIN);
                    size_t rn = commcmd_request(tid, enc_rand, CMD_COMMSTS_SEND, pb, n, pt, sizeof(pt));
                    if (rn == 0) break;
                    sts = 1; pb_get_varint_field(pt, rn, 11, &sts);
                    Serial.printf("[HS] PIN poll: sts=%u\n", sts);
                    if (sts == 0) { login_sts = 1; break; }       // success
                    if (sts == 1) { Serial.println("[HS] CommCmd: WRONG PIN"); break; }
                    delay(1000); esp_task_wdt_reset();
                }
            }
        }
    }

    // ---- Step 3: action=104 time-sync (always; reference does it regardless) ----
    char time_data[40];
    snprintf(time_data, sizeof(time_data), "%lu,%d\r", (unsigned long)time(nullptr), 28800);
    Serial.println("[HS] CommCmd time-sync (action=104)");
    n = build_commcmd_res(pb, COMMCMD_TIME_SYNC, time_data);
    if (commcmd_request(tid, enc_rand, CMD_COMMCMD_SEND, pb, n, pt, sizeof(pt)) == 0) {
        Serial.println("[HS] CommCmd: time-sync send failed");
        return false;
    }
    n = build_commcmd_status_res(pb, COMMCMD_TIME_SYNC);
    size_t rn = commcmd_request(tid, enc_rand, CMD_COMMSTS_SEND, pb, n, pt, sizeof(pt));
    if (rn) { sts = 0; pb_get_varint_field(pt, rn, 11, &sts);
              Serial.printf("[HS] time-sync poll: sts=%u\n", sts); }

    Serial.printf("[HS] CommCmd handshake done (login_sts=%d)\n", login_sts);
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

    // CommCmd application-layer handshake: required after every BLE (re)connect
    // before the DTU will answer V1 data requests (RealDataNew). Empirically
    // confirmed: without it, RealDataNew times out.
    if (!do_commcmd_handshake(tid, enc_rand_out, BLE_ID, BLE_PIN)) {
        Serial.println("[HS] CommCmd handshake failed");
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
