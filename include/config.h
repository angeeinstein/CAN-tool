// Hardware pins, defaults and limits. Every value can be overridden with -D in platformio.ini.
#pragma once

#include <sdkconfig.h>

// ---- CAN transceiver (SN65HVD230) --------------------------------------
// Classic ESP32: GPIO 13/27 are neither strapping pins nor active during boot, so the bus sees no
// glitch at power-up (GPIO 5, 12, 14, 15 would). S3/C3: GPIO 5/4 (check your board before wiring).
#if defined(CONFIG_IDF_TARGET_ESP32)
#ifndef CAN_TX_PIN
#define CAN_TX_PIN 13  // ESP32 -> CTX
#endif
#ifndef CAN_RX_PIN
#define CAN_RX_PIN 27  // ESP32 <- CRX
#endif
#else
#ifndef CAN_TX_PIN
#define CAN_TX_PIN 5   // ESP32 -> CTX
#endif
#ifndef CAN_RX_PIN
#define CAN_RX_PIN 4   // ESP32 <- CRX
#endif
#endif

#ifndef CAN_DEFAULT_BITRATE
#define CAN_DEFAULT_BITRATE 125000
#endif

// ---- Status LED ----------------------------------------------------------
// ESP32 DevKit: blue LED on GPIO2 (plain GPIO). S3/C3 DevKits: on-board addressable RGB LED (WS2812),
// S3-DevKitC-1 v1.0 on GPIO48 (v1.1: GPIO38), C3-DevKitM-1 on GPIO8. Set STATUS_LED_PIN to -1 to disable.
#ifndef STATUS_LED_PIN
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define STATUS_LED_PIN 48
#define STATUS_LED_RGB 1
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#define STATUS_LED_PIN 8
#define STATUS_LED_RGB 1
#else
#define STATUS_LED_PIN 2
#endif
#endif
#ifndef STATUS_LED_RGB
#define STATUS_LED_RGB 0          // 1 = addressable RGB LED, 0 = plain GPIO LED
#endif
#ifndef STATUS_LED_ON_LEVEL
#define STATUS_LED_ON_LEVEL 1     // plain GPIO LED: level for "on"
#endif

// ---- Emergency-stop button (BOOT button, active low) --------------------
#ifndef ESTOP_BUTTON_PIN
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#define ESTOP_BUTTON_PIN 9
#else
#define ESTOP_BUTTON_PIN 0
#endif
#endif

// ---- Host link -------------------------------------------------------------
// Classic ESP32: UART0 through the USB bridge. S3/C3: the chip's USB-Serial/JTAG port (no baud rate).
#ifndef HOST_SERIAL_BAUD
#define HOST_SERIAL_BAUD 921600
#endif

// ---- Queues / limits -------------------------------------------------------
#ifndef TWAI_RX_QUEUE_LEN
#define TWAI_RX_QUEUE_LEN 128     // driver RX queue (spec: >= 64)
#endif
#ifndef TWAI_TX_QUEUE_LEN
#define TWAI_TX_QUEUE_LEN 16
#endif
#ifndef HOST_FRAME_QUEUE_LEN
#define HOST_FRAME_QUEUE_LEN 256  // frames waiting for USB/TCP output (~190 ms at a fully loaded bus)
#endif
#ifndef HOST_MSG_QUEUE_LEN
#define HOST_MSG_QUEUE_LEN 24     // asynchronous "#..." notifications
#endif
#ifndef CMD_QUEUE_LEN
#define CMD_QUEUE_LEN 12          // command lines from USB/TCP/web waiting for the main task
#endif
#ifndef REPLAY_FRAMES
#define REPLAY_FRAMES 512         // RAM ring of recent frames for clients that reconnect (32 B each)
#endif
#ifndef REPLAY_MSGS
#define REPLAY_MSGS 32            // recent notifications (markers, #stop ...), ~208 B each
#endif
#ifndef TX_RATE_LIMIT_DEFAULT
#define TX_RATE_LIMIT_DEFAULT 50  // max transmitted frames per second, exceeding -> emergency stop
#endif
#ifndef RULE_PENDING_MAX
#define RULE_PENDING_MAX 48       // scheduled (delayed) rule actions
#endif
#ifndef CAN_ACTIVE_WATCHDOG_S
// If the TCP/web client that enabled ACTIVE disconnects and does not come back within this many
// seconds, the firmware reverts to LISTEN and disables all rules (not latched). 0 = off.
// Runtime override: "#watchdog <secs>". USB (SINK_SERIAL) is never watched.
#define CAN_ACTIVE_WATCHDOG_S 30
#endif

// ---- WLAN, TCP, web interface, OTA ---------------------------------------------
#ifndef TCP_PORT
#define TCP_PORT 3333
#endif
#ifndef MDNS_HOSTNAME
#define MDNS_HOSTNAME "franke-can"
#endif
#ifndef WEB_MAX_CLIENTS
#define WEB_MAX_CLIENTS 4                 // authenticated browsers at the same time
#endif
#ifndef WEB_WS_FLUSH_MS
#define WEB_WS_FLUSH_MS 100               // live frame batches to the browsers (10/s)
#endif
#ifndef WEB_SEND_TIMEOUT_S
// A browser that does not take a batch within this time is disconnected (it reconnects and fetches the
// missed frames with #replay). Memory use is fixed: nothing is queued per browser.
#define WEB_SEND_TIMEOUT_S 3
#endif
#ifndef HEALTH_BUSY_FPS
#define HEALTH_BUSY_FPS 100               // automatic "#health" every 5 s while rx is above this rate
#endif
#ifndef APP_VALID_AFTER_S
// A new firmware is confirmed (no rollback any more) after running this long without a crash.
#define APP_VALID_AFTER_S 30
#endif

// Credentials: include/secrets.h (optional, not in git). Defaults below are used without it.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""                      // seeds the saved-network list on first boot (optional)
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef WIFI_USERNAME
#define WIFI_USERNAME ""                  // set = WPA2-Enterprise (PEAP/TTLS, e.g. eduroam) with this login
#endif
#ifndef AP_SSID
#define AP_SSID "franke-can"
#endif
#ifndef AP_PASSWORD
#define AP_PASSWORD "franke-can"          // min. 8 characters
#endif
#ifndef WEB_USER
#define WEB_USER "admin"
#endif
#ifndef WEB_PASSWORD
#define WEB_PASSWORD "franke"             // protects web UI, WebSocket and OTA upload
#endif

#define FW_VERSION "2.0.0"
