// Command input: SLCAN commands and the "#..." extension commands from all links.
//
// Every link has its own reader (serial task, TCP task, web server) that posts complete lines to one
// queue; the main task executes them one after another, so command handlers never run in parallel.
// Every "#" command is answered with optional data lines and exactly one final "#ok ..." or "#err ...",
// only on the link (and for the web: only to the browser) it came from.
#pragma once

#include <freertos/FreeRTOS.h>
#include <stddef.h>
#include <stdint.h>

constexpr size_t CMD_LINE_MAX = 320;

void cmd_init();

// Any task: queue one line from `source` (SINK_*); client = browser id for SINK_WEB, else 0.
// Returns false if the queue stayed full for `wait` ticks.
bool cmd_post(uint8_t source, uint32_t client, const char* line, TickType_t wait);

// Main task: executes the next queued command (waits up to `wait` ticks). False if there was none.
bool cmd_process(TickType_t wait);

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
    void reset() {
        len_ = 0;
        overflow_ = false;
    }

private:
    char buf_[CMD_LINE_MAX];
    size_t len_ = 0;
    bool overflow_ = false;
};
