// Diagnostics: why did the last reset happen, where did it crash, how is the device doing under load,
// and a synthetic CAN flood to reproduce load problems without the machine.
//
// - Reset reason (esp_reset_reason) + boot/crash counters in NVS ("sys").
// - Crash location: ESP-IDF writes a core dump into the "coredump" partition while crashing. After the
//   reboot its summary (task, PC, backtrace) is reported ("#warn crash: ...", "#health") and the dump
//   erased. Decode the addresses with: xtensa-esp32-elf-addr2line -pfiaC -e .pio/build/<env>/firmware.elf <addr...>
// - Every second a small snapshot (uptime, heap, rx rate, drops ...) is written to RTC memory, which
//   survives a crash, watchdog or software reset (not a power cycle), and is reported after such a reset.
// - Every 5 s: "#warn dropped ..." if any frames/data were lost, and "#health ..." while rx is busy
//   (> HEALTH_BUSY_FPS) or a self-test runs. "#health" on demand shows the same plus the boot info.
// - "#selftest flood <fps> <secs>": injects frames ID 7FF (data 10 80 10 02 <seq16> 00 20) into the
//   same output path real frames take (host queue, USB/TCP/web, replay ring). Not on the bus, no rules.
#pragma once

#include <stddef.h>
#include <stdint.h>

void health_init();          // early in app_main: reset reason, crash dump, previous-run snapshot, counters
void health_boot_report();   // after the #boot line: "#info reset ..." / "#warn crash ..." notifications
void health_tick();          // main task, cheap
void health_print();         // "#health" command reply (main task)
const char* health_reset_reason();   // short name of this boot's reset reason

bool selftest_flood_start(uint32_t fps, uint32_t secs, char* err, size_t errcap);
void selftest_stop();
bool selftest_running();
