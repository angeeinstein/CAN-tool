// Unit tests for the rule engine and rate limiter (run: pio test -e native)
#include <unity.h>

#include <cstring>

#include "rule_engine.h"
#include "slcan_codec.h"

void setUp() {}
void tearDown() {}

static CanFrame frame(const char* text) {
    CanFrame f;
    TEST_ASSERT_TRUE_MESSAGE(parse_frame_text(text, f), text);
    return f;
}

struct Emitted {
    CanFrame frames[16];
    uint16_t delays[16];
    char names[16][RULE_NAME_LEN];
    int n = 0;
};

static void collect(void* ctx, const Rule& r, const RuleAction& a, const CanFrame& f) {
    Emitted* e = static_cast<Emitted*>(ctx);
    if (e->n >= 16) return;
    e->frames[e->n] = f;
    e->delays[e->n] = a.delay_ms;
    strcpy(e->names[e->n], r.name);
    e->n++;
}

static void test_parse_spec_example() {
    Rule r;
    char err[96] = "";
    TEST_ASSERT_TRUE_MESSAGE(
        rule_parse("poll", "match=481:11 00 27  send=400:00 01 09@1  send=480:00 01 28@3", r, err, sizeof(err)), err);
    TEST_ASSERT_EQUAL_STRING("poll", r.name);
    TEST_ASSERT_EQUAL_HEX32(0x481, r.id);
    TEST_ASSERT_EQUAL_HEX32(0x7FF, r.id_mask);
    TEST_ASSERT_EQUAL(3, r.match_len);
    TEST_ASSERT_EQUAL_HEX8(0x27, r.match_data[2]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, r.match_mask[2]);
    TEST_ASSERT_EQUAL(2, r.n_actions);
    TEST_ASSERT_EQUAL_HEX32(0x400, r.actions[0].frame.id);
    TEST_ASSERT_EQUAL(3, r.actions[0].frame.dlc);
    TEST_ASSERT_EQUAL(1, r.actions[0].delay_ms);
    TEST_ASSERT_EQUAL_HEX32(0x480, r.actions[1].frame.id);
    TEST_ASSERT_EQUAL_HEX8(0x28, r.actions[1].frame.data[2]);
    TEST_ASSERT_EQUAL(3, r.actions[1].delay_ms);
    TEST_ASSERT_FALSE(r.once);
    // normalized source (single spaces)
    TEST_ASSERT_EQUAL_STRING("match=481:11 00 27 send=400:00 01 09@1 send=480:00 01 28@3", r.src);
}

static void test_parse_variants() {
    Rule r;
    char err[96] = "";
    // compact bytes, wildcards, mask, dlc, once, placeholders, separate @delay token
    TEST_ASSERT_TRUE_MESSAGE(rule_parse("x", "match=480/7F0:12??5? dlc=8 send=4C6:00 01 {b2} {b7} @250 once", r,
                                        err, sizeof(err)),
                             err);
    TEST_ASSERT_EQUAL_HEX32(0x7F0, r.id_mask);
    TEST_ASSERT_EQUAL(3, r.match_len);
    TEST_ASSERT_EQUAL_HEX8(0x00, r.match_mask[1]);
    TEST_ASSERT_EQUAL_HEX8(0xF0, r.match_mask[2]);
    TEST_ASSERT_EQUAL(8, r.dlc);
    TEST_ASSERT_TRUE(r.once);
    TEST_ASSERT_EQUAL(250, r.actions[0].delay_ms);
    TEST_ASSERT_EQUAL(4, r.actions[0].frame.dlc);
    TEST_ASSERT_EQUAL(2, r.actions[0].src_byte[2]);
    TEST_ASSERT_EQUAL(7, r.actions[0].src_byte[3]);
    TEST_ASSERT_EQUAL(-1, r.actions[0].src_byte[0]);

    // match without data, empty send with delay only
    TEST_ASSERT_TRUE_MESSAGE(rule_parse("y", "match=181 send=400:@10", r, err, sizeof(err)), err);
    TEST_ASSERT_EQUAL(0, r.match_len);
    TEST_ASSERT_EQUAL(0, r.actions[0].frame.dlc);
    TEST_ASSERT_EQUAL(10, r.actions[0].delay_ms);
}

static void test_parse_errors() {
    Rule r;
    char err[96];
    TEST_ASSERT_FALSE(rule_parse("a", "send=400:00", r, err, sizeof(err)));          // no match
    TEST_ASSERT_FALSE(rule_parse("a", "match=481:11", r, err, sizeof(err)));         // no send
    TEST_ASSERT_FALSE(rule_parse("a", "match=481:1 send=400:00", r, err, sizeof(err)));  // half byte
    TEST_ASSERT_FALSE(rule_parse("a", "match=481:11 send=400:00@6000", r, err, sizeof(err)));  // delay
    TEST_ASSERT_FALSE(rule_parse("a", "match=481:11 send=400:00@1 22", r, err, sizeof(err)));  // bytes after delay
    TEST_ASSERT_FALSE(rule_parse("a", "match=481:11 send=400:{b8}", r, err, sizeof(err)));     // placeholder
    TEST_ASSERT_FALSE(rule_parse("a", "match=481:11 send=400:00 01 02 03 04 05 06 07 08", r, err, sizeof(err)));
    TEST_ASSERT_FALSE(rule_parse("a", "match=481:11 junk send=400:00", r, err, sizeof(err)));  // not hex
    TEST_ASSERT_FALSE(rule_parse("a", "foo match=481 send=400", r, err, sizeof(err)));       // token before field
    TEST_ASSERT_FALSE(rule_parse("bad name!", "match=481 send=400", r, err, sizeof(err)));
    TEST_ASSERT_FALSE(rule_parse("a", "match=481:11 22 33 dlc=2 send=400", r, err, sizeof(err)));
}

static void test_matching() {
    Rule r;
    char err[96];
    TEST_ASSERT_TRUE(rule_parse("p", "match=481:11 00 27 send=400:00", r, err, sizeof(err)));
    TEST_ASSERT_TRUE(rule_matches(r, frame("481#110027")));
    TEST_ASSERT_TRUE(rule_matches(r, frame("481#110027AABB")));   // prefix match
    TEST_ASSERT_FALSE(rule_matches(r, frame("481#110021")));
    TEST_ASSERT_FALSE(rule_matches(r, frame("481#1100")));        // too short
    TEST_ASSERT_FALSE(rule_matches(r, frame("482#110027")));
    TEST_ASSERT_FALSE(rule_matches(r, frame("00000481#110027"))); // extended id differs

    TEST_ASSERT_TRUE(rule_parse("w", "match=480/7F0:12 ?? 5? send=400:00", r, err, sizeof(err)));
    TEST_ASSERT_TRUE(rule_matches(r, frame("481#124655")));
    TEST_ASSERT_TRUE(rule_matches(r, frame("48F#12FF57")));
    TEST_ASSERT_FALSE(rule_matches(r, frame("491#124655")));
    TEST_ASSERT_FALSE(rule_matches(r, frame("481#124665")));

    TEST_ASSERT_TRUE(rule_parse("d", "match=4C6:00 01 dlc=8 send=400:00", r, err, sizeof(err)));
    TEST_ASSERT_TRUE(rule_matches(r, frame("4C6#0001560100020015")));
    TEST_ASSERT_FALSE(rule_matches(r, frame("4C6#000156")));

    TEST_ASSERT_TRUE(rule_parse("any", "match=181 send=400:00", r, err, sizeof(err)));
    TEST_ASSERT_TRUE(rule_matches(r, frame("181#")));
    TEST_ASSERT_TRUE(rule_matches(r, frame("181#000003")));
}

static void test_placeholders() {
    Rule r;
    char err[96];
    TEST_ASSERT_TRUE(rule_parse("echo", "match=481:12 46 send=4C6:00 01 {b2} {b5} {b7}", r, err, sizeof(err)));
    CanFrame out;
    rule_build_frame(r.actions[0], frame("481#124657010164"), out);   // b7 does not exist -> 0
    TEST_ASSERT_EQUAL(5, out.dlc);
    TEST_ASSERT_EQUAL_HEX8(0x00, out.data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, out.data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x57, out.data[2]);
    TEST_ASSERT_EQUAL_HEX8(0x64, out.data[3]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out.data[4]);
}

static void test_ruleset_process_once_and_replace() {
    RuleSet rs;
    char err[96];
    TEST_ASSERT_TRUE(rs.add("poll match=481:11 00 27 send=400:00 01 09@1 send=480:00 01 28@3", err, sizeof(err)));
    TEST_ASSERT_TRUE(rs.add("anfr match=481:11 00 21 send=400:00 01 09@1 send=480:00 01 22@3 once", err, sizeof(err)));
    TEST_ASSERT_EQUAL(2, rs.count());

    Emitted e;
    TEST_ASSERT_EQUAL(1, rs.process(frame("481#110027"), collect, &e));
    TEST_ASSERT_EQUAL(2, e.n);
    TEST_ASSERT_EQUAL_STRING("poll", e.names[0]);
    TEST_ASSERT_EQUAL_HEX32(0x400, e.frames[0].id);
    TEST_ASSERT_EQUAL(3, e.delays[1]);

    // once-rule fires only one time
    e.n = 0;
    TEST_ASSERT_EQUAL(1, rs.process(frame("481#110021"), collect, &e));
    TEST_ASSERT_EQUAL(0, rs.process(frame("481#110021"), collect, &e));
    TEST_ASSERT_EQUAL(2, e.n);
    TEST_ASSERT_EQUAL(1u, rs.find("anfr")->hits);
    // re-enable re-arms it
    TEST_ASSERT_TRUE(rs.set_enabled("anfr", true));
    TEST_ASSERT_EQUAL(1, rs.process(frame("481#110021"), collect, &e));

    // disable
    TEST_ASSERT_TRUE(rs.set_enabled("poll", false));
    TEST_ASSERT_EQUAL(0, rs.process(frame("481#110027"), collect, &e));
    TEST_ASSERT_EQUAL(1, rs.enabled_count());

    // re-add with same name replaces (and is enabled again)
    TEST_ASSERT_TRUE(rs.add("poll match=481:11 00 27 send=400:00 01 09", err, sizeof(err)));
    TEST_ASSERT_EQUAL(2, rs.count());
    TEST_ASSERT_EQUAL(1, rs.find("poll")->n_actions);
    TEST_ASSERT_TRUE(rs.find("poll")->enabled);

    // emergency stop semantics
    rs.set_all_enabled(false);
    TEST_ASSERT_EQUAL(0, rs.process(frame("481#110027"), collect, &e));

    TEST_ASSERT_TRUE(rs.remove("poll"));
    TEST_ASSERT_FALSE(rs.remove("poll"));
    TEST_ASSERT_EQUAL(1, rs.count());
    rs.clear();
    TEST_ASSERT_EQUAL(0, rs.count());
}

static void test_ruleset_full() {
    RuleSet rs;
    char err[96];
    char line[64];
    for (int i = 0; i < RULE_MAX; i++) {
        snprintf(line, sizeof(line), "r%d match=%03X send=400:00", i, 0x100 + i);
        TEST_ASSERT_TRUE(rs.add(line, err, sizeof(err)));
    }
    TEST_ASSERT_FALSE(rs.add("extra match=181 send=400:00", err, sizeof(err)));
    TEST_ASSERT_TRUE(rs.add("r3 match=181 send=400:00", err, sizeof(err)));   // replace still works
}

static void test_rate_limiter() {
    RateLimiter rl;
    rl.set_limit(3);
    TEST_ASSERT_TRUE(rl.allow(1000));
    TEST_ASSERT_TRUE(rl.allow(1100));
    TEST_ASSERT_TRUE(rl.allow(1200));
    TEST_ASSERT_FALSE(rl.allow(1300));    // 4th within 1 s
    TEST_ASSERT_FALSE(rl.allow(1999));
    TEST_ASSERT_TRUE(rl.allow(2000));     // first one left the window
    TEST_ASSERT_FALSE(rl.allow(2050));
    TEST_ASSERT_TRUE(rl.allow(5000));
    // millis() wrap-around
    rl.set_limit(2);
    TEST_ASSERT_TRUE(rl.allow(0xFFFFFF00u));
    TEST_ASSERT_TRUE(rl.allow(0xFFFFFFF0u));
    TEST_ASSERT_FALSE(rl.allow(0x00000010u));
    TEST_ASSERT_TRUE(rl.allow(0x00000400u));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_spec_example);
    RUN_TEST(test_parse_variants);
    RUN_TEST(test_parse_errors);
    RUN_TEST(test_matching);
    RUN_TEST(test_placeholders);
    RUN_TEST(test_ruleset_process_once_and_replace);
    RUN_TEST(test_ruleset_full);
    RUN_TEST(test_rate_limiter);
    return UNITY_END();
}
