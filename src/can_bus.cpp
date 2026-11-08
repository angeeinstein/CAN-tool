#include "can_bus.h"

#include <atomic>
#include <driver/gpio.h>
#include <driver/twai.h>
#include <esp_intr_alloc.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "host_io.h"
#include "rule_engine.h"
#include "rules.h"
#include "status_led.h"
#include "sys.h"

namespace {

// ---- state -----------------------------------------------------------------
volatile CanMode g_mode = CanMode::Closed;
volatile uint32_t g_bitrate = CAN_DEFAULT_BITRATE;
volatile bool g_installed = false;
volatile bool g_tx_enabled = false;   // TX gate, only true while ACTIVE and not stopped
volatile bool g_bus_off = false;
volatile uint32_t g_last_rx_ms = 0;

TaskHandle_t g_rx_task = nullptr;
SemaphoreHandle_t g_tx_mtx = nullptr;   // excludes transmit while the driver is reconfigured

// reconfiguration requests (processed by the RX task)
portMUX_TYPE g_req_mux = portMUX_INITIALIZER_UNLOCKED;
CanMode g_req_mode = CanMode::Listen;
uint32_t g_req_bitrate = CAN_DEFAULT_BITRATE;
std::atomic<uint32_t> g_req_seq{0};
volatile uint32_t g_done_seq = 0;
volatile bool g_last_cfg_ok = true;

// rate limiter
portMUX_TYPE g_rate_mux = portMUX_INITIALIZER_UNLOCKED;
RateLimiter g_rate;

// statistics
std::atomic<uint32_t> s_rx{0}, s_tx{0}, s_tx_err{0}, s_bus_off{0};
// driver counters are reset by uninstall -> keep an accumulated offset
uint32_t acc_missed = 0, acc_overrun = 0, acc_bus_err = 0, acc_arb = 0, acc_txfail = 0;
volatile uint8_t g_slcan_flags = 0;

// STOP latch: set by an emergency stop (STOP button, #stop, BOOT key, TX rate limit), kept in NVS,
// released only by "#resume". While set, nothing may switch to ACTIVE, send or (re)enable rules.
volatile bool g_stop_latched = false;
std::atomic<uint32_t> g_stop_count{0};

// ACTIVE watchdog
volatile uint8_t g_active_owner = 0;        // sink (SINK_*) that enabled ACTIVE, 0 = none
volatile uint32_t g_active_owner_cli = 0;   // web client id if owner is SINK_WEB
volatile uint32_t g_wdog_deadline = 0;      // now_ms() at which it fires (0 = not armed)
uint32_t g_wdog_secs = CAN_ACTIVE_WATCHDOG_S;

void save_latch() {
    Nvs p("can", true);
    p.set_bool("latch", g_stop_latched);
    p.set_u32("stops", g_stop_count);
}

// pins: configured (NVS) and the ones the installed driver currently uses
volatile int g_tx_pin = CAN_TX_PIN;
volatile int g_rx_pin = CAN_RX_PIN;
int g_used_tx = -1;
int g_used_rx = -1;

// Pins that are safe for CTX/CRX: no strapping pins, no pins that are active during boot,
// no flash/PSRAM/USB/UART0 pins, not the status LED.
#if defined(CONFIG_IDF_TARGET_ESP32)
const uint8_t TX_PINS[] = {13, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
const uint8_t RX_PINS[] = {13, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33, 34, 35, 36, 39};
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
const uint8_t TX_PINS[] = {1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 47};
const uint8_t RX_PINS[] = {1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 47};
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
const uint8_t TX_PINS[] = {0, 1, 3, 4, 5, 6, 7, 10};
const uint8_t RX_PINS[] = {0, 1, 3, 4, 5, 6, 7, 10};
#endif

template <size_t N>
bool in_list(const uint8_t (&list)[N], int pin) {
    for (uint8_t p : list)
        if (p == pin) return true;
    return false;
}

template <size_t N>
size_t list_str(const uint8_t (&list)[N], char* out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < N && n + 4 < cap; i++) n += snprintf(out + n, cap - n, i ? ",%u" : "%u", list[i]);
    return n;
}

// ---- helpers ---------------------------------------------------------------

bool timing_for(uint32_t bps, twai_timing_config_t& t) {
    switch (bps) {
#ifdef TWAI_TIMING_CONFIG_10KBITS
        case 10000: t = TWAI_TIMING_CONFIG_10KBITS(); return true;
#endif
#ifdef TWAI_TIMING_CONFIG_20KBITS
        case 20000: t = TWAI_TIMING_CONFIG_20KBITS(); return true;
#endif
        case 50000: t = TWAI_TIMING_CONFIG_50KBITS(); return true;
        case 100000: t = TWAI_TIMING_CONFIG_100KBITS(); return true;
        case 125000: t = TWAI_TIMING_CONFIG_125KBITS(); return true;
        case 250000: t = TWAI_TIMING_CONFIG_250KBITS(); return true;
        case 500000: t = TWAI_TIMING_CONFIG_500KBITS(); return true;
        case 800000: t = TWAI_TIMING_CONFIG_800KBITS(); return true;
        case 1000000: t = TWAI_TIMING_CONFIG_1MBITS(); return true;
        default: return false;
    }
}

void accumulate_driver_stats() {
    twai_status_info_t st;
    if (g_installed && twai_get_status_info(&st) == ESP_OK) {
        acc_missed += st.rx_missed_count;
        acc_overrun += st.rx_overrun_count;
        acc_bus_err += st.bus_error_count;
        acc_arb += st.arb_lost_count;
        acc_txfail += st.tx_failed_count;
    }
}

// Removes the installed driver. The TWAI driver only allows uninstall in STOPPED or BUS_OFF, and stop only
// in RUNNING; during a bus-off recovery (RECOVERING) neither is possible. Then the driver stays as it is
// and false is returned - the request is retried until the recovery has finished (otherwise the driver
// object would leak, every later install would fail and the bus would stay deaf until a reboot).
bool remove_driver() {
    if (!g_installed) return true;
    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK) return false;
    if (st.state == TWAI_STATE_RECOVERING) return false;
    accumulate_driver_stats();
    if (st.state == TWAI_STATE_RUNNING) twai_stop();
    if (twai_driver_uninstall() != ESP_OK) return false;
    g_installed = false;
    return true;
}

enum class Applied : uint8_t { Ok, Failed, Pending };

// Runs in the RX task only.
Applied apply_config(CanMode mode, uint32_t bitrate) {
    g_tx_enabled = false;
    // the STOP latch wins over a concurrent "go ACTIVE" request (both may be posted at the same time)
    if (mode == CanMode::Active && g_stop_latched) mode = CanMode::Listen;
    // leaving ACTIVE: actions still scheduled by rules must not go out after a later switch back
    if (mode != CanMode::Active) rules_clear_pending();
    xSemaphoreTake(g_tx_mtx, portMAX_DELAY);

    if (!remove_driver()) {
        xSemaphoreGive(g_tx_mtx);
        return Applied::Pending;
    }
    // pins changed: detach the old ones from the TWAI signals (the GPIO matrix keeps them routed)
    const int tx_pin = g_tx_pin, rx_pin = g_rx_pin;
    if (g_used_tx >= 0 && g_used_tx != tx_pin) gpio_reset_pin(static_cast<gpio_num_t>(g_used_tx));
    if (g_used_rx >= 0 && g_used_rx != rx_pin) gpio_reset_pin(static_cast<gpio_num_t>(g_used_rx));
    g_used_tx = g_used_rx = -1;
    g_mode = CanMode::Closed;
    Applied result = Applied::Ok;

    if (mode != CanMode::Closed) {
        twai_timing_config_t t;
        if (!timing_for(bitrate, t)) {
            bitrate = CAN_DEFAULT_BITRATE;
            timing_for(bitrate, t);
        }
        const twai_mode_t tm = (mode == CanMode::Active) ? TWAI_MODE_NORMAL : TWAI_MODE_LISTEN_ONLY;
        twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
            static_cast<gpio_num_t>(tx_pin), static_cast<gpio_num_t>(rx_pin), tm);
        g.rx_queue_len = TWAI_RX_QUEUE_LEN;
        g.tx_queue_len = (mode == CanMode::Active) ? TWAI_TX_QUEUE_LEN : 0;
        g.alerts_enabled = TWAI_ALERT_RX_DATA | TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED |
                           TWAI_ALERT_ERR_PASS | TWAI_ALERT_ERR_ACTIVE | TWAI_ALERT_ABOVE_ERR_WARN |
                           TWAI_ALERT_RX_QUEUE_FULL | TWAI_ALERT_RX_FIFO_OVERRUN |
                           TWAI_ALERT_ARB_LOST | TWAI_ALERT_BUS_ERROR | TWAI_ALERT_TX_FAILED;
        g.intr_flags = ESP_INTR_FLAG_IRAM;   // CONFIG_TWAI_ISR_IN_IRAM: keeps receiving while flash is written
        twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

        esp_err_t e = twai_driver_install(&g, &t, &f);
        if (e == ESP_OK) {
            e = twai_start();
            if (e != ESP_OK) twai_driver_uninstall();
        }
        if (e == ESP_OK) {
            g_installed = true;
            g_used_tx = tx_pin;
            g_used_rx = rx_pin;
            g_mode = mode;
            g_bitrate = bitrate;
            g_bus_off = false;
        } else {
            result = Applied::Failed;
            host_notify("#error twai %s (bus closed)", esp_err_to_name(e));
        }
    }

    if (g_mode == CanMode::Active) {
        portENTER_CRITICAL(&g_rate_mux);
        g_rate.reset();
        portEXIT_CRITICAL(&g_rate_mux);
        g_tx_enabled = true;
    }
    xSemaphoreGive(g_tx_mtx);
    led_set_mode(g_mode == CanMode::Active, false);
    return result;
}

uint32_t post_request(CanMode mode, uint32_t bitrate) {
    portENTER_CRITICAL(&g_req_mux);
    g_req_mode = mode;
    g_req_bitrate = bitrate;
    uint32_t seq = ++g_req_seq;
    portEXIT_CRITICAL(&g_req_mux);
    if (g_rx_task) xTaskNotifyGive(g_rx_task);
    return seq;
}

bool wait_applied(uint32_t seq, uint32_t timeout_ms = 1000) {
    const uint32_t start = now_ms();
    while (static_cast<int32_t>(g_done_seq - seq) < 0) {
        if (now_ms() - start > timeout_ms) return false;
        vTaskDelay(1);
    }
    return g_last_cfg_ok;
}

void process_request_if_any() {
    portENTER_CRITICAL(&g_req_mux);
    const uint32_t seq = g_req_seq;
    const CanMode mode = g_req_mode;
    const uint32_t br = g_req_bitrate;
    portEXIT_CRITICAL(&g_req_mux);
    if (seq == g_done_seq) return;
    static uint32_t pending_reported = 0;
    const Applied r = apply_config(mode, br);
    if (r == Applied::Pending) {   // bus-off recovery still running: retry on the next loop pass (~10 ms)
        if (pending_reported != seq) {
            pending_reported = seq;
            host_notify("#warn bus-off recovery in progress - mode change waits until the bus is free again");
        }
        return;
    }
    g_last_cfg_ok = r == Applied::Ok;
    g_done_seq = seq;
}

void handle_alerts(uint32_t a) {
    if (a & TWAI_ALERT_RX_QUEUE_FULL) g_slcan_flags |= 0x01;
    if (a & TWAI_ALERT_RX_FIFO_OVERRUN) g_slcan_flags |= 0x08;
    if (a & TWAI_ALERT_ABOVE_ERR_WARN) g_slcan_flags |= 0x04;
    if (a & TWAI_ALERT_ARB_LOST) g_slcan_flags |= 0x40;
    if (a & TWAI_ALERT_BUS_ERROR) g_slcan_flags |= 0x80;
    if (a & TWAI_ALERT_ERR_PASS) {
        g_slcan_flags |= 0x20;
        host_notify("#warn error-passive");
    }
    if (a & TWAI_ALERT_ERR_ACTIVE) host_notify("#info error-active");

    if (a & TWAI_ALERT_BUS_OFF) {
        g_tx_enabled = false;
        g_bus_off = true;
        s_bus_off++;
        rules_clear_pending();
        led_set_mode(false, true);
        twai_status_info_t st;
        uint32_t tec = 0;
        if (twai_get_status_info(&st) == ESP_OK) tec = st.tx_error_counter;
        host_notify("#busoff count=%lu tec=%lu -> recovering, then LISTEN", (unsigned long)s_bus_off,
                    (unsigned long)tec);
        twai_initiate_recovery();
    }
    if (a & TWAI_ALERT_BUS_RECOVERED) {
        host_notify("#busrecovered -> LISTEN");
        post_request(CanMode::Listen, g_bitrate);   // processed in the next loop iteration
    }
}

void rx_task(void*) {
    for (;;) {
        process_request_if_any();

        if (!g_installed) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
            continue;
        }

        uint32_t alerts = 0;
        twai_read_alerts(&alerts, pdMS_TO_TICKS(10));

        twai_message_t m;
        while (twai_receive(&m, 0) == ESP_OK) {
            const uint64_t ts = esp_timer_get_time();
            CanFrame f;
            f.id = m.identifier;
            f.ext = m.extd;
            f.rtr = m.rtr;
            f.dlc = m.data_length_code > 8 ? 8 : m.data_length_code;
            memcpy(f.data, m.data, 8);
            s_rx++;
            g_last_rx_ms = now_ms();
            led_notify_rx();
            host_emit_rx(f, ts);
            if (g_mode == CanMode::Active && g_tx_enabled) rules_on_frame(f, ts);
        }

        if (alerts & ~TWAI_ALERT_RX_DATA) handle_alerts(alerts);
    }
}

}  // namespace

// ---- public API ------------------------------------------------------------------

const char* can_mode_name(CanMode m) {
    switch (m) {
        case CanMode::Listen: return "listen";
        case CanMode::Active: return "active";
        default: return "closed";
    }
}

const char* tx_result_name(TxResult r) {
    switch (r) {
        case TxResult::Ok: return "ok";
        case TxResult::NotActive: return "not-active";
        case TxResult::RateLimited: return "rate-limit";
        case TxResult::Busy: return "tx-queue-full";
        default: return "error";
    }
}

void can_init() {
    g_tx_mtx = xSemaphoreCreateMutex();
    // pins from NVS (set via "#pins" / web UI), only if still valid for this chip
    {
        Nvs p("can", false);
        const int tx = p.get_i32("tx", CAN_TX_PIN), rx = p.get_i32("rx", CAN_RX_PIN);
        g_stop_latched = p.get_bool("latch", false);
        g_stop_count = p.get_u32("stops", 0);
        g_wdog_secs = p.get_u32("wdog", CAN_ACTIVE_WATCHDOG_S);
        if (in_list(TX_PINS, tx) && in_list(RX_PINS, rx) && tx != rx) {
            g_tx_pin = tx;
            g_rx_pin = rx;
        }
    }
    g_rate.set_limit(TX_RATE_LIMIT_DEFAULT);
    // boot always into LISTEN
    g_last_cfg_ok = apply_config(CanMode::Listen, CAN_DEFAULT_BITRATE) == Applied::Ok;
    xTaskCreatePinnedToCore(rx_task, "can_rx", 4096, nullptr, configMAX_PRIORITIES - 2, &g_rx_task,
                            portNUM_PROCESSORS > 1 ? 1 : tskNO_AFFINITY);
}

bool can_set_mode(CanMode m) {
    if (m == CanMode::Active && g_stop_latched) return false;   // STOP latch: no ACTIVE until #resume
    return wait_applied(post_request(m, g_bitrate));
}

bool can_bitrate_supported(uint32_t bps) {
    twai_timing_config_t t;
    return timing_for(bps, t);
}

bool can_set_bitrate(uint32_t bps) {
    if (!can_bitrate_supported(bps)) return false;
    CanMode m = g_mode;
    if (!g_installed) {
        g_bitrate = bps;   // takes effect on the next open
        return true;
    }
    return wait_applied(post_request(m, bps));
}

bool can_set_pins(int tx, int rx, char* err, size_t errcap) {
    if (!in_list(TX_PINS, tx) || !in_list(RX_PINS, rx) || tx == rx) {
        char t[96], r[96];
        list_str(TX_PINS, t, sizeof(t));
        list_str(RX_PINS, r, sizeof(r));
        snprintf(err, errcap, "allowed TX: %s  RX: %s (TX != RX)", t, r);
        return false;
    }
    {
        Nvs p("can", true);
        p.set_i32("tx", tx);
        p.set_i32("rx", rx);
    }
    g_tx_pin = tx;
    g_rx_pin = rx;
    // re-open on the new pins; for safety always in LISTEN
    rules_clear_pending();
    if (!wait_applied(post_request(g_mode == CanMode::Closed ? CanMode::Closed : CanMode::Listen, g_bitrate))) {
        snprintf(err, errcap, "driver did not start on the new pins");
        return false;
    }
    return true;
}

void can_get_pins(int& tx, int& rx) {
    tx = g_tx_pin;
    rx = g_rx_pin;
}

void can_pins_allowed(char* tx_out, size_t tx_cap, char* rx_out, size_t rx_cap) {
    list_str(TX_PINS, tx_out, tx_cap);
    list_str(RX_PINS, rx_out, rx_cap);
}

CanMode can_mode() { return g_mode; }
uint32_t can_bitrate() { return g_bitrate; }
bool can_bus_off() { return g_bus_off; }
uint32_t can_last_rx_ms() { return g_last_rx_ms; }

TxResult can_transmit(const CanFrame& f, const char* reason) {
    // TX gate: nothing leaves the controller unless we are ACTIVE (checked again under the mutex)
    if (g_mode != CanMode::Active || !g_tx_enabled) return TxResult::NotActive;
    if (xSemaphoreTake(g_tx_mtx, pdMS_TO_TICKS(5)) != pdTRUE) return TxResult::Error;
    if (g_mode != CanMode::Active || !g_tx_enabled || !g_installed) {
        xSemaphoreGive(g_tx_mtx);
        return TxResult::NotActive;
    }

    portENTER_CRITICAL(&g_rate_mux);
    const bool allowed = g_rate.allow(now_ms());
    portEXIT_CRITICAL(&g_rate_mux);
    if (!allowed) {
        g_tx_enabled = false;
        xSemaphoreGive(g_tx_mtx);
        char why[48];
        snprintf(why, sizeof(why), "tx-rate-limit %u/s exceeded", g_rate.limit());
        can_emergency_stop(why);
        return TxResult::RateLimited;
    }

    twai_message_t m = {};
    m.identifier = f.id;
    m.extd = f.ext;
    m.rtr = f.rtr;
    m.data_length_code = f.dlc > 8 ? 8 : f.dlc;
    memcpy(m.data, f.data, 8);
    const esp_err_t e = twai_transmit(&m, 0);
    const uint64_t ts = esp_timer_get_time();
    xSemaphoreGive(g_tx_mtx);

    if (e == ESP_OK) {
        s_tx++;
        if (host_echo()) host_emit_tx(f, ts, reason);
        return TxResult::Ok;
    }
    s_tx_err++;
    host_notify("#txerr %03lX %s %s", (unsigned long)f.id, reason ? reason : "-", esp_err_to_name(e));
    return (e == ESP_ERR_TIMEOUT || e == ESP_FAIL) ? TxResult::Busy : TxResult::Error;
}

void can_emergency_stop(const char* reason, bool latch) {
    g_tx_enabled = false;
    rules_disable_all();
    rules_clear_pending();
    if (xSemaphoreTake(g_tx_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (g_installed && g_mode == CanMode::Active) twai_clear_transmit_queue();
        xSemaphoreGive(g_tx_mtx);
    }
    // latch first: a concurrent "#mode active" that is applied after this LISTEN request is then
    // turned into LISTEN by apply_config instead of re-opening the bus in ACTIVE
    if (latch) g_stop_latched = true;
    post_request(CanMode::Listen, g_bitrate);
    if (latch) {
        g_stop_count++;
        save_latch();
        host_notify("#stop %s -> LISTEN, all rules disabled; LATCHED (stops=%lu) - #resume to release",
                    reason ? reason : "", (unsigned long)g_stop_count);
    } else {
        host_notify("#stop %s -> LISTEN, all rules disabled", reason ? reason : "");
    }
}

bool can_stop_latched() { return g_stop_latched; }
uint32_t can_stop_count() { return g_stop_count; }

void can_resume() {
    g_stop_latched = false;
    g_stop_count = 0;
    save_latch();
    host_notify("#resume stop latch released (bus stays in %s until a mode change)", can_mode_name(g_mode));
}

void can_active_set_owner(uint8_t sink, uint32_t web_client) {
    g_active_owner = sink;
    g_active_owner_cli = web_client;
    g_wdog_deadline = 0;
}
uint8_t can_active_owner() { return g_active_owner; }
uint32_t can_active_owner_client() { return g_active_owner_cli; }
uint32_t can_watchdog_secs() { return g_wdog_secs; }

void can_set_watchdog(uint32_t secs) {
    g_wdog_secs = secs;
    g_wdog_deadline = 0;
    Nvs p("can", true);
    p.set_u32("wdog", secs);
}

void can_watchdog_tick(bool owner_alive) {
    // only watch ACTIVE owned by a network link; USB is physical and never watched
    if (g_mode != CanMode::Active || g_active_owner == 0 || g_active_owner == SINK_SERIAL || g_wdog_secs == 0) {
        g_active_owner = 0;
        g_wdog_deadline = 0;
        return;
    }
    if (owner_alive) { g_wdog_deadline = 0; return; }
    if (g_wdog_deadline == 0) { g_wdog_deadline = now_ms() + g_wdog_secs * 1000; return; }
    if (static_cast<int32_t>(now_ms() - g_wdog_deadline) >= 0) {
        uint32_t secs = g_wdog_secs;
        g_active_owner = 0;
        g_wdog_deadline = 0;
        rules_disable_all();
        can_set_mode(CanMode::Listen);   // not latched: this is not a user STOP
        host_notify("#stop watchdog: controlling client gone %lus -> LISTEN, all rules disabled "
                    "(not latched)", (unsigned long)secs);
    }
}

void can_set_rate_limit(uint16_t per_sec) {
    portENTER_CRITICAL(&g_rate_mux);
    g_rate.set_limit(per_sec);
    portEXIT_CRITICAL(&g_rate_mux);
}

uint16_t can_rate_limit() { return g_rate.limit(); }

void can_get_stats(CanStats& s) {
    s.rx = s_rx;
    s.tx = s_tx;
    s.tx_errors = s_tx_err;
    s.bus_off_count = s_bus_off;
    s.tec = s.rec = 0;
    s.rx_missed = acc_missed;
    s.rx_overrun = acc_overrun;
    s.bus_errors = acc_bus_err;
    s.arb_lost = acc_arb;
    s.tx_failed = acc_txfail;
    s.state = g_bus_off ? "bus-off" : (g_installed ? "running" : "stopped");
    twai_status_info_t st;
    // g_tx_mtx: the RX task may be uninstalling the driver right now (it holds the mutex meanwhile)
    bool have = false;
    if (xSemaphoreTake(g_tx_mtx, pdMS_TO_TICKS(5)) == pdTRUE) {
        have = g_installed && twai_get_status_info(&st) == ESP_OK;
        xSemaphoreGive(g_tx_mtx);
    }
    if (have) {
        s.tec = st.tx_error_counter;
        s.rec = st.rx_error_counter;
        s.rx_missed += st.rx_missed_count;
        s.rx_overrun += st.rx_overrun_count;
        s.bus_errors += st.bus_error_count;
        s.arb_lost += st.arb_lost_count;
        s.tx_failed += st.tx_failed_count;
        if (st.state == TWAI_STATE_BUS_OFF) s.state = "bus-off";
        else if (st.state == TWAI_STATE_RECOVERING) s.state = "recovering";
    }
}

void can_reset_stats() {
    s_rx = s_tx = s_tx_err = s_bus_off = 0;
    // set the offsets so that "accumulated + current driver counter" becomes 0
    acc_missed = acc_overrun = acc_bus_err = acc_arb = acc_txfail = 0;
    twai_status_info_t st;
    bool have = false;
    if (xSemaphoreTake(g_tx_mtx, pdMS_TO_TICKS(5)) == pdTRUE) {
        have = g_installed && twai_get_status_info(&st) == ESP_OK;
        xSemaphoreGive(g_tx_mtx);
    }
    if (have) {
        acc_missed -= st.rx_missed_count;
        acc_overrun -= st.rx_overrun_count;
        acc_bus_err -= st.bus_error_count;
        acc_arb -= st.arb_lost_count;
        acc_txfail -= st.tx_failed_count;
    }
}

uint8_t can_take_slcan_flags() {
    uint8_t f = g_slcan_flags;
    g_slcan_flags = 0;
    if (g_bus_off) f |= 0x20;
    return f;
}
