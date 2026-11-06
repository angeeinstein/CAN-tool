// Hardware pins, defaults and limits. Every value can be overridden with -D in platformio.ini.
#pragma once

#include <Arduino.h>   // pins_arduino.h: RGB_BUILTIN, HIGH
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
// ESP32 DevKit: blue LED on GPIO2. S3/C3 DevKits: on-board RGB LED (driven via RGB_BUILTIN).
// Set STATUS_LED_PIN to -1 to disable.
#ifndef STATUS_LED_PIN
#if defined(RGB_BUILTIN)
#define STATUS_LED_PIN RGB_BUILTIN
#else
#define STATUS_LED_PIN 2
#endif
#endif
#ifndef STATUS_LED_ON
#define STATUS_LED_ON HIGH
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
#ifndef HOST_SERIAL_BAUD
#define HOST_SERIAL_BAUD 921600   // ignored for native USB (S3/C3)
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
#ifndef REPLAY_FRAMES
#define REPLAY_FRAMES 1024        // RAM ring of recent frames for clients that reconnect (32 B each)
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

// ---- WLAN, TCP, web interface, OTA ---------------------------------------------
#ifndef ENABLE_WIFI
#define ENABLE_WIFI 1
#endif
#ifndef TCP_PORT
#define TCP_PORT 3333
#endif
#ifndef MDNS_HOSTNAME
#define MDNS_HOSTNAME "franke-can"
#endif
#ifndef WEB_WS_FLUSH_MS
#define WEB_WS_FLUSH_MS 50                // live frame batches to the browser
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

#define FW_VERSION "1.0.0"
