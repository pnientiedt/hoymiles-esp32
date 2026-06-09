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
    TEST_ASSERT_EQUAL_HEX16(0xA201, ((uint16_t)buf[2] << 8) | buf[3]);
    TEST_ASSERT_EQUAL_HEX16(1,      ((uint16_t)buf[4] << 8) | buf[5]);
    TEST_ASSERT_EQUAL_HEX16(0x2BA1, ((uint16_t)buf[6] << 8) | buf[7]);
    TEST_ASSERT_EQUAL_HEX16(14, ((uint16_t)buf[8] << 8) | buf[9]);
    TEST_ASSERT_EQUAL_MEMORY(ct, buf + 10, 4);
}

void test_frame_build_v1_with_tag(void) {
    uint8_t ct[]  = {0xAA, 0xBB, 0xCC};
    uint8_t tag[16];
    memset(tag, 0x55, 16);
    uint8_t buf[128];
    size_t len = frame_build(buf, sizeof(buf), 0xA311, 2, ct, 3, tag);

    TEST_ASSERT_EQUAL(10 + 3 + 16, len);
    uint16_t expected_crc = crc16_modbus(ct, 3);
    TEST_ASSERT_EQUAL_HEX16(expected_crc, ((uint16_t)buf[6] << 8) | buf[7]);
    TEST_ASSERT_EQUAL_HEX16(13, ((uint16_t)buf[8] << 8) | buf[9]);
    TEST_ASSERT_EQUAL_MEMORY(tag, buf + 10 + 3, 16);
}

void test_frame_build_buf_too_small(void) {
    uint8_t ct[] = {0x01};
    uint8_t buf[5];
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
    TEST_ASSERT_EQUAL_HEX16(0xC19B, crc);
    TEST_ASSERT_EQUAL_HEX16(14, length);
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
    TEST_ASSERT_EQUAL_HEX16(15, len);
    const uint8_t *p = frame_payload(buf, n);
    TEST_ASSERT_EQUAL_HEX16(crc16_modbus(ct, 5), crc);
    TEST_ASSERT_EQUAL_MEMORY(ct, p, 5);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_frame_build_v0_no_tag);
    RUN_TEST(test_frame_build_v1_with_tag);
    RUN_TEST(test_frame_build_buf_too_small);
    RUN_TEST(test_frame_parse_header_valid);
    RUN_TEST(test_frame_parse_header_bad_magic);
    RUN_TEST(test_frame_parse_header_too_short);
    RUN_TEST(test_frame_payload_offset_and_guard);
    RUN_TEST(test_frame_roundtrip_crc);
    return UNITY_END();
}
