// ESP32 CAN sniffer & payment-system emulator for the Franke FM850 (ESP-IDF).
//
// Tasks:
//   can_rx    (core 1, prio max-2) owns the TWAI driver: receive, alerts, reconfiguration
//   rule_tx   (core 1, prio max-1) sends scheduled rule actions at the right time
//   host_out  (core 0, prio 6)     writes frames/notifications to USB, TCP and the web buffer
//   serial_rx / tcp_srv / httpd    read command lines from USB / TCP / browsers -> command queue
//   web_tx    (core 0, prio 4)     sends live data and replies to the browsers
//   safety    (core 1, prio 10)    BOOT button (emergency stop) and ACTIVE watchdog - independent of how
//                                  long a command in the main task takes
//   main      (core 0, prio 1)     executes the queued commands one after another, WLAN state machine,
//                                  LED, health, rollback confirm
//
// After every boot the bus is in LISTEN mode (no ACK, no transmit). ACTIVE only on command.
#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include "can_bus.h"
#include "clock.h"
#include "commands.h"
#include "config.h"
#include "health.h"
#include "host_io.h"
#include "host_serial.h"
#include "net.h"
#include "rules.h"
#include "status_led.h"
#include "sys.h"
#include "web.h"

namespace {

void init_nvs() {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();   // unusable NVS partition: start with defaults
        nvs_flash_init();
    }
}

void init_estop_button() {
    const gpio_num_t pin = static_cast<gpio_num_t>(ESTOP_BUTTON_PIN);
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
}

// BOOT button = emergency stop (debounced 30 ms, active low)
void poll_estop_button() {
    static uint32_t low_since = 0;
    static bool fired = false;
    const bool pressed = gpio_get_level(static_cast<gpio_num_t>(ESTOP_BUTTON_PIN)) == 0;
    const uint32_t now = now_ms();
    if (!pressed) {
        low_since = 0;
        fired = false;
        return;
    }
    if (low_since == 0) low_since = now ? now : 1;
    if (!fired && now - low_since >= 30) {
        fired = true;
        can_emergency_stop("button");
    }
}

// ACTIVE watchdog: if the TCP/web client that enabled ACTIVE is gone, revert to LISTEN (not latched)
void watchdog_tick() {
    const uint8_t owner = can_active_owner();
    bool alive = true;
    if (owner == SINK_TCP) alive = net_tcp_connected();
    else if (owner == SINK_WEB) alive = web_client_connected(can_active_owner_client());
    can_watchdog_tick(alive);
}

void safety_task(void*) {
    uint32_t last_wdog = 0;
    for (;;) {
        poll_estop_button();
        const uint32_t now = now_ms();
        if (now - last_wdog >= 1000) {
            last_wdog = now;
            watchdog_tick();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

}  // namespace

extern "C" void app_main() {
    esp_log_level_set("*", ESP_LOG_NONE);   // the console UART is the SLCAN host port
    init_nvs();
    esp_netif_init();
    esp_event_loop_create_default();

    cmd_init();
    host_init();          // output queues + task: everything after this may notify
    host_serial_init();   // USB link + reader task
    led_init();
    init_estop_button();
    health_init();        // reset reason, crash dump, previous-run snapshot
    rules_init();
    can_init();           // -> LISTEN
    net_init();           // WLAN, mDNS, TCP server
    clock_init();         // NTP (needs the network stack)
    web_init();

    host_notify("#boot franke-can fw=%s board=%s idf=%s mode=%s bitrate=%lu (type #help)", FW_VERSION,
                CONFIG_IDF_TARGET, esp_get_idf_version(), can_mode_name(can_mode()), (unsigned long)can_bitrate());
    health_boot_report();
    if (sys_web_login_is_default())
        host_notify("#warn web login is the default or too short (WEB_USER/WEB_PASSWORD) - set your own "
                    "(min. 8 characters) in include/secrets.h");
    if (sys_ap_password_is_default())
        host_notify("#warn access point password is the default (AP_PASSWORD) - set your own in include/secrets.h");
    if (sys_app_pending_verify())
        host_notify("#info new firmware: it is confirmed after %d s without a crash, otherwise the previous "
                    "one is restored", APP_VALID_AFTER_S);
    xTaskCreatePinnedToCore(safety_task, "safety", 3072, nullptr, 10, nullptr, portNUM_PROCESSORS > 1 ? 1 : 0);

    uint32_t last_net = 0, last_1s = 0;
    bool verify_checked = false;   // the rollback state is read from flash once, after APP_VALID_AFTER_S
    for (;;) {
        cmd_process(pdMS_TO_TICKS(10));   // executes one queued command, or waits up to 10 ms

        const uint32_t now = now_ms();
        led_set_mode(can_mode() == CanMode::Active, can_bus_off());
        led_update();
        health_tick();
        if (now - last_net >= 200) {
            last_net = now;
            net_tick();
        }
        if (now - last_1s >= 1000) {
            last_1s = now;
            if (!verify_checked && now >= APP_VALID_AFTER_S * 1000U) {
                verify_checked = true;
                if (sys_app_confirm()) host_notify("#info firmware confirmed");
            }
        }
        if (sys_reboot_due()) esp_restart();
    }
}
