// WLAN management, mDNS and the TCP server (port 3333) with the same protocol as USB.
// Compiled to no-ops when ENABLE_WIFI=0.
//
// WLAN behaviour (like a phone):
//   - Up to NET_MAX_SAVED saved networks (NVS). Seeded once from secrets.h (WIFI_SSID).
//   - Boot: connect directly to the last used network (no scan, fastest). Otherwise / on failure:
//     scan and connect to the strongest saved network. Connection lost -> immediate retry, then scans.
//   - The own access point (AP_SSID, 192.168.4.1) is always on. The ESP32 has one radio, so the AP
//     follows the channel of the station connection; while scanning, AP clients may stall briefly.
#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr int NET_MAX_SAVED = 8;
constexpr int NET_MAX_SCAN = 24;

void net_init();
void net_loop();                              // loop task: WLAN state machine, TCP client I/O
void net_write(const char* buf, size_t len);  // output task: non-blocking send to the TCP client
void net_status(char* out, size_t cap);       // short "key=value ..." text for #status (no SSIDs)

// ---- network management (loop task) ----
struct NetScanEntry {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool secured;
    bool saved;
};

bool net_wifi_add(const char* ssid, const char* pass, char* err, size_t errcap);  // save + connect
bool net_wifi_delete(const char* ssid);
bool net_wifi_connect(const char* ssid);
bool net_wifi_scan_start();                   // asynchronous
int net_wifi_scan_result_count();             // -1 = running, -2 = none yet / failed, >= 0 results
const NetScanEntry* net_wifi_scan_entry(int i);
int net_wifi_saved_count();
const char* net_wifi_saved_ssid(int i);
const char* net_wifi_current_ssid();          // "" if not connected
// "connected <rssi> <ip> <ssid>" | "connecting - - <ssid>" | "scanning - - -" | "idle - - -"
void net_wifi_state(char* out, size_t cap);
