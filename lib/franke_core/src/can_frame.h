// Plain CAN frame type shared by firmware and native unit tests.
// No Arduino / ESP-IDF dependencies in this library.
#pragma once

#include <cstddef>
#include <cstdint>

struct CanFrame {
    uint32_t id = 0;      // 11-bit (standard) or 29-bit (extended)
    uint8_t dlc = 0;      // 0..8
    bool ext = false;     // extended (29-bit) identifier
    bool rtr = false;     // remote transmission request
    uint8_t data[8] = {0, 0, 0, 0, 0, 0, 0, 0};
};

constexpr uint32_t CAN_STD_ID_MAX = 0x7FF;
constexpr uint32_t CAN_EXT_ID_MAX = 0x1FFFFFFF;

// ---- small hex helpers -------------------------------------------------

// Returns 0..15 or -1 if c is not a hex digit.
int hex_nibble(char c);

// Parses exactly n hex characters. Returns false on any non-hex char or n == 0 / n > 8.
bool parse_hex_n(const char* s, size_t n, uint32_t& out);

// Parses a hex string up to the first non-hex char. Returns number of chars consumed (0 = error).
size_t parse_hex_prefix(const char* s, uint32_t& out, size_t max_chars = 8);

// Writes two uppercase hex chars, no terminator.
void put_hex2(char* out, uint8_t v);
