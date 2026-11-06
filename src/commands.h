// Command dispatcher for SLCAN commands and the "#..." extension commands.
// Every "#" command is answered with optional data lines and exactly one final "#ok ..." or "#err ..." line.
#pragma once

#include <stddef.h>

#include <stdint.h>

// Handles one complete line (without terminator) received on link `source` (SINK_*).
// Replies go back to that link only. Must be called from the loop task.
void cmd_handle_line(char* line, uint8_t source);

// Collects characters into lines; '\r' and '\n' both terminate a line.
class LineBuffer {
public:
    // Returns true when a complete, non-empty line is available via line().
    bool feed(char c) {
        if (c == '\r' || c == '\n') {
            if (len_ == 0) return false;
            buf_[len_] = '\0';
            len_ = 0;
            if (overflow_) {
                overflow_ = false;
                return false;
            }
            return true;
        }
        if (len_ < sizeof(buf_) - 1) buf_[len_++] = c;
        else overflow_ = true;
        return false;
    }
    char* line() { return buf_; }
    void reset() { len_ = 0; overflow_ = false; }

private:
    char buf_[320];
    size_t len_ = 0;
    bool overflow_ = false;
};
