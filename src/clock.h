// Wall clock for the device.
//
// All frame timestamps stay "µs since boot" (esp_timer, monotonic, never jumps). This module provides
// the mapping to real time (epoch), so the browser / PC can convert timestamps without network latency:
//   - NTP (pool.ntp.org) as soon as the WLAN has internet; re-synced hourly.
//   - Without NTP a client may set the time ("#time set <epoch_ms>"); NTP overrides it later.
#pragma once

#include <stdint.h>

enum class ClockSource : uint8_t { None, Host, Ntp };

void clock_init();                         // after WiFi init (starts SNTP when WLAN is enabled)
ClockSource clock_source();
const char* clock_source_name();
// Current boot time (µs) and the matching epoch time (µs, 0 if the clock is not set).
void clock_now(uint64_t& boot_us, uint64_t& epoch_us);
uint64_t clock_epoch_of(uint64_t boot_us);  // 0 if the clock is not set
bool clock_set_from_host(uint64_t epoch_ms);  // false if NTP is active (NTP wins)
