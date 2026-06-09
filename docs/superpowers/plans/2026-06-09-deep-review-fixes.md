# Deep-Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the findings from the 2026-06-09 deep review of the Hoymiles ESP32 BLE→MQTT bridge: 1 Critical (watchdog), 9 Important, and the Minor/hardening + test-coverage items.

**Requirements source:** the deep-review findings (Critical/Important/Minor + verification risks) already agreed with the user. This plan is the implementation of those findings.

**Tech Stack:** PlatformIO, Arduino (ESP32), NimBLE-Arduino, PubSubClient, nanopb, mbedTLS.

**Verification convention (every task gate):**
- `python3 -m platformio run -e esp32` → `SUCCESS`
- `python3 -m platformio test -e native` → all tests pass (15 baseline; more added in Task 7)

`pio` is NOT on PATH — always use `python3 -m platformio`. `src/secrets.h` exists locally (gitignored); never touch it. Branch: `fix/deep-review-findings`.

---

## Task 1: Watchdog feeds + bounded scan + clamp paging (Critical C1, Important I5)

**Files:** `src/handshake.cpp`, `src/poller.cpp`, `src/ble_client.cpp`

The 30s task WDT is only fed in `main.cpp`. Scan + handshake run in one `loop()` iteration without a feed and can exceed 30s → panic-reset loop. Feed it inside every blocking wait, shorten the scan, and clamp the paging loop.

- [ ] **Step 1: Feed WDT in handshake `wait_for_rx`** (`src/handshake.cpp`)
Add `#include <esp_task_wdt.h>` with the other includes. In the `wait_for_rx` while-loop, add a feed:
```c
static bool wait_for_rx(uint32_t timeout_ms) {
    uint32_t start = millis();
    while (!s_rx_ready) {
        if (millis() - start > timeout_ms) return false;
        esp_task_wdt_reset();
        delay(10);
    }
    return true;
}
```

- [ ] **Step 2: Feed WDT in poller `wait_rx`** (`src/poller.cpp`)
Add `#include <esp_task_wdt.h>`. In the `wait_rx` while-loop add `esp_task_wdt_reset();` before the `delay(5);` (same shape as Step 1).

- [ ] **Step 3: Shorten scan and feed WDT around it** (`src/ble_client.cpp`)
Add `#include <esp_task_wdt.h>`. Change `NimBLEScanResults results = scan->start(10);` to `scan->start(5);`, and add `esp_task_wdt_reset();` immediately after the scan returns and again immediately after `s_client->connect(target_addr)` succeeds.

- [ ] **Step 4: Clamp the paging loop** (`src/poller.cpp`)
In `poller_poll`, replace the page loop header `for (int cp = 1; cp < combined.ap; cp++) {` with a clamped bound:
```c
    // Clamp device-reported page count to a sane max; a corrupt/large ap must
    // not spin many 5s request/response cycles (watchdog risk).
    int pages = combined.ap;
    if (pages > 8) pages = 8;
    for (int cp = 1; cp < pages; cp++) {
```

- [ ] **Step 5: Build + test**
`python3 -m platformio run -e esp32` → SUCCESS; `python3 -m platformio test -e native` → 15 pass.

- [ ] **Step 6: Commit**
`git add src/handshake.cpp src/poller.cpp src/ble_client.cpp && git commit -m "fix: feed watchdog during scan/handshake/poll waits; clamp paging loop"`

---

## Task 2: Response validation + CommCmd success check + length guard (Important I1, I3, I4)

**Files:** `src/poller.cpp`, `src/handshake.cpp`

Validate that responses match the expected command and transaction id, that the declared frame length fits the received bytes, that CommCmd actually succeeded, and reject empty ciphertext.

- [ ] **Step 1: Pass the expected tid into `decode_page` and validate cmd/tid/length/empty** (`src/poller.cpp`)
Change `decode_page`'s signature to take the expected tid:
```c
static bool decode_page(const uint8_t *buf, size_t buf_len,
                        const uint8_t enc_rand[16], uint16_t expected_tid,
                        RealDataNewReqDTO *out) {
```
Right after the existing `frame_parse_header(...)` call and the `rlen` bounds check, add cmd + tid validation, and after computing `ct_len` add an empty-ciphertext guard and an explicit pt-bounds note. The validation block (place immediately after the `rlen < HM_HEADER_LEN || (size_t)rlen + 16 > buf_len` guard):
```c
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
```
After `size_t ct_len = (size_t)rlen - HM_HEADER_LEN;`, add:
```c
    if (ct_len == 0) return false;
```

- [ ] **Step 2: Add explicit pt bounds check before decrypt** (`src/poller.cpp`, in `decode_page`)
Immediately before `if (!v1_decrypt(ct, ct_len, ...)` (after `uint8_t pt[2048];`), add:
```c
    if (ct_len > sizeof(pt)) return false;
```

- [ ] **Step 3: Capture request tid and pass it through in `poller_poll`** (`src/poller.cpp`)
The first page: capture the tid before `send_real_req` (which uses `*tid` then increments), and pass it to `decode_page`:
```c
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
```
Update the first-page decode call to `decode_page((const uint8_t *)s_rx_buf, s_rx_len, enc_rand, req_tid, &combined)`.
In the paging loop, capture `uint16_t page_tid = *tid;` before `send_real_req(tid, enc_rand, cp)`, and update the page decode call to `decode_page((const uint8_t *)s_rx_buf, s_rx_len, enc_rand, page_tid, &page)`.

- [ ] **Step 4: Guard V0 pairing response length + empty ct** (`src/handshake.cpp`, `do_v0_pairing`)
Before the `crc16_modbus(payload, payload_ct_len)` check, ensure the declared frame fits what was received and is non-empty. The existing code computes `payload_ct_len = rlen - HM_HEADER_LEN;` after checking `rlen < HM_HEADER_LEN`. Add, immediately after that `payload_ct_len` assignment:
```c
    if ((size_t)rlen > s_rx_len || payload_ct_len == 0) {
        Serial.println("[HS] V0: declared length exceeds received / empty.");
        return false;
    }
```
Also add a pt bounds guard before `v0_decrypt`: immediately before `size_t pt_len = v0_decrypt(...)`, add `if (payload_ct_len > sizeof(pt)) return false;`.

- [ ] **Step 5: Validate the CommCmd response (cmd + tid + decode + err_code)** (`src/handshake.cpp`, `do_commcmd`)
At the top of `do_commcmd`, capture the request tid before it is incremented: add `uint16_t req_tid = *tid;` as the first statement.
Replace the current response-handling tail (the block that parses the header and does the optional CRC check, ending with `return true;`) with full validation. It must: parse the header; require `rcmd == CMD_COMMCMD` and `rtid == req_tid` (log mismatch); bound-check `(size_t)rlen + 16 <= s_rx_len`; CRC-check the ciphertext; V1-decrypt using a nonce/AAD derived from `rtid`; decode `CommandReqDTO`; and require `err_code == 0` if present. Concretely:
```c
    uint16_t rcmd, rtid, rcrc, rlen;
    if (!frame_parse_header(s_rx_buf, s_rx_len, &rcmd, &rtid, &rcrc, &rlen)) {
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
    const uint8_t *resp_ct = frame_payload(s_rx_buf, s_rx_len);
    const uint8_t *resp_tag = s_rx_buf + rlen;
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
    CommandReqDTO cres = CommandReqDTO_init_default;
    pb_istream_t cstream = pb_istream_from_buffer(rpt, resp_ct_len);
    if (!pb_decode(&cstream, CommandReqDTO_fields, &cres)) {
        Serial.println("[HS] CommCmd: pb_decode failed");
        return false;
    }
    if (cres.has_err_code && cres.err_code != 0) {
        Serial.printf("[HS] CommCmd action=%d rejected, err_code=%d\n",
                      (int)action, (int)cres.err_code);
        return false;
    }
    return true;
```
Note: `CommandReqDTO` has an `err_code` field per `proto/CommCmd.proto`; confirm the field name (`err_code`) in the generated `src/proto/CommCmd.pb.h` and adjust if the generator named it differently.

- [ ] **Step 6: Build + test**
esp32 SUCCESS; native 15 pass.

- [ ] **Step 7: Commit**
`git add src/poller.cpp src/handshake.cpp && git commit -m "fix: validate response cmd/tid/length and CommCmd err_code"`

---

## Task 3: Cross-core RX hardening (Important I2, I6)

**Files:** `src/handshake.cpp`, `src/poller.cpp`

Make the handshake RX buffer `volatile` (consistent with poller), and stop both RX handlers from accumulating after the frame is ready (prevents a late/continuation notification mutating the buffer mid-parse).

- [ ] **Step 1: Make handshake `s_rx_buf` volatile** (`src/handshake.cpp`)
Change `static uint8_t  s_rx_buf[RX_BUF_LEN];` to `static volatile uint8_t  s_rx_buf[RX_BUF_LEN];`. Then fix the resulting qualifier mismatches by casting at each use site (mirror how `poller.cpp` does it):
- in `rx_handler`: `memcpy((uint8_t *)s_rx_buf + s_rx_len, data, len);`
- every `frame_parse_header(s_rx_buf, ...)`, `frame_payload(s_rx_buf, ...)`, `crc16_modbus(payload, ...)` where `payload` came from `s_rx_buf`, and any read passed to `v0_decrypt`/`pb_*`: cast the buffer argument to `(const uint8_t *)s_rx_buf` (or assign through a `const uint8_t *p = (const uint8_t *)s_rx_buf;` local at the top of each function that reads it). Ensure it compiles cleanly with no `-Wcast-qual`/discarded-qualifier errors.

- [ ] **Step 2: Stop accumulating after ready — handshake** (`src/handshake.cpp`)
As the first statement in `rx_handler`, add:
```c
    if (s_rx_ready) return;
```

- [ ] **Step 3: Stop accumulating after ready — poller** (`src/poller.cpp`)
Same: add `if (s_rx_ready) return;` as the first statement in poller's `rx_handler`.

- [ ] **Step 4: Build + test**
esp32 SUCCESS; native 15 pass.

- [ ] **Step 5: Commit**
`git add src/handshake.cpp src/poller.cpp && git commit -m "fix: harden cross-core RX (volatile buffer, stop accumulating after ready)"`

---

## Task 4: MQTT robustness + NTP gating (Important I7, I8)

**Files:** `src/main.cpp`, `src/poller.cpp`

Don't report success when publishes fail; only mark `online` after a successful poll; don't send/publish 1970 timestamps before NTP sync.

- [ ] **Step 1: Bounded NTP wait after WiFi connect** (`src/main.cpp`)
In `wifi_connect`, after `configTime(0, 0, "pool.ntp.org");`, add a bounded, WDT-fed wait so the first handshake/poll uses a real time (best-effort — proceed after timeout):
```c
    configTime(0, 0, "pool.ntp.org");
    uint32_t ntp_start = millis();
    while (time(nullptr) < 1700000000UL && millis() - ntp_start < 8000) {
        esp_task_wdt_reset();
        delay(200);
    }
```

- [ ] **Step 2: Move "online" out of `mqtt_connect`** (`src/main.cpp`)
In `mqtt_connect`, delete the line `s_mqtt.publish(s_status_topic, "online", true);` (keep the firmware_version publish and the LWT). `online` will be published after a successful poll instead.

- [ ] **Step 3: Publish "online" after a successful poll** (`src/main.cpp`)
In `loop()`, in the `else` branch of the poll result (the `s_poll_failures = 0;` path), publish online:
```c
    } else {
        s_poll_failures = 0;
        s_mqtt.publish(s_status_topic, "online", true);
    }
```

- [ ] **Step 4: Check publish results + gate last_seen on valid time** (`src/poller.cpp`)
In `poller_poll`, replace the `last_seen` publish block:
```c
    char ts[16];
    snprintf(ts, sizeof(ts), "%lu", (unsigned long)time(nullptr));
    publish_str(mqtt, "last_seen", ts);
```
with a time-gated, checked publish (treat publish failure as a poll failure so the loop reconnects):
```c
    time_t now = time(nullptr);
    if (now > 1700000000) {
        char ts[16];
        snprintf(ts, sizeof(ts), "%lu", (unsigned long)now);
        if (!publish_str_checked(mqtt, "last_seen", ts)) {
            Serial.println("[PL] MQTT publish failed.");
            return false;
        }
    }
```
Add a checked variant of `publish_str` near the existing helpers (returns the PubSubClient result):
```c
static bool publish_str_checked(PubSubClient &mqtt, const char *subtopic, const char *value) {
    char topic[128];
    snprintf(topic, sizeof(topic), "%s%s", s_base_topic, subtopic);
    return mqtt.publish(topic, value, true);
}
```

- [ ] **Step 5: Build + test**
esp32 SUCCESS; native 15 pass.

- [ ] **Step 6: Commit**
`git add src/main.cpp src/poller.cpp && git commit -m "fix: check MQTT publishes, gate timestamps on NTP, online after first poll"`

---

## Task 5: Small safety fixes (Minor)

**Files:** `src/crypto.cpp`, `src/ble_client.cpp`, `src/ble_client.h`, `src/main.cpp`

- [ ] **Step 1: Zero crypto outputs on the SN-length reject path** (`src/crypto.cpp`)
In `v0_derive_key`, on the `sn_len > ...` reject branch, `memset(key_out, 0, 16); return;` instead of leaving it uninitialized. In `v0_derive_iv`, `memset(iv_out, 0, 16); return;` on its reject branch. (Find the existing length-guard `return;` statements and add the memset before each.)

- [ ] **Step 2: Guard `ble_write` MTU underflow** (`src/ble_client.cpp`)
Replace `uint16_t mtu = s_client->getMTU() - 3;` with:
```c
    uint16_t mtu = s_client->getMTU();
    if (mtu < 23) mtu = 23;   // never underflow if MTU not yet negotiated
    mtu -= 3;
```

- [ ] **Step 3: Remove the dead `rx_cb` parameter from `ble_connect`** (`src/ble_client.h`, `src/ble_client.cpp`, `src/main.cpp`)
The real callback is always set via `ble_set_rx_callback` (handshake/poller call it); `ble_connect` is always passed `nullptr`. Remove the `BleRxCallback rx_cb` parameter from the declaration (`ble_client.h`) and definition (`ble_client.cpp`), and delete the `s_rx_cb.store(rx_cb);` line inside `ble_connect`. Update the call site in `main.cpp` `ble_connect_and_handshake` to `ble_connect(BLE_NAME_PREFIX, INVERTER_SN_FILTER, s_sn, sizeof(s_sn))`.

- [ ] **Step 4: Build + test**
esp32 SUCCESS; native 15 pass.

- [ ] **Step 5: Commit**
`git add src/crypto.cpp src/ble_client.cpp src/ble_client.h src/main.cpp && git commit -m "fix: zero crypto reject output, guard MTU underflow, drop dead ble_connect param"`

---

## Task 6: Build config + repo hygiene (Important I9 + Minor)

**Files:** `platformio.ini`, `.gitignore`, `src/main.cpp`

- [ ] **Step 1: Use the stable Homebrew opt symlink for mbedTLS** (`platformio.ini`)
In `[env:native]` build_flags, replace the versioned Cellar paths:
```
    -I/opt/homebrew/Cellar/mbedtls@3/3.6.6/include
    -L/opt/homebrew/Cellar/mbedtls@3/3.6.6/lib
```
with the version-stable opt symlink:
```
    -I/opt/homebrew/opt/mbedtls@3/include
    -L/opt/homebrew/opt/mbedtls@3/lib
```

- [ ] **Step 2: Pin platform and tighten NimBLE** (`platformio.ini`)
Find the currently-resolved esp32 platform version: run `python3 -m platformio pkg list -e esp32` and read the `espressif32` version. Pin `platform = espressif32@<that exact version>`. Change `h2zero/NimBLE-Arduino@^1.4.2` to `h2zero/NimBLE-Arduino@~1.4.2` (patch-only).

- [ ] **Step 3: Drop redundant MQTT compile flag** (`platformio.ini`, `src/main.cpp`)
Remove `-DMQTT_MAX_PACKET_SIZE=512` from `[env:esp32]` build_flags. In `main.cpp`, add a one-line comment above the `s_mqtt.setBufferSize(MQTT_BUFFER_SIZE);` call noting it is the authoritative buffer control at runtime.

- [ ] **Step 4: Ignore `.claude/`** (`.gitignore`)
Add a line `.claude/` (with a brief comment) so the local agent worktree/tooling dir isn't shown as untracked.

- [ ] **Step 5: Build + test** (this is the key gate — confirms the pins/paths still resolve)
`python3 -m platformio run -e esp32` → SUCCESS; `python3 -m platformio test -e native` → 15 pass.

- [ ] **Step 6: Commit**
`git add platformio.ini .gitignore src/main.cpp && git commit -m "build: stable mbedtls path, pin platform/NimBLE, drop redundant flag, ignore .claude"`

---

## Task 7: Test vectors + document protocol assumptions (verification-risk hardening)

**Files:** `test/test_crypto/test_crypto.cpp`, `test/test_frame/test_frame.cpp`, `src/handshake.cpp`, `src/poller.cpp`

Add the missing-derivation test vectors using an INDEPENDENT oracle (the IV/nonce derivations are pure triple-SHA256, computable from the spec with Python `hashlib` alone — no AES, no dependence on the C code), and document the deliberate Req/Res DTO inversion in code.

- [ ] **Step 1: Generate independent expected vectors with Python**
Run this and capture the two hex strings (it uses only the standard library):
```bash
python3 - <<'PY'
import hashlib, struct
def t(b):
    for _ in range(3): b = hashlib.sha256(b).digest()
    return b
sn = b"AABBCCDDEE12"
# V0 response IV: triple_sha256(BE16(cmd)+BE16(tid)+sn)[16:32], cmd=0xA301 tid=1
iv = t(struct.pack(">HH", 0xA301, 1) + sn)[16:32]
print("v0_iv_resp =", iv.hex())
# CommCmd V1 nonce: triple_sha256(LE16(cmd)+LE16(tid)+enc_rand)[20:32], cmd=0xA305 tid=1
er = bytes(range(16))
nonce = t(struct.pack("<HH", 0xA305, 1) + er)[20:32]
print("commcmd_nonce =", nonce.hex())
PY
```

- [ ] **Step 2: Add the two derivation tests** (`test/test_crypto/test_crypto.cpp`)
Add (substituting the hex printed in Step 1):
```c
void test_v0_derive_iv_response(void) {
    uint8_t iv[16];
    v0_derive_iv(0xA301, 1, "AABBCCDDEE12", iv);
    uint8_t expected[16];
    hex_to_bytes("<v0_iv_resp hex>", expected, 16);
    TEST_ASSERT_EQUAL_MEMORY(expected, iv, 16);
}
void test_v1_derive_nonce_commcmd(void) {
    uint8_t enc_rand[16];
    hex_to_bytes("000102030405060708090a0b0c0d0e0f", enc_rand, 16);
    uint8_t nonce[12];
    v1_derive_nonce(0xA305, 1, enc_rand, nonce);
    uint8_t expected[12];
    hex_to_bytes("<commcmd_nonce hex>", expected, 12);
    TEST_ASSERT_EQUAL_MEMORY(expected, nonce, 12);
}
```
Register both in `main()` with `RUN_TEST(...)`.

- [ ] **Step 3: Add PKCS7 rejection tests** (`test/test_crypto/test_crypto.cpp`)
```c
void test_v0_decrypt_rejects_unaligned(void) {
    uint8_t key[16] = {0}, iv[16] = {0}, ct[17] = {0}, pt[32];
    TEST_ASSERT_EQUAL(0, v0_decrypt(ct, 17, key, iv, pt));  // not a multiple of 16
}
void test_v0_decrypt_rejects_bad_padding(void) {
    // Encrypt 16 bytes, then corrupt the final block so PKCS7 strip must reject.
    uint8_t key[16], iv[16];
    hex_to_bytes("4c3f5d3bbf452b9fe337146ae214b0ee", key, 16);
    hex_to_bytes("31e25c9a5aaf687e3767a3551fd58395", iv, 16);
    uint8_t pt_in[16] = "0123456789abcde";  // 15 chars + NUL = 16 bytes
    uint8_t ct[32];
    size_t cl = v0_encrypt(pt_in, 16, key, iv, ct);
    ct[cl - 1] ^= 0xFF;  // corrupt last ciphertext byte -> bogus padding
    uint8_t pt_out[32];
    TEST_ASSERT_EQUAL(0, v0_decrypt(ct, cl, key, iv, pt_out));
}
```
Register both in `main()`.

- [ ] **Step 4: Add a frame round-trip + frame_payload test** (`test/test_frame/test_frame.cpp`)
```c
void test_frame_payload_offset_and_guard(void) {
    uint8_t ct[] = {0xAA, 0xBB};
    uint8_t buf[64];
    size_t n = frame_build(buf, sizeof(buf), 0xA311, 7, ct, 2, NULL);
    TEST_ASSERT_TRUE(n > 0);
    const uint8_t *p = frame_payload(buf, n);
    TEST_ASSERT_EQUAL_PTR(buf + 10, p);
    TEST_ASSERT_NULL(frame_payload(buf, 5));  // too short -> NULL
}
void test_frame_roundtrip_crc(void) {
    uint8_t ct[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};
    uint8_t buf[64];
    size_t n = frame_build(buf, sizeof(buf), 0xA311, 9, ct, 5, NULL);
    uint16_t cmd, tid, crc, len;
    TEST_ASSERT_TRUE(frame_parse_header(buf, n, &cmd, &tid, &crc, &len));
    TEST_ASSERT_EQUAL_HEX16(0xA311, cmd);
    TEST_ASSERT_EQUAL_HEX16(9, tid);
    TEST_ASSERT_EQUAL_HEX16(15, len);              // 5 + 10
    const uint8_t *p = frame_payload(buf, n);
    TEST_ASSERT_EQUAL_HEX16(crc16_modbus(ct, 5), crc);
    TEST_ASSERT_EQUAL_MEMORY(ct, p, 5);
}
```
Register both in `main()`.

- [ ] **Step 5: Document the Req/Res DTO inversion in code** (`src/handshake.cpp`, `src/poller.cpp`)
Add a brief comment at each site that encodes a `...ResDTO` for a request or decodes a `...ReqDTO` for a response, e.g. above the `RealDataNewResDTO req = ...` in `send_real_req` and above the `APPInfoDataResDTO req = ...` in `do_v0_pairing`:
```c
    // NOTE: the DTU acts as the "server", so the ESP32 SENDS the library's
    // ...ResDTO and DECODES the ...ReqDTO. This inversion is intentional and
    // mirrors the hoymiles-wifi reference. Verify against a device capture.
```

- [ ] **Step 6: Build + test** (now expect 15 + 6 = 21 native test cases)
`python3 -m platformio run -e esp32` → SUCCESS; `python3 -m platformio test -e native` → all pass.

- [ ] **Step 7: Commit**
`git add test/ src/handshake.cpp src/poller.cpp && git commit -m "test: add derivation/PKCS7/frame vectors; document Req/Res inversion"`

---

## Out of scope (cannot be fixed without the real hoymiles-wifi library or a device)

These remain open and require the on-device verification step:
- Proto field numbers/types correctness.
- Scale factors (esp. `power_limit ÷10`).
- Req/Res inversion *direction/field-set* per message (now documented, not verified).
- CommCmd login payload completeness (whether a `bleId`/`data` field is required for action=64).
- Whether the device echoes the request tid (Task 2 logs a mismatch so this is observable).
