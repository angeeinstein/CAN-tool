#include "rule_engine.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace {

void set_err(char* err, size_t cap, const char* msg, const char* tok = nullptr) {
    if (!err || cap == 0) return;
    if (tok)
        snprintf(err, cap, "%s: '%s'", msg, tok);
    else
        snprintf(err, cap, "%s", msg);
}

bool valid_name(const char* n) {
    size_t len = strlen(n);
    if (len == 0 || len >= static_cast<size_t>(RULE_NAME_LEN)) return false;
    for (size_t i = 0; i < len; i++) {
        char c = n[i];
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    }
    return true;
}

// Parses "<id>[/<mask>]" from s, stopping at ':' '@' or end. Advances s.
bool parse_id_mask(const char*& s, uint32_t& id, uint32_t& mask, bool& ext, bool allow_mask) {
    size_t n = parse_hex_prefix(s, id, 8);
    if (n == 0) return false;
    ext = n > 3 || id > CAN_STD_ID_MAX;
    if (id > CAN_EXT_ID_MAX) return false;
    s += n;
    mask = ext ? CAN_EXT_ID_MAX : CAN_STD_ID_MAX;
    if (*s == '/') {
        if (!allow_mask) return false;
        s++;
        size_t m = parse_hex_prefix(s, mask, 8);
        if (m == 0) return false;
        s += m;
    }
    return true;
}

// Portable whitespace tokenizer (strtok_r is not available everywhere).
char* next_token(char*& p) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return nullptr;
    char* start = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = '\0';
    return start;
}

int digit_or_wild(char c) {  // 0..15, 16 = '?', -1 = invalid
    if (c == '?') return 16;
    return hex_nibble(c);
}

// Adds match bytes from token text [s, end).
bool add_match_bytes(Rule& r, const char* s, char* err, size_t cap, const char* tok) {
    while (*s) {
        if (*s == '.' || *s == ',') {
            s++;
            continue;
        }
        int hi = digit_or_wild(s[0]);
        int lo = s[0] ? digit_or_wild(s[1]) : -1;
        if (hi < 0 || lo < 0) {
            set_err(err, cap, "bad match byte", tok);
            return false;
        }
        if (r.match_len >= 8) {
            set_err(err, cap, "match has more than 8 bytes");
            return false;
        }
        uint8_t v = 0, m = 0;
        if (hi != 16) { v |= static_cast<uint8_t>(hi << 4); m |= 0xF0; }
        if (lo != 16) { v |= static_cast<uint8_t>(lo); m |= 0x0F; }
        r.match_data[r.match_len] = v;
        r.match_mask[r.match_len] = m;
        r.match_len++;
        s += 2;
    }
    return true;
}

bool parse_delay(const char* s, uint16_t& out, char* err, size_t cap, const char* tok) {
    if (!*s) {
        set_err(err, cap, "missing delay after '@'", tok);
        return false;
    }
    uint32_t v = 0;
    while (*s) {
        if (*s < '0' || *s > '9') {
            set_err(err, cap, "bad delay", tok);
            return false;
        }
        v = v * 10 + static_cast<uint32_t>(*s - '0');
        if (v > RULE_MAX_DELAY_MS) {
            set_err(err, cap, "delay > 5000 ms", tok);
            return false;
        }
        s++;
    }
    out = static_cast<uint16_t>(v);
    return true;
}

// Adds send bytes / placeholders / @delay from token text.
bool add_send_bytes(RuleAction& a, bool& delay_set, const char* s, char* err, size_t cap, const char* tok) {
    while (*s) {
        if (*s == '.' || *s == ',') {
            s++;
            continue;
        }
        if (*s == '@') {
            if (delay_set) {
                set_err(err, cap, "delay given twice", tok);
                return false;
            }
            delay_set = true;
            return parse_delay(s + 1, a.delay_ms, err, cap, tok);
        }
        if (delay_set) {
            set_err(err, cap, "bytes after delay", tok);
            return false;
        }
        if (a.frame.dlc >= 8) {
            set_err(err, cap, "send has more than 8 bytes");
            return false;
        }
        if (s[0] == '{') {
            if ((s[1] == 'b' || s[1] == 'B') && s[2] >= '0' && s[2] <= '7' && s[3] == '}') {
                a.src_byte[a.frame.dlc] = static_cast<int8_t>(s[2] - '0');
                a.frame.data[a.frame.dlc] = 0;
                a.frame.dlc++;
                s += 4;
                continue;
            }
            set_err(err, cap, "bad placeholder (use {b0}..{b7})", tok);
            return false;
        }
        int hi = hex_nibble(s[0]);
        int lo = hex_nibble(s[1]);
        if (hi < 0 || lo < 0) {
            set_err(err, cap, "bad send byte", tok);
            return false;
        }
        a.src_byte[a.frame.dlc] = -1;
        a.frame.data[a.frame.dlc] = static_cast<uint8_t>((hi << 4) | lo);
        a.frame.dlc++;
        s += 2;
    }
    return true;
}

}  // namespace

bool rule_parse(const char* name, const char* spec, Rule& out, char* err, size_t errcap) {
    if (!name || !valid_name(name)) {
        set_err(err, errcap, "invalid rule name (1-15 chars: A-Z a-z 0-9 _ -)");
        return false;
    }
    if (!spec) spec = "";

    char buf[RULE_SRC_LEN * 2];
    if (strlen(spec) >= sizeof(buf)) {
        set_err(err, errcap, "rule too long");
        return false;
    }
    strcpy(buf, spec);

    Rule r;
    snprintf(r.name, sizeof(r.name), "%s", name);
    for (auto& a : r.actions)
        for (auto& b : a.src_byte) b = -1;

    enum Field { NONE, MATCH, SEND } field = NONE;
    bool have_match = false;
    bool delay_set = false;
    size_t src_len = 0;

    char* cursor = buf;
    for (char* tok = next_token(cursor); tok; tok = next_token(cursor)) {
        // normalized source text
        size_t tl = strlen(tok);
        if (src_len + tl + 2 > sizeof(r.src)) {
            set_err(err, errcap, "rule too long");
            return false;
        }
        if (src_len) r.src[src_len++] = ' ';
        memcpy(r.src + src_len, tok, tl);
        src_len += tl;
        r.src[src_len] = '\0';

        if (strncmp(tok, "match=", 6) == 0) {
            if (have_match) {
                set_err(err, errcap, "only one match= allowed");
                return false;
            }
            have_match = true;
            field = MATCH;
            const char* s = tok + 6;
            if (!parse_id_mask(s, r.id, r.id_mask, r.ext, true)) {
                set_err(err, errcap, "bad match id", tok);
                return false;
            }
            if (*s == ':') s++;
            else if (*s) {
                set_err(err, errcap, "expected ':' after id", tok);
                return false;
            }
            if (!add_match_bytes(r, s, err, errcap, tok)) return false;
        } else if (strncmp(tok, "send=", 5) == 0) {
            if (r.n_actions >= RULE_MAX_ACTIONS) {
                set_err(err, errcap, "too many send= actions");
                return false;
            }
            field = SEND;
            delay_set = false;
            RuleAction& a = r.actions[r.n_actions++];
            const char* s = tok + 5;
            uint32_t mask;
            if (!parse_id_mask(s, a.frame.id, mask, a.frame.ext, false)) {
                set_err(err, errcap, "bad send id", tok);
                return false;
            }
            if (*s == ':') s++;
            else if (*s && *s != '@') {
                set_err(err, errcap, "expected ':' after id", tok);
                return false;
            }
            if (!add_send_bytes(a, delay_set, s, err, errcap, tok)) return false;
        } else if (strncmp(tok, "dlc=", 4) == 0) {
            field = NONE;
            const char* s = tok + 4;
            if (s[0] < '0' || s[0] > '8' || s[1]) {
                set_err(err, errcap, "bad dlc", tok);
                return false;
            }
            r.dlc = static_cast<int8_t>(s[0] - '0');
        } else if (strcmp(tok, "once") == 0) {
            field = NONE;
            r.once = true;
        } else if (field == MATCH) {
            if (!add_match_bytes(r, tok, err, errcap, tok)) return false;
        } else if (field == SEND) {
            if (!add_send_bytes(r.actions[r.n_actions - 1], delay_set, tok, err, errcap, tok)) return false;
        } else {
            set_err(err, errcap, "unexpected token", tok);
            return false;
        }
    }

    if (!have_match) {
        set_err(err, errcap, "missing match=");
        return false;
    }
    if (r.n_actions == 0) {
        set_err(err, errcap, "missing send=");
        return false;
    }
    if (r.dlc >= 0 && r.match_len > r.dlc) {
        set_err(err, errcap, "match has more bytes than dlc");
        return false;
    }
    out = r;
    return true;
}

bool rule_matches(const Rule& r, const CanFrame& f) {
    if (f.ext != r.ext) return false;
    if ((f.id & r.id_mask) != (r.id & r.id_mask)) return false;
    if (r.dlc >= 0 && f.dlc != r.dlc) return false;
    if (r.match_len > 0) {
        if (f.rtr || f.dlc < r.match_len) return false;
        for (uint8_t i = 0; i < r.match_len; i++) {
            if ((f.data[i] & r.match_mask[i]) != (r.match_data[i] & r.match_mask[i])) return false;
        }
    }
    return true;
}

void rule_build_frame(const RuleAction& a, const CanFrame& trigger, CanFrame& out) {
    out = a.frame;
    for (uint8_t i = 0; i < out.dlc && i < 8; i++) {
        int8_t src = a.src_byte[i];
        if (src >= 0) out.data[i] = (src < trigger.dlc && !trigger.rtr) ? trigger.data[src] : 0;
    }
}

// ---- RuleSet -----------------------------------------------------------

bool RuleSet::add(const char* line, char* err, size_t errcap) {
    if (!line) line = "";
    while (*line == ' ') line++;
    char name[RULE_NAME_LEN + 1];
    size_t n = 0;
    while (line[n] && line[n] != ' ' && line[n] != '\t') n++;
    if (n == 0 || n >= sizeof(name) - 1) {
        set_err(err, errcap, "invalid rule name (1-15 chars: A-Z a-z 0-9 _ -)");
        return false;
    }
    memcpy(name, line, n);
    name[n] = '\0';

    Rule r;
    if (!rule_parse(name, line + n, r, err, errcap)) return false;

    if (Rule* existing = find(name)) {
        *existing = r;
        return true;
    }
    if (count_ >= RULE_MAX) {
        set_err(err, errcap, "rule table full");
        return false;
    }
    rules_[count_++] = r;
    return true;
}

bool RuleSet::remove(const char* name) {
    for (int i = 0; i < count_; i++) {
        if (strcmp(rules_[i].name, name) == 0) {
            for (int j = i; j < count_ - 1; j++) rules_[j] = rules_[j + 1];
            count_--;
            return true;
        }
    }
    return false;
}

Rule* RuleSet::find(const char* name) {
    for (int i = 0; i < count_; i++)
        if (strcmp(rules_[i].name, name) == 0) return &rules_[i];
    return nullptr;
}

int RuleSet::enabled_count() const {
    int n = 0;
    for (int i = 0; i < count_; i++)
        if (rules_[i].enabled) n++;
    return n;
}

void RuleSet::set_all_enabled(bool en) {
    for (int i = 0; i < count_; i++) {
        rules_[i].enabled = en;
        if (en) rules_[i].spent = false;
    }
}

bool RuleSet::set_enabled(const char* name, bool en) {
    Rule* r = find(name);
    if (!r) return false;
    r->enabled = en;
    if (en) r->spent = false;  // re-enabling re-arms a "once" rule
    return true;
}

int RuleSet::process(const CanFrame& f, RuleEmitFn emit, void* ctx) {
    int fired = 0;
    for (int i = 0; i < count_; i++) {
        Rule& r = rules_[i];
        if (!r.enabled || r.spent) continue;
        if (!rule_matches(r, f)) continue;
        r.hits++;
        if (r.once) r.spent = true;
        fired++;
        for (uint8_t k = 0; k < r.n_actions; k++) {
            CanFrame out;
            rule_build_frame(r.actions[k], f, out);
            if (emit) emit(ctx, r, r.actions[k], out);
        }
    }
    return fired;
}

// ---- RateLimiter -------------------------------------------------------

void RateLimiter::set_limit(uint16_t per_sec) {
    if (per_sec < 1) per_sec = 1;
    if (per_sec > MAX_LIMIT) per_sec = MAX_LIMIT;
    limit_ = per_sec;
    reset();
}

bool RateLimiter::allow(uint32_t now_ms) {
    // drop entries older than the 1 s window
    while (count_ > 0 && static_cast<uint32_t>(now_ms - ring_[head_]) >= 1000u) {
        head_ = static_cast<uint16_t>((head_ + 1) % MAX_LIMIT);
        count_--;
    }
    if (count_ >= limit_) return false;
    ring_[(head_ + count_) % MAX_LIMIT] = now_ms;
    count_++;
    return true;
}
