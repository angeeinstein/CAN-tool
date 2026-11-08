#include "status_led.h"

#include <driver/gpio.h>
#include <stdint.h>

#include "config.h"
#include "sys.h"

#if STATUS_LED_PIN >= 0 && STATUS_LED_RGB
#include <led_strip.h>
#endif

namespace {
volatile uint32_t g_last_rx = 0;
volatile bool g_active = false;
volatile bool g_bus_off = false;
constexpr uint32_t FLASH_MS = 25;

enum class Shown : uint8_t { Off, Listen, Active, BusOff };
Shown g_shown = Shown::Off;

#if STATUS_LED_PIN >= 0 && STATUS_LED_RGB
led_strip_handle_t g_strip = nullptr;

void show(Shown s) {
    if (s == g_shown || !g_strip) return;
    g_shown = s;
    constexpr uint8_t L = 24;   // brightness (0..255): enough to see, not blinding
    switch (s) {
        case Shown::Listen: led_strip_set_pixel(g_strip, 0, 0, L, 0); break;
        case Shown::Active: led_strip_set_pixel(g_strip, 0, L, 0, 0); break;
        case Shown::BusOff: led_strip_set_pixel(g_strip, 0, L, L, 0); break;
        default: led_strip_clear(g_strip); return;
    }
    led_strip_refresh(g_strip);
}
#elif STATUS_LED_PIN >= 0
void show(Shown s) {
    if (s == g_shown) return;
    g_shown = s;
    const int on = STATUS_LED_ON_LEVEL;
    gpio_set_level(static_cast<gpio_num_t>(STATUS_LED_PIN), s == Shown::Off ? !on : on);
}
#else
void show(Shown) {}
#endif
}  // namespace

void led_init() {
#if STATUS_LED_PIN >= 0 && STATUS_LED_RGB
    led_strip_config_t sc = {};
    sc.strip_gpio_num = STATUS_LED_PIN;
    sc.max_leds = 1;
    sc.led_pixel_format = LED_PIXEL_FORMAT_GRB;
    sc.led_model = LED_MODEL_WS2812;
    led_strip_rmt_config_t rc = {};
    rc.clk_src = RMT_CLK_SRC_DEFAULT;
    rc.resolution_hz = 10 * 1000 * 1000;
    if (led_strip_new_rmt_device(&sc, &rc, &g_strip) == ESP_OK) led_strip_clear(g_strip);
#elif STATUS_LED_PIN >= 0
    const gpio_num_t pin = static_cast<gpio_num_t>(STATUS_LED_PIN);
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, !STATUS_LED_ON_LEVEL);
#endif
    g_shown = Shown::Off;
}

void led_notify_rx() { g_last_rx = now_ms(); }

void led_set_mode(bool active, bool bus_off) {
    g_active = active;
    g_bus_off = bus_off;
}

void led_update() {
    const uint32_t now = now_ms();
    if (g_bus_off) {
        show((now / 80) % 2 == 0 ? Shown::BusOff : Shown::Off);   // fast blink
        return;
    }
    const bool recent_rx = (now - g_last_rx) < FLASH_MS;
    // ACTIVE: solid on, briefly off per frame. LISTEN/CLOSED: off, briefly on per frame.
    const bool on = g_active ? !recent_rx : recent_rx;
    show(on ? (g_active ? Shown::Active : Shown::Listen) : Shown::Off);
}
