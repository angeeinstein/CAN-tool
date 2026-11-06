#include "slcan_codec.h"

#include <cstdio>
#include <cstring>

uint32_t slcan_bitrate_from_code(char code) {
    switch (code) {
        case '0': return 10000;
        case '1': return 20000;
        case '2': return 50000;
        case '3': return 100000;
        case '4': return 125000;
        case '5': return 250000;
        case '6': return 500000;
        case '7': return 800000;
        case '8': return 1000000;
        default: return 0;
    }
}

bool slcan_parse_frame(const char* line, CanFrame& out) {
    if (!line || !line[0]) return false;
    const char type = line[0];
    const bool ext = (type == 'T' || type == 'R');
    const bool rtr = (type == 'r' || type == 'R');
    if (type != 't' && type != 'T' && type != 'r' && type != 'R') return false;

    const size_t id_len = ext ? 8 : 3;
    const size_t len = strlen(line);
    if (len < 1 + id_len + 1) return false;

    uint32_t id;
    if (!parse_hex_n(line + 1, id_len, id)) return false;
    if (id > (ext ? CAN_EXT_ID_MAX : CAN_STD_ID_MAX)) return false;

    const char dlc_c = line[1 + id_len];
    if (dlc_c < '0' || dlc_c > '8') return false;
    const uint8_t dlc = static_cast<uint8_t>(dlc_c - '0');

    const size_t data_pos = 2 + id_len;
    const size_t expected = data_pos + (rtr ? 0 : dlc * 2u);
    if (len != expected) return false;

    CanFrame f;
    f.id = id;
    f.ext = ext;
    f.rtr = rtr;
    f.dlc = dlc;
    if (!rtr) {
        for (uint8_t i = 0; i < dlc; i++) {
            uint32_t b;
            if (!parse_hex_n(line + data_pos + i * 2u, 2, b)) return false;
            f.data[i] = static_cast<uint8_t>(b);
        }
    }
    out = f;
    return true;
}

size_t slcan_format_frame(const CanFrame& f, bool with_ts, uint16_t ts_ms, char* out, size_t cap) {
    // worst case: T + 8 id + 1 dlc + 16 data + 4 ts + NUL = 31
    if (cap < 32) return 0;
    size_t p = 0;
    if (f.ext) {
        out[p++] = f.rtr ? 'R' : 'T';
        p += static_cast<size_t>(snprintf(out + p, cap - p, "%08lX", static_cast<unsigned long>(f.id & CAN_EXT_ID_MAX)));
    } else {
        out[p++] = f.rtr ? 'r' : 't';
        p += static_cast<size_t>(snprintf(out + p, cap - p, "%03lX", static_cast<unsigned long>(f.id & CAN_STD_ID_MAX)));
    }
    const uint8_t dlc = f.dlc > 8 ? 8 : f.dlc;
    out[p++] = static_cast<char>('0' + dlc);
    if (!f.rtr) {
        for (uint8_t i = 0; i < dlc; i++) {
            put_hex2(out + p, f.data[i]);
            p += 2;
        }
    }
    if (with_ts) {
        p += static_cast<size_t>(snprintf(out + p, cap - p, "%04X", static_cast<unsigned>(ts_ms % 60000u)));
    }
    out[p] = '\0';
    return p;
}

size_t text_format_frame(const char* tag, uint64_t us, const CanFrame& f, const char* reason,
                         char* out, size_t cap) {
    int n = snprintf(out, cap, f.ext ? "#%s %llu %08lX %u" : "#%s %llu %03lX %u", tag,
                     static_cast<unsigned long long>(us), static_cast<unsigned long>(f.id),
                     static_cast<unsigned>(f.dlc));
    if (n < 0 || static_cast<size_t>(n) >= cap) return 0;
    size_t p = static_cast<size_t>(n);
    if (f.rtr) {
        if (p + 3 > cap) return 0;
        out[p++] = ' ';
        out[p++] = 'R';
    } else {
        const uint8_t dlc = f.dlc > 8 ? 8 : f.dlc;
        for (uint8_t i = 0; i < dlc; i++) {
            if (p + 4 > cap) return 0;
            out[p++] = ' ';
            put_hex2(out + p, f.data[i]);
            p += 2;
        }
    }
    if (reason && reason[0]) {
        n = snprintf(out + p, cap - p, " %s", reason);
        if (n < 0 || p + static_cast<size_t>(n) >= cap) return 0;
        p += static_cast<size_t>(n);
    }
    out[p] = '\0';
    return p;
}

bool parse_frame_text(const char* s, CanFrame& out) {
    if (!s) return false;
    while (*s == ' ') s++;

    uint32_t id;
    size_t id_len = parse_hex_prefix(s, id, 8);
    if (id_len == 0) return false;
    const bool ext = id_len > 3 || id > CAN_STD_ID_MAX;
    if (id > CAN_EXT_ID_MAX) return false;
    s += id_len;

    CanFrame f;
    f.id = id;
    f.ext = ext;

    if (*s == '#') {
        s++;
        if (*s == 'R' || *s == 'r') {
            f.rtr = true;
            s++;
            if (*s >= '0' && *s <= '8') f.dlc = static_cast<uint8_t>(*s++ - '0');
            while (*s == ' ') s++;
            if (*s) return false;
            out = f;
            return true;
        }
    } else if (*s == ':' || *s == ' ') {
        s++;
    } else if (*s != '\0') {
        return false;
    }

    // data: hex byte pairs, separators (space, '.', ':') allowed between pairs
    uint8_t n = 0;
    while (*s) {
        if (*s == ' ' || *s == '.' || *s == ':') {
            s++;
            continue;
        }
        if (n >= 8) return false;
        int hi = hex_nibble(s[0]);
        int lo = hex_nibble(s[1]);
        if (hi < 0 || lo < 0) return false;
        f.data[n++] = static_cast<uint8_t>((hi << 4) | lo);
        s += 2;
    }
    f.dlc = n;
    out = f;
    return true;
}
