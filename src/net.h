// Network: WLAN management (net_wifi.cpp), mDNS, and the TCP server on port 3333 (net_tcp.cpp) with
// the same protocol as USB.
//
// WLAN behaviour (like a phone):
//   - Up to NET_MAX_SAVED saved networks (NVS "wifi"). Seeded once from secrets.h (WIFI_SSID).
//   - Boot: connect directly to the last used network (no scan, fastest). Otherwise / on failure:
//     scan and connect to the strongest saved network. Connection lost -> immediate retry, then scans.
//   - WPA2-Enterprise (PEAP/TTLS, e.g. eduroam) for networks saved with a username.
//   - The own access point (AP_SSID, 192.168.4.1) is always on. The ESP32 has one radio, so the AP
//     follows the channel of the station connection; while scanning, AP clients may stall briefly.
#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr int NET_MAX_SAVED = 8;
constexpr int NET_MAX_SCAN = 24;

void net_init();   // after nvs_flash_init(), esp_netif_init() and the default event loop
void net_tick();   // main task, every ~200 ms: WLAN state machine

// ---- TCP client (one at a time) ----
// Frames/notifications (reply=false): never blocks, a slow client loses data (counted).
// Command replies (reply=true): waits for the client (up to 2 s per chunk), nothing is lost.
void net_tcp_write(const char* buf, size_t len, bool reply);
bool net_tcp_connected();
uint32_t net_tcp_dropped();   // bytes not sent to the TCP client (it was too slow)

void net_status(char* out, size_t cap);   // short "key=value ..." text for #status (no SSIDs)

// ---- WLAN management (main task) ----
struct NetScanEntry {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool secured;
    bool enterprise;   // WPA2-Enterprise (needs a username, e.g. eduroam)
    bool saved;
};

// save + connect. user empty/null = normal WLAN (pass = PSK or empty for open),
// user set = WPA2-Enterprise (PEAP/TTLS): user = login (also sent as identity), pass = its password.
bool net_wifi_add(const char* ssid, const char* pass, const char* user, char* err, size_t errcap);
bool net_wifi_delete(const char* ssid);
bool net_wifi_connect(const char* ssid);
bool net_wifi_scan_start();                   // asynchronous
int net_wifi_scan_result_count();             // -1 = running, -2 = none yet / failed, >= 0 results
const NetScanEntry* net_wifi_scan_entry(int i);
int net_wifi_saved_count();
const char* net_wifi_saved_ssid(int i);
bool net_wifi_saved_enterprise(int i);
const char* net_wifi_saved_user(int i);        // enterprise login, "" for normal WLANs
bool net_wifi_saved_rejected(int i);
const char* net_wifi_saved_pass(int i);        // only for "#wifi show" over USB           // enterprise login refused: not retried automatically
const char* net_wifi_current_ssid();          // "" if not connected
// "connected <rssi> <ip> <ssid>" | "connecting - - <ssid>" | "scanning - - -" | "idle - - -"
void net_wifi_state(char* out, size_t cap);

// internal: TCP part of net_init()/net_status()
void net_tcp_init();
void net_tcp_status(char* out, size_t cap);
