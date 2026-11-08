// Output path to the hosts (USB serial, TCP client, browsers).
//
// Frames and asynchronous notifications are put into queues without blocking and written by a
// dedicated output task, so slow USB/TCP/WLAN can never stall CAN reception. Lost entries are counted.
//
// Line endings: SLCAN frames end with "\r" (standard). "#..." lines end with "\n\r":
// terminals get a proper new line, and SLCAN parsers that split on '\r' see "#...\n"
// (ignored) followed by a clean next frame.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "can_frame.h"

enum class OutFormat : uint8_t { Slcan, Text };

// Host links. Frames and notifications go to all; command replies only to the requesting link.
// The web UI (WebSocket) always receives the text format, independent of the format setting.
enum : uint8_t { SINK_SERIAL = 1, SINK_TCP = 2, SINK_WEB = 4, SINK_ALL = 7 };

void host_init();

// Where host_reply()/host_reply_raw() go: the link (and browser) of the command being executed.
// Set by the command dispatcher (main task).
void host_set_reply_target(uint8_t sink, uint32_t web_client);
uint8_t host_reply_sink();
uint32_t host_reply_client();   // browser id if the command came from the web, else 0

// Non-blocking, callable from any task.
void host_emit_rx(const CanFrame& f, uint64_t ts_us);
void host_emit_tx(const CanFrame& f, uint64_t ts_us, const char* reason);
void host_notify(const char* fmt, ...) __attribute__((format(printf, 1, 2)));  // "#..." line

// For command replies (main task). May wait for a slow link, never loses data while it is connected.
void host_reply(const char* fmt, ...) __attribute__((format(printf, 1, 2)));   // "#..." line
void host_reply_raw(const char* s);                                            // SLCAN reply, as is

// Replay buffer: the last REPLAY_FRAMES frames and REPLAY_MSGS notifications are kept in RAM.
// host_replay() sends everything after since_us to the requesting link (main task, "#replay").
void host_replay(uint64_t since_us);
// The frame ring (REPLAY_FRAMES x 32 B) can be released temporarily when the heap is needed elsewhere
// (WPA2-Enterprise login). A later #replay reports that time as "#replay gap". restore() returns false
// while the memory is not available yet - call it again.
void host_replay_release();
bool host_replay_restore();

// Output settings
void host_set_format(OutFormat f);
OutFormat host_format();
void host_set_slcan_timestamps(bool on);
bool host_slcan_timestamps();
void host_set_echo(bool on);
bool host_echo();

struct HostStats {
    uint32_t frames_dropped;   // frame queue full
    uint32_t msgs_dropped;     // notification queue full
};
void host_get_stats(HostStats& s);
void host_reset_stats();
