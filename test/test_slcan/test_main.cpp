// Unit tests for the SLCAN codec (run: pio test -e native)
#include <unity.h>

#include <cstring>

#include "slcan_codec.h"

void setUp() {}
void tearDown() {}

static void test_parse_std_frame() {
    CanFrame f;
    TEST_ASSERT_TRUE(slcan_parse_frame("t4813110027", f));
    TEST_ASSERT_EQUAL_HEX32(0x481, f.id);
    TEST_ASSERT_EQUAL(3, f.dlc);
    TEST_ASSERT_FALSE(f.ext);
    TEST_ASSERT_FALSE(f.rtr);
    TEST_ASSERT_EQUAL_HEX8(0x11, f.data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, f.data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x27, f.data[2]);
}

static void test_parse_lowercase_and_dlc0() {
    CanFrame f;
    TEST_ASSERT_TRUE(slcan_parse_frame("t4c68000156010200ab14", f));
    TEST_ASSERT_EQUAL_HEX32(0x4C6, f.id);
    TEST_ASSERT_EQUAL(8, f.dlc);
    TEST_ASSERT_EQUAL_HEX8(0xAB, f.data[6]);
    TEST_ASSERT_TRUE(slcan_parse_frame("t1810", f));
    TEST_ASSERT_EQUAL(0, f.dlc);
}

static void test_parse_ext_and_rtr() {
    CanFrame f;
    TEST_ASSERT_TRUE(slcan_parse_frame("T1ABCDEF02BEEF", f));
    TEST_ASSERT_TRUE(f.ext);
    TEST_ASSERT_EQUAL_HEX32(0x1ABCDEF0, f.id);
    TEST_ASSERT_EQUAL(2, f.dlc);
    TEST_ASSERT_TRUE(slcan_parse_frame("r1232", f));
    TEST_ASSERT_TRUE(f.rtr);
    TEST_ASSERT_EQUAL(2, f.dlc);
    TEST_ASSERT_TRUE(slcan_parse_frame("R000001234", f));
    TEST_ASSERT_TRUE(f.rtr && f.ext);
}

static void test_parse_errors() {
    CanFrame f;
    TEST_ASSERT_FALSE(slcan_parse_frame("", f));
    TEST_ASSERT_FALSE(slcan_parse_frame("t48", f));          // too short
    TEST_ASSERT_FALSE(slcan_parse_frame("t481311002", f));   // data too short
    TEST_ASSERT_FALSE(slcan_parse_frame("t48131100277", f)); // data too long
    TEST_ASSERT_FALSE(slcan_parse_frame("t4819", f));        // dlc 9
    TEST_ASSERT_FALSE(slcan_parse_frame("t8001", f));        // id > 0x7FF ... "800" ok? -> 0x800 > 0x7FF
    TEST_ASSERT_FALSE(slcan_parse_frame("t48G0", f));        // bad hex
    TEST_ASSERT_FALSE(slcan_parse_frame("T2000000000", f));  // ext id > 29 bit
    TEST_ASSERT_FALSE(slcan_parse_frame("x4810", f));
}

static void test_format_roundtrip() {
    CanFrame f;
    f.id = 0x181;
    f.dlc = 3;
    f.data[2] = 0x01;
    char buf[40];
    TEST_ASSERT_EQUAL(11, slcan_format_frame(f, false, 0, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("t1813000001", buf);
    slcan_format_frame(f, true, 0xEA5F, buf, sizeof(buf));   // 59999 -> wraps to range
    TEST_ASSERT_EQUAL_STRING("t1813000001EA5F", buf);
    slcan_format_frame(f, true, 60001, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("t18130000010001", buf);

    CanFrame g;
    f.dlc = 8;
    for (int i = 0; i < 8; i++) f.data[i] = static_cast<uint8_t>(i * 0x11);
    slcan_format_frame(f, false, 0, buf, sizeof(buf));
    TEST_ASSERT_TRUE(slcan_parse_frame(buf, g));
    TEST_ASSERT_EQUAL_MEMORY(f.data, g.data, 8);
    TEST_ASSERT_EQUAL_HEX32(f.id, g.id);

    f.ext = true;
    f.id = 0x18DAF110;
    f.dlc = 1;
    slcan_format_frame(f, false, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("T18DAF110100", buf);
}

static void test_text_format() {
    CanFrame f;
    f.id = 0x481;
    f.dlc = 3;
    f.data[0] = 0x11;
    f.data[1] = 0x00;
    f.data[2] = 0x27;
    char buf[96];
    text_format_frame("rx", 123456789ULL, f, nullptr, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("#rx 123456789 481 3 11 00 27", buf);
    text_format_frame("tx", 5, f, "poll", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("#tx 5 481 3 11 00 27 poll", buf);
    f.dlc = 0;
    text_format_frame("rx", 1, f, nullptr, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("#rx 1 481 0", buf);
}

static void test_bitrate_codes() {
    TEST_ASSERT_EQUAL_UINT32(125000, slcan_bitrate_from_code('4'));
    TEST_ASSERT_EQUAL_UINT32(10000, slcan_bitrate_from_code('0'));
    TEST_ASSERT_EQUAL_UINT32(1000000, slcan_bitrate_from_code('8'));
    TEST_ASSERT_EQUAL_UINT32(0, slcan_bitrate_from_code('9'));
}

static void test_parse_frame_text() {
    CanFrame f;
    TEST_ASSERT_TRUE(parse_frame_text("481#124655", f));
    TEST_ASSERT_EQUAL_HEX32(0x481, f.id);
    TEST_ASSERT_EQUAL(3, f.dlc);
    TEST_ASSERT_EQUAL_HEX8(0x55, f.data[2]);
    TEST_ASSERT_TRUE(parse_frame_text("400 00 01 09", f));
    TEST_ASSERT_EQUAL_HEX32(0x400, f.id);
    TEST_ASSERT_EQUAL(3, f.dlc);
    TEST_ASSERT_EQUAL_HEX8(0x09, f.data[2]);
    TEST_ASSERT_TRUE(parse_frame_text("480:00 01 28", f));
    TEST_ASSERT_EQUAL(3, f.dlc);
    TEST_ASSERT_TRUE(parse_frame_text("181#", f));
    TEST_ASSERT_EQUAL(0, f.dlc);
    TEST_ASSERT_TRUE(parse_frame_text("123#R", f));
    TEST_ASSERT_TRUE(f.rtr);
    TEST_ASSERT_TRUE(parse_frame_text("18DAF110#0102", f));
    TEST_ASSERT_TRUE(f.ext);
    TEST_ASSERT_FALSE(parse_frame_text("481#1", f));                    // half byte
    TEST_ASSERT_FALSE(parse_frame_text("481#112233445566778899", f));   // 9 bytes
    TEST_ASSERT_FALSE(parse_frame_text("xyz", f));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_std_frame);
    RUN_TEST(test_parse_lowercase_and_dlc0);
    RUN_TEST(test_parse_ext_and_rtr);
    RUN_TEST(test_parse_errors);
    RUN_TEST(test_format_roundtrip);
    RUN_TEST(test_text_format);
    RUN_TEST(test_bitrate_codes);
    RUN_TEST(test_parse_frame_text);
    return UNITY_END();
}
