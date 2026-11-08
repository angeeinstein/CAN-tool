#include "clock.h"

#include <esp_sntp.h>
#include <esp_timer.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "config.h"
#include "host_io.h"

namespace {

volatile ClockSource g_src = ClockSource::None;
constexpr uint64_t MIN_VALID_EPOCH_US = 1700000000ULL * 1000000ULL;   // 2023-11 - anything earlier is "not set"

uint64_t epoch_now_us() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000ULL + static_cast<uint64_t>(tv.tv_usec);
}

void on_ntp_sync(struct timeval*) {
    const bool first = g_src != ClockSource::Ntp;
    g_src = ClockSource::Ntp;
    if (first) {
        time_t t = time(nullptr);
        struct tm tm;
        gmtime_r(&t, &tm);
        host_notify("#clock synced via NTP: %04d-%02d-%02d %02d:%02d:%02d UTC", tm.tm_year + 1900, tm.tm_mon + 1,
                    tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
}

}  // namespace

void clock_init() {
    // UTC everywhere; the browser / PC shows local time
    setenv("TZ", "UTC0", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);   // re-sync every CONFIG_LWIP_SNTP_UPDATE_DELAY (1 h)
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_setservername(2, "time.cloudflare.com");
    sntp_set_time_sync_notification_cb(on_ntp_sync);
    esp_sntp_init();   // keeps trying until the WLAN has internet
}

ClockSource clock_source() { return g_src; }

const char* clock_source_name() {
    switch (g_src) {
        case ClockSource::Ntp: return "ntp";
        case ClockSource::Host: return "host";
        default: return "none";
    }
}

void clock_now(uint64_t& boot_us, uint64_t& epoch_us) {
    // read both clocks back to back so the pair is consistent to a few µs
    // (no interrupt lock: gettimeofday() takes a mutex internally)
    boot_us = static_cast<uint64_t>(esp_timer_get_time());
    const uint64_t e = epoch_now_us();
    epoch_us = (g_src != ClockSource::None && e >= MIN_VALID_EPOCH_US) ? e : 0;
}

uint64_t clock_epoch_of(uint64_t boot_us) {
    uint64_t b, e;
    clock_now(b, e);
    if (e == 0) return 0;
    return e - (b - boot_us);
}

bool clock_set_from_host(uint64_t epoch_ms) {
    if (g_src == ClockSource::Ntp) return false;
    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(epoch_ms / 1000ULL);
    tv.tv_usec = static_cast<suseconds_t>((epoch_ms % 1000ULL) * 1000ULL);
    if (static_cast<uint64_t>(tv.tv_sec) * 1000000ULL < MIN_VALID_EPOCH_US) return false;
    settimeofday(&tv, nullptr);
    g_src = ClockSource::Host;
    return true;
}
