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
#include <esp_task_wdt.h>

#define CMD_REAL  0xA311

// ---------------------------------------------------------------------------
// RX state
// ---------------------------------------------------------------------------

static volatile uint8_t  s_rx_buf[2048];
static volatile size_t   s_rx_len = 0;
static volatile bool     s_rx_ready = false;

static void rx_handler(const uint8_t *data, size_t len) {
    if (s_rx_ready) return;
    if (s_rx_len + len <= sizeof(s_rx_buf)) {
        memcpy((uint8_t *)s_rx_buf + s_rx_len, data, len);
        s_rx_len += len;
    }
    // Only signal complete when the full V1 frame has arrived.
    // RealDataNew is always V1 (cmd=0xA311), so tag_len=16 always.
    if (s_rx_len >= HM_HEADER_LEN) {
        uint16_t wire_len = ((uint16_t)s_rx_buf[8] << 8) | s_rx_buf[9];
        size_t expected = (size_t)wire_len + 16;  // V1 always has 16-byte tag
        if (s_rx_len >= expected) {
            s_rx_ready = true;
        }
    }
}

// Arm RX accumulation. MUST be called before ble_write() so a fast response
// notification arriving before wait_rx() runs cannot be dropped.
static void rx_reset(void) {
    s_rx_len = 0;
    s_rx_ready = false;
}

static bool wait_rx(uint32_t timeout_ms) {
    uint32_t start = millis();
    while (!s_rx_ready) {
        if (millis() - start > timeout_ms) return false;
        esp_task_wdt_reset();
        delay(5);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Request sender
// ---------------------------------------------------------------------------

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
    aad[0] = CMD_REAL & 0xFF;          // 0x11
    aad[1] = (CMD_REAL >> 8) & 0xFF;   // 0xA3
    aad[2] = *tid & 0xFF;
    aad[3] = (*tid >> 8) & 0xFF;

    uint8_t ct[256], tag[16];
    if (!v1_encrypt(pb_buf, pb_len, key, nonce, aad, 4, ct, tag)) return false;

    uint8_t frame[512];
    size_t frame_len = frame_build(frame, sizeof(frame), CMD_REAL, *tid, ct, pb_len, tag);
    (*tid)++;

    return ble_write(frame, frame_len);
}

// ---------------------------------------------------------------------------
// Response decoder
// ---------------------------------------------------------------------------

static bool decode_page(const uint8_t *buf, size_t buf_len,
                        const uint8_t enc_rand[16], uint16_t expected_tid,
                        RealDataNewReqDTO *out) {
    uint16_t cmd, resp_tid, rcrc, rlen;
    if (!frame_parse_header(buf, buf_len, &cmd, &resp_tid, &rcrc, &rlen)) return false;

    // Use the frame's declared length, not the total bytes accumulated in the
    // RX buffer. Trailing notification bytes must not be folded into the
    // ciphertext (that would shift the tag pointer and break CRC/GCM).
    // Frame layout: HM_HEADER_LEN header + ciphertext + 16-byte GCM tag, where
    // rlen = HM_HEADER_LEN + ct_len.
    if (rlen < HM_HEADER_LEN || (size_t)rlen + 16 > buf_len) {
        Serial.println("[PL] Bad frame length.");
        return false;
    }
    if (cmd != CMD_REAL) {
        Serial.printf("[PL] Unexpected cmd 0x%04X\n", cmd);
        return false;
    }
    // The device is expected to echo our request tid. If a particular inverter
    // uses independent response tids, relax this check — the log makes it obvious.
    if (resp_tid != expected_tid) {
        Serial.printf("[PL] tid mismatch (req=%u resp=%u)\n", expected_tid, resp_tid);
        return false;
    }
    const uint8_t *ct = frame_payload(buf, buf_len);
    size_t ct_len = (size_t)rlen - HM_HEADER_LEN;  // ciphertext only, no tag
    if (ct_len == 0) return false;
    const uint8_t *tag = buf + rlen;

    // CRC validation (over ciphertext only, no tag)
    if (crc16_modbus(ct, ct_len) != rcrc) {
        Serial.println("[PL] CRC mismatch.");
        return false;
    }

    uint8_t key[16], nonce[12];
    v1_derive_key(enc_rand, key);
    v1_derive_nonce(CMD_REAL, resp_tid, enc_rand, nonce);
    uint8_t aad[4];
    aad[0] = CMD_REAL & 0xFF;
    aad[1] = (CMD_REAL >> 8) & 0xFF;
    aad[2] = resp_tid & 0xFF;
    aad[3] = (resp_tid >> 8) & 0xFF;

    uint8_t pt[2048];
    if (ct_len > sizeof(pt)) return false;
    if (!v1_decrypt(ct, ct_len, key, nonce, aad, 4, tag, pt)) {
        Serial.println("[PL] GCM auth tag failure.");
        return false;
    }

    pb_istream_t istream = pb_istream_from_buffer(pt, ct_len);
    if (!pb_decode(&istream, RealDataNewReqDTO_fields, out)) {
        Serial.println("[PL] Proto decode failed.");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// MQTT helpers
// ---------------------------------------------------------------------------

static const char *s_base_topic = "";

static void publish_float(PubSubClient &mqtt, const char *subtopic, float value) {
    char topic[128], payload[32];
    snprintf(topic, sizeof(topic), "%s%s", s_base_topic, subtopic);
    snprintf(payload, sizeof(payload), "%.3f", value);
    mqtt.publish(topic, payload, true);
}

static void publish_str(PubSubClient &mqtt, const char *subtopic, const char *value) {
    char topic[128];
    snprintf(topic, sizeof(topic), "%s%s", s_base_topic, subtopic);
    mqtt.publish(topic, value, true);
}

// ---------------------------------------------------------------------------
// Main poll entry-point
// ---------------------------------------------------------------------------

bool poller_poll(PubSubClient &mqtt, const char *base_topic,
                 uint16_t *tid, const uint8_t enc_rand[16]) {
    s_base_topic = base_topic;
    ble_set_rx_callback(rx_handler);

    // Send first page request (arm RX before the write to avoid a race)
    uint16_t req_tid = *tid;
    rx_reset();
    if (!send_real_req(tid, enc_rand, 0)) {
        Serial.println("[PL] Send request failed.");
        return false;
    }
    if (!wait_rx(RESPONSE_TIMEOUT_MS)) {
        Serial.println("[PL] Timeout.");
        return false;
    }

    uint16_t cmd, resp_tid, rcrc, rlen;
    if (!frame_parse_header((const uint8_t *)s_rx_buf, s_rx_len, &cmd, &resp_tid, &rcrc, &rlen)) {
        Serial.println("[PL] Frame parse failed.");
        return false;
    }

    RealDataNewReqDTO combined = RealDataNewReqDTO_init_zero;
    if (!decode_page((const uint8_t *)s_rx_buf, s_rx_len, enc_rand, req_tid, &combined)) {
        return false;
    }

    // Fetch additional pages if ap > 1
    // Clamp device-reported page count to a sane max; a corrupt/large ap must
    // not spin many 5s request/response cycles (watchdog risk).
    int pages = combined.ap;
    if (pages > 8) pages = 8;
    for (int cp = 1; cp < pages; cp++) {
        // A paged response is all-or-nothing: any failure mid-paging aborts the
        // whole poll so we never publish a partial merge (spec: discard partial).
        rx_reset();
        uint16_t page_tid = *tid;
        if (!send_real_req(tid, enc_rand, cp)) return false;
        if (!wait_rx(RESPONSE_TIMEOUT_MS)) return false;

        if (!frame_parse_header((const uint8_t *)s_rx_buf, s_rx_len, &cmd, &resp_tid, &rcrc, &rlen)) return false;

        RealDataNewReqDTO page = RealDataNewReqDTO_init_zero;
        if (!decode_page((const uint8_t *)s_rx_buf, s_rx_len, enc_rand, page_tid, &page)) return false;

        // Merge pv_data from this page
        for (pb_size_t i = 0; i < page.pv_data_count; i++) {
            if (combined.pv_data_count < 4) {
                combined.pv_data[combined.pv_data_count++] = page.pv_data[i];
            }
        }
    }

    // Publish AC output
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

    // Publish per-panel DC
    float total_energy_kWh = 0.0f;
    for (pb_size_t i = 0; i < combined.pv_data_count; i++) {
        PvMO &pv = combined.pv_data[i];
        char base[32], subtopic[64];
        snprintf(base, sizeof(base), "pv/%d/", (int)pv.port_number);

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
