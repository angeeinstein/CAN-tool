#include "status_led.h"

#include <Arduino.h>

#include "config.h"

namespace {
volatile uint32_t g_last_rx = 0;
volatile bool g_active = false;
volatile bool g_bus_off = false;
bool g_state = false;
constexpr uint32_t FLASH_MS = 25;

void write_led(bool on) {
#if STATUS_LED_PIN >= 0
    if (on == g_state) return;
    g_state = on;
    digitalWrite(STATUS_LED_PIN, on ? STATUS_LED_ON : !STATUS_LED_ON);
#else
    (void)on;
#endif
}
}  // namespace

void led_init() {
#if STATUS_LED_PIN >= 0
    pinMode(STATUS_LED_PIN, OUTPUT);
    g_state = true;
    write_led(false);
#endif
}

void led_notify_rx() { g_last_rx = millis(); }

void led_set_mode(bool active, bool bus_off) {
    g_active = active;
    g_bus_off = bus_off;
}

void led_update() {
    const uint32_t now = millis();
    if (g_bus_off) {
        write_led((now / 80) % 2 == 0);   // fast blink
        return;
    }
    const bool recent_rx = (now - g_last_rx) < FLASH_MS;
    // ACTIVE: solid on, briefly off per frame. LISTEN/CLOSED: off, briefly on per frame.
    write_led(g_active ? !recent_rx : recent_rx);
}
