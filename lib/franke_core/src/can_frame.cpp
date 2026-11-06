#include "can_frame.h"

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_hex_n(const char* s, size_t n, uint32_t& out) {
    if (n == 0 || n > 8) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) {
        int d = hex_nibble(s[i]);
        if (d < 0) return false;
        v = (v << 4) | static_cast<uint32_t>(d);
    }
    out = v;
    return true;
}

size_t parse_hex_prefix(const char* s, uint32_t& out, size_t max_chars) {
    uint32_t v = 0;
    size_t i = 0;
    while (i < max_chars && s[i]) {
        int d = hex_nibble(s[i]);
        if (d < 0) break;
        v = (v << 4) | static_cast<uint32_t>(d);
        i++;
    }
    if (i > 0) out = v;
    return i;
}

void put_hex2(char* out, uint8_t v) {
    static const char digits[] = "0123456789ABCDEF";
    out[0] = digits[v >> 4];
    out[1] = digits[v & 0x0F];
}
