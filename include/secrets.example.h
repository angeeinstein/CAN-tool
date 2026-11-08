// Copy this file to include/secrets.h (ignored by git) and fill in your values.
// Everything is optional: without secrets.h the device opens its own access point
// "franke-can" and you can enter the WLAN in the web interface (stored in NVS).
#pragma once

// Home WLAN (initial default; WLAN settings saved in the web UI take precedence)
#define WIFI_SSID "your-ssid"
#define WIFI_PASSWORD "your-password"
// Enterprise network (WPA2-Enterprise, e.g. eduroam): also set the login name. Leave "" for a normal WLAN.
// #define WIFI_USERNAME "user@university.example"

// Fallback access point, opened when the WLAN is not reachable for 30 s
#define AP_SSID "franke-can"
#define AP_PASSWORD "change-me-please"   // min. 8 characters

// Login for the web interface, WebSocket and firmware upload (HTTP basic auth)
#define WEB_USER "admin"
#define WEB_PASSWORD "change-me"
