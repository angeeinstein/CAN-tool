// Rule engine: condition (ID/mask + masked data prefix [+ DLC]) -> one or more delayed frames.
// Pure logic, no timing / RTOS. The firmware glue (src/rules.cpp) schedules the actions.
//
// Rule syntax (everything after "#rule add <name>"):
//   match=<id>[/<mask>][:<bytes>]   bytes: "11 00 27", "110027", "??" = any byte, "1?" = nibble wildcard
//   send=<id>:<bytes>[@<ms>]        bytes may contain {b0}..{b7} = copy byte from the trigger frame
//   dlc=<n>                         optional: trigger frame must have exactly this DLC
//   once                            optional: fire only once (until re-enabled / re-added)
// Several send= actions are allowed; each has its own delay (0..5000 ms, relative to the trigger).
#pragma once

#include "can_frame.h"

constexpr int RULE_MAX = 16;
constexpr int RULE_MAX_ACTIONS = 6;
constexpr int RULE_NAME_LEN = 16;   // incl. NUL
constexpr int RULE_SRC_LEN = 224;   // normalized definition text incl. NUL
constexpr uint16_t RULE_MAX_DELAY_MS = 5000;

struct RuleAction {
    CanFrame frame;          // template; literal bytes
    int8_t src_byte[8];      // -1 = literal, 0..7 = copy that byte of the trigger frame
    uint16_t delay_ms = 0;
};

struct Rule {
    char name[RULE_NAME_LEN] = {0};
    char src[RULE_SRC_LEN] = {0};   // normalized definition (without name), for list/save
    uint32_t id = 0;
    uint32_t id_mask = CAN_STD_ID_MAX;
    bool ext = false;
    uint8_t match_len = 0;
    uint8_t match_data[8] = {0};
    uint8_t match_mask[8] = {0};
    int8_t dlc = -1;                // -1 = any
    RuleAction actions[RULE_MAX_ACTIONS];
    uint8_t n_actions = 0;
    bool once = false;
    bool enabled = true;
    bool spent = false;             // once-rule has fired
    uint32_t hits = 0;
};

// Parses a rule definition. On error returns false and writes a message into err.
bool rule_parse(const char* name, const char* spec, Rule& out, char* err, size_t errcap);

// True if the frame satisfies the rule's condition (ignores enabled/spent).
bool rule_matches(const Rule& r, const CanFrame& f);

// Builds the concrete frame for an action, substituting {bN} placeholders from the trigger.
void rule_build_frame(const RuleAction& a, const CanFrame& trigger, CanFrame& out);

// Callback for each action of each fired rule.
typedef void (*RuleEmitFn)(void* ctx, const Rule& rule, const RuleAction& action, const CanFrame& frame);

class RuleSet {
public:
    // "line" = "<name> <spec...>". Replaces an existing rule with the same name.
    bool add(const char* line, char* err, size_t errcap);
    bool remove(const char* name);
    Rule* find(const char* name);
    void clear() { count_ = 0; }
    int count() const { return count_; }
    int enabled_count() const;
    Rule& at(int i) { return rules_[i]; }
    const Rule& at(int i) const { return rules_[i]; }
    void set_all_enabled(bool en);
    bool set_enabled(const char* name, bool en);

    // Evaluates all enabled rules for a received frame. For every rule that fires,
    // hits is incremented and emit() is called once per action. Returns number of rules fired.
    int process(const CanFrame& f, RuleEmitFn emit, void* ctx);

private:
    Rule rules_[RULE_MAX];
    int count_ = 0;
};

// Sliding-window rate limiter: at most `limit` events within any 1000 ms window.
class RateLimiter {
public:
    static constexpr uint16_t MAX_LIMIT = 500;
    void set_limit(uint16_t per_sec);
    uint16_t limit() const { return limit_; }
    // Returns true and records the event if allowed; false if it would exceed the limit.
    bool allow(uint32_t now_ms);
    void reset() { head_ = 0; count_ = 0; }

private:
    uint32_t ring_[MAX_LIMIT] = {0};
    uint16_t limit_ = 50;
    uint16_t head_ = 0;   // index of oldest entry
    uint16_t count_ = 0;
};
