// SLCAN (Lawicel) frame encoding/decoding plus the "#rx/#tx" text format.
#pragma once

#include "can_frame.h"

// Bitrate for an SLCAN "Sn" code ('0'..'8'). Returns 0 for an invalid code.
//   S0=10k S1=20k S2=50k S3=100k S4=125k S5=250k S6=500k S7=800k S8=1M
uint32_t slcan_bitrate_from_code(char code);

// Parses an SLCAN transmit command: tIIILDD.., TIIIIIIIILDD.., rIIIL, RIIIIIIIIL.
// The string must contain nothing else (no trailing \r). Returns false on any error.
bool slcan_parse_frame(const char* line, CanFrame& out);

// Formats a received frame in SLCAN notation (without the trailing \r).
// If with_ts is set, the 4-hex-digit millisecond timestamp (0..59999) is appended.
// Returns the length written (excluding NUL), or 0 if cap is too small.
size_t slcan_format_frame(const CanFrame& f, bool with_ts, uint16_t ts_ms, char* out, size_t cap);

// Formats "#<tag> <us> <id> <dlc> <b0 b1 ..>[ <reason>]" (without line terminator).
// RTR frames print "R" instead of data. Returns length, or 0 if cap is too small.
size_t text_format_frame(const char* tag, uint64_t us, const CanFrame& f, const char* reason,
                         char* out, size_t cap);

// Parses a human-friendly frame description used by "#send" and the host tool:
//   "481#112233"   (candump style, "481#R" = RTR)
//   "481 11 22 33" / "481:11 22 33" / "481:112233"
// IDs with more than 3 hex digits or a value > 0x7FF are extended.
bool parse_frame_text(const char* s, CanFrame& out);
