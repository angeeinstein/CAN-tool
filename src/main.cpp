// ESP32 CAN sniffer & payment-system emulator for the Franke FM850.
//
// Tasks:
//   can_rx   (core 1, prio max-2) owns the TWAI driver: receive, alerts, reconfiguration
//   rule_tx  (core 1, prio max-1) sends scheduled rule actions at the right time
//   host_out (core 0, prio 3)     writes frames/notifications to USB (+TCP), never blocks can_rx
//   loop     (Arduino)            command input (USB/TCP), LED, emergency-stop button, WLAN
//
// After every boot the bus is in LISTEN mode (no ACK, no transmit). ACTIVE only on command.
#include <Arduino.h>
#include <esp_log.h>

#include "can_bus.h"
#include "clock.h"
#include "commands.h"
#include "config.h"
#include "host_io.h"
#include "net.h"
#include "rules.h"
#include "status_led.h"
#include "web.h"

namespace {

LineBuffer g_serial_line;

// BOOT button = emergency stop (debounced, active low)
void poll_estop_button() {
    static uint32_t low_since = 0;
    static bool fired = false;
    const bool pressed = digitalRead(ESTOP_BUTTON_PIN) == LOW;
    const uint32_t now = millis();
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

void setup_serial() {
#if ARDUINO_USB_CDC_ON_BOOT
    Serial.setRxBufferSize(1024);
    Serial.setTxBufferSize(4096);
    Serial.begin(HOST_SERIAL_BAUD);
    Serial.setTxTimeoutMs(5);   // no host attached -> don't stall the output task
#else
    Serial.setRxBufferSize(1024);
    Serial.setTxBufferSize(8192);
    Serial.begin(HOST_SERIAL_BAUD);
#endif
}

}  // namespace

void setup() {
    esp_log_level_set("*", ESP_LOG_NONE);   // keep the host port clean
    setup_serial();
    pinMode(ESTOP_BUTTON_PIN, INPUT_PULLUP);
    led_init();

    host_init();
    rules_init();
    can_init();   // -> LISTEN
    net_init();
    clock_init();   // NTP (needs the network stack from net_init)
    web_init();

    host_notify("#boot franke-can fw=%s board=%s mode=%s bitrate=%lu (type #help)", FW_VERSION,
                CONFIG_IDF_TARGET, can_mode_name(can_mode()), (unsigned long)can_bitrate());
}

void loop() {
    int budget = 512;
    while (budget-- > 0 && Serial.available() > 0) {
        const int c = Serial.read();
        if (c < 0) break;
        if (g_serial_line.feed(static_cast<char>(c))) cmd_handle_line(g_serial_line.line(), SINK_SERIAL);
    }

    net_loop();
    web_loop();
    poll_estop_button();
    led_set_mode(can_mode() == CanMode::Active, can_bus_off());
    led_update();
    delay(1);
}
