#include "health.h"

#include <esp_attr.h>
#include <esp_core_dump.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "can_bus.h"
#include "config.h"
#include "host_io.h"
#include "net.h"
#include "sys.h"
#include "web.h"

namespace {

// ---- snapshot that survives a crash/watchdog/software reset (not a power cycle) ----
struct Snapshot {
    uint32_t magic;
    uint32_t uptime_s;
    uint32_t heap_free;
    uint32_t heap_min;
    uint32_t heap_max_block;
    uint32_t rx_fps;
    uint32_t frames_dropped;
    uint32_t ws_dropped;
    uint16_t stack_rx;
    uint16_t stack_out;
    uint16_t stack_main;
    uint16_t stack_web;
    uint8_t mode_active;
    uint8_t selftest;
};
constexpr uint32_t SNAP_MAGIC = 0x48454132;   // "HEA2" (layout changed with the ESP-IDF firmware)
RTC_NOINIT_ATTR Snapshot g_snap;
Snapshot g_prev;                              // copy of the last run's snapshot, for after a crash

esp_reset_reason_t g_reason = ESP_RST_UNKNOWN;
bool g_prev_valid = false;
uint32_t g_boot_count = 0;
uint32_t g_crash_count = 0;
char g_crash[176] = "";   // summary of the core dump found at boot ("" = none)

const char* reason_name(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic/exception";
        case ESP_RST_INT_WDT:   return "interrupt-watchdog";
        case ESP_RST_TASK_WDT:  return "task-watchdog";
        case ESP_RST_WDT:       return "other-watchdog";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

// a reset that was not clean power-on / external / normal software reboot
bool is_crash(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
           r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
}

// task stack headroom in bytes (lowest ever), 0 if the task does not exist
uint16_t stack_free(const char* name) {
    TaskHandle_t h = xTaskGetHandle(name);
    return h ? static_cast<uint16_t>(uxTaskGetStackHighWaterMark(h) * sizeof(StackType_t)) : 0;
}

void read_core_dump() {
    if (esp_core_dump_image_check() != ESP_OK) return;   // no (valid) dump in flash
    esp_core_dump_summary_t* s = static_cast<esp_core_dump_summary_t*>(malloc(sizeof(esp_core_dump_summary_t)));
    if (s && esp_core_dump_get_summary(s) == ESP_OK) {
        int n = snprintf(g_crash, sizeof(g_crash), "task=%.16s pc=0x%08lx", s->exc_task, (unsigned long)s->exc_pc);
#if CONFIG_IDF_TARGET_ARCH_XTENSA
        n += snprintf(g_crash + n, sizeof(g_crash) - n, " bt=");
        for (uint32_t i = 0; i < s->exc_bt_info.depth && i < 10 && n < static_cast<int>(sizeof(g_crash)) - 12; i++)
            n += snprintf(g_crash + n, sizeof(g_crash) - n, "%08lx ", (unsigned long)s->exc_bt_info.bt[i]);
        if (s->exc_bt_info.corrupted && n < static_cast<int>(sizeof(g_crash)) - 12)
            snprintf(g_crash + n, sizeof(g_crash) - n, "(corrupted)");
#else
        (void)n;   // RISC-V (C3): the summary has no ready-made backtrace
#endif
    } else {
        snprintf(g_crash, sizeof(g_crash), "dump present but unreadable");
    }
    free(s);
    esp_core_dump_image_erase();   // report each crash once
}

// ---- synthetic flood -------------------------------------------------------------------
volatile bool g_flood = false;
volatile bool g_flood_task_running = false;
volatile uint32_t g_flood_fps = 0;
volatile uint32_t g_flood_until_ms = 0;

// runs only while a self-test is active, then deletes itself
void flood_task(void*) {
    static uint16_t seq = 0;
    for (;;) {
        if (!g_flood || static_cast<int32_t>(now_ms() - g_flood_until_ms) >= 0) {
            g_flood = false;
            host_notify("#selftest flood done");
            g_flood_task_running = false;
            vTaskDelete(nullptr);
            return;
        }
        // 20 ms slices: smooth rate, the task yields often
        uint32_t per_slice = g_flood_fps / 50;
        if (per_slice < 1) per_slice = 1;
        for (uint32_t i = 0; i < per_slice; i++) {
            CanFrame f{};
            f.id = 0x7FF;   // clearly synthetic (highest 11-bit id), never a real Franke id
            f.dlc = 8;
            f.data[0] = 0x10; f.data[1] = 0x80; f.data[2] = 0x10; f.data[3] = 0x02;
            f.data[4] = static_cast<uint8_t>(seq >> 8); f.data[5] = static_cast<uint8_t>(seq);
            f.data[6] = 0x00; f.data[7] = 0x20;
            seq++;
            host_emit_rx(f, now_us());
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ---- reporting -------------------------------------------------------------------------
uint32_t g_last_report_ms = 0;
uint32_t g_last_snap_ms = 0;
uint32_t g_last_rx_total = 0;
uint32_t g_rx_fps = 0;
uint32_t g_warned_frames = 0, g_warned_msgs = 0, g_warned_ws = 0, g_warned_tcp = 0;

void fill_snapshot(Snapshot& s) {
    HostStats hs;
    WebStats ws;
    host_get_stats(hs);
    web_get_stats(ws);
    s.magic = SNAP_MAGIC;
    s.uptime_s = static_cast<uint32_t>(now_us() / 1000000ULL);
    s.heap_free = heap_free();
    s.heap_min = heap_min();
    s.heap_max_block = heap_largest();
    s.rx_fps = g_rx_fps;
    s.frames_dropped = hs.frames_dropped;
    s.ws_dropped = ws.dropped_bytes;
    s.stack_rx = stack_free("can_rx");
    s.stack_out = stack_free("host_out");
    s.stack_main = stack_free("main");
    s.stack_web = stack_free("web_tx");
    s.mode_active = can_mode() == CanMode::Active ? 1 : 0;
    s.selftest = g_flood ? 1 : 0;
}

}  // namespace

const char* health_reset_reason() { return reason_name(g_reason); }

void health_init() {
    g_reason = esp_reset_reason();
    g_prev_valid = g_snap.magic == SNAP_MAGIC && g_reason != ESP_RST_POWERON;
    if (g_prev_valid) g_prev = g_snap;   // keep the previous run's last snapshot
    memset(&g_snap, 0, sizeof(g_snap));
    g_snap.magic = SNAP_MAGIC;
    read_core_dump();

    Nvs p("sys", true);
    g_boot_count = p.get_u32("boots", 0) + 1;
    g_crash_count = p.get_u32("crashes", 0) + (is_crash(g_reason) ? 1 : 0);
    p.set_u32("boots", g_boot_count);
    p.set_u32("crashes", g_crash_count);
}

void health_boot_report() {
    host_notify("#info reset reason=%s boots=%lu crashes=%lu", reason_name(g_reason),
                (unsigned long)g_boot_count, (unsigned long)g_crash_count);
    if (g_crash[0]) host_notify("#warn crash: %s", g_crash);
    if (g_prev_valid && is_crash(g_reason)) {
        host_notify("#warn before the %s reset: up=%lus heap=%lu min=%lu block=%lu rx=%lu/s drop=%lu web=%lu "
                    "stack rx=%u out=%u main=%u web=%u active=%u selftest=%u",
                    reason_name(g_reason), (unsigned long)g_prev.uptime_s, (unsigned long)g_prev.heap_free,
                    (unsigned long)g_prev.heap_min, (unsigned long)g_prev.heap_max_block,
                    (unsigned long)g_prev.rx_fps, (unsigned long)g_prev.frames_dropped,
                    (unsigned long)g_prev.ws_dropped, g_prev.stack_rx, g_prev.stack_out, g_prev.stack_main,
                    g_prev.stack_web, g_prev.mode_active, g_prev.selftest);
    }
}

void health_print() {
    CanStats cs;
    HostStats hs;
    WebStats ws;
    can_get_stats(cs);
    host_get_stats(hs);
    web_get_stats(ws);
    host_reply("#health uptime_s=%lu heap_free=%lu heap_min=%lu heap_max_block=%lu rx_fps=%lu "
               "rx_total=%lu frames_dropped=%lu msgs_dropped=%lu rx_queue_lost=%lu fifo_lost=%lu",
               (unsigned long)(now_us() / 1000000ULL), (unsigned long)heap_free(), (unsigned long)heap_min(),
               (unsigned long)heap_largest(), (unsigned long)g_rx_fps, (unsigned long)cs.rx,
               (unsigned long)hs.frames_dropped, (unsigned long)hs.msgs_dropped, (unsigned long)cs.rx_missed,
               (unsigned long)cs.rx_overrun);
    host_reply("#health web_clients=%d ws_dropped=%lu ws_batches_lost=%lu ws_slow_closes=%lu ws_send_ms_peak=%lu "
               "tcp_dropped=%lu selftest=%s reset=%s",
               ws.clients, (unsigned long)ws.dropped_bytes, (unsigned long)ws.dropped_batches,
               (unsigned long)ws.slow_closes, (unsigned long)ws.send_ms_peak, (unsigned long)net_tcp_dropped(),
               selftest_running() ? "on" : "off", reason_name(g_reason));
    host_reply("#health stack_free can_rx=%u rule_tx=%u host_out=%u main=%u web_tx=%u httpd=%u tcp_srv=%u serial_rx=%u",
               stack_free("can_rx"), stack_free("rule_tx"), stack_free("host_out"), stack_free("main"),
               stack_free("web_tx"), stack_free("httpd"), stack_free("tcp_srv"), stack_free("serial_rx"));
    if (g_crash[0]) host_reply("#health last_crash %s", g_crash);
    host_reply("#ok health");
}

void health_tick() {
    const uint32_t now = now_ms();

    // 1 Hz: rx rate + snapshot to RTC memory (survives a crash)
    if (now - g_last_snap_ms >= 1000) {
        g_last_snap_ms = now;
        CanStats cs;
        can_get_stats(cs);
        g_rx_fps = cs.rx - g_last_rx_total;
        g_last_rx_total = cs.rx;
        fill_snapshot(g_snap);
    }

    if (now - g_last_report_ms < 5000) return;
    g_last_report_ms = now;

    // drop warnings (only when a counter grew)
    HostStats hs;
    WebStats ws;
    host_get_stats(hs);
    web_get_stats(ws);
    const uint32_t tcpd = net_tcp_dropped();
    if (hs.frames_dropped != g_warned_frames || hs.msgs_dropped != g_warned_msgs ||
        ws.dropped_bytes != g_warned_ws || tcpd != g_warned_tcp) {
        host_notify("#warn dropped: frames=%lu msgs=%lu web_bytes=%lu(batches=%lu, slow_closes=%lu) tcp_bytes=%lu",
                    (unsigned long)hs.frames_dropped, (unsigned long)hs.msgs_dropped,
                    (unsigned long)ws.dropped_bytes, (unsigned long)ws.dropped_batches,
                    (unsigned long)ws.slow_closes, (unsigned long)tcpd);
        g_warned_frames = hs.frames_dropped;
        g_warned_msgs = hs.msgs_dropped;
        g_warned_ws = ws.dropped_bytes;
        g_warned_tcp = tcpd;
    }

    // periodic health while the bus is busy or a self-test runs
    if (g_rx_fps > HEALTH_BUSY_FPS || selftest_running()) {
        host_notify("#health rx_fps=%lu heap_free=%lu heap_min=%lu heap_max_block=%lu frames_dropped=%lu "
                    "ws_send_ms_peak=%lu stack_rx=%u stack_out=%u stack_main=%u selftest=%s",
                    (unsigned long)g_rx_fps, (unsigned long)heap_free(), (unsigned long)heap_min(),
                    (unsigned long)heap_largest(), (unsigned long)hs.frames_dropped,
                    (unsigned long)ws.send_ms_peak, stack_free("can_rx"), stack_free("host_out"),
                    stack_free("main"), selftest_running() ? "on" : "off");
        web_reset_peaks();
    }
}

bool selftest_flood_start(uint32_t fps, uint32_t secs, char* err, size_t errcap) {
    if (fps < 1 || fps > 5000) {
        snprintf(err, errcap, "fps 1..5000");
        return false;
    }
    if (secs < 1 || secs > 1800) {
        snprintf(err, errcap, "secs 1..1800");
        return false;
    }
    g_flood_fps = fps;
    g_flood_until_ms = now_ms() + secs * 1000;
    g_flood = true;
    if (!g_flood_task_running) {
        g_flood_task_running = true;
        if (xTaskCreatePinnedToCore(flood_task, "flood", 3072, nullptr, 2, nullptr, 0) != pdPASS) {
            g_flood = false;
            g_flood_task_running = false;
            snprintf(err, errcap, "no memory for the test task");
            return false;
        }
    }
    return true;
}

void selftest_stop() { g_flood = false; }
bool selftest_running() { return g_flood; }
