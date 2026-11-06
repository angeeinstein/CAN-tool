#include "net.h"

#include "config.h"

#if ENABLE_WIFI

#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>

#include "commands.h"
#include "host_io.h"

namespace {

// ---- saved networks ------------------------------------------------------------------
struct Saved {
    char ssid[33];
    char pass[65];
};
Saved g_saved[NET_MAX_SAVED];
int g_saved_n = 0;
char g_last[33] = {0};   // last successfully used network

// ---- scan results ---------------------------------------------------------------------
NetScanEntry g_scan[NET_MAX_SCAN];
uint8_t g_scan_bssid[NET_MAX_SCAN][6];
int g_scan_n = -2;

// ---- connection state machine -------------------------------------------------------------
enum class St : uint8_t { Idle, Connecting, Connected, Scanning };
St g_st = St::Idle;
char g_cur[33] = {0};        // network being connected / connected
uint32_t g_attempt_start = 0;
uint32_t g_next_try = 0;
uint8_t g_failures = 0;
bool g_scan_auto = false;    // scan started by the reconnect logic (connect afterwards)
constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;
volatile uint8_t g_disc_reason = 0;   // last STA disconnect reason from the WiFi driver (0 = none)

// ---- TCP ------------------------------------------------------------------------------
WiFiServer g_server(TCP_PORT);
WiFiClient g_client;
SemaphoreHandle_t g_client_mtx = nullptr;
volatile int g_fd = -1;
volatile bool g_drop_client = false;
volatile uint32_t g_tcp_dropped = 0;
LineBuffer g_line;

// ---- NVS --------------------------------------------------------------------------------
void save_all() {
    Preferences p;
    if (!p.begin("wifi", false)) return;
    p.putUChar("n", static_cast<uint8_t>(g_saved_n));
    char k[4];
    for (int i = 0; i < NET_MAX_SAVED; i++) {
        snprintf(k, sizeof(k), "s%d", i);
        if (i < g_saved_n) p.putString(k, g_saved[i].ssid);
        else p.remove(k);
        k[0] = 'p';
        if (i < g_saved_n) p.putString(k, g_saved[i].pass);
        else p.remove(k);
    }
    p.putString("last", g_last);
    p.end();
}

void save_last() {
    Preferences p;
    if (p.begin("wifi", false)) {
        p.putString("last", g_last);
        p.end();
    }
}

int find_saved(const char* ssid) {
    for (int i = 0; i < g_saved_n; i++)
        if (strcmp(g_saved[i].ssid, ssid) == 0) return i;
    return -1;
}

bool add_saved(const char* ssid, const char* pass) {
    int i = find_saved(ssid);
    if (i < 0) {
        if (g_saved_n >= NET_MAX_SAVED) return false;
        i = g_saved_n++;
    }
    snprintf(g_saved[i].ssid, sizeof(g_saved[i].ssid), "%s", ssid);
    snprintf(g_saved[i].pass, sizeof(g_saved[i].pass), "%s", pass ? pass : "");
    return true;
}

void load_all() {
    Preferences p;
    if (!p.begin("wifi", false)) return;
    if (!p.isKey("n")) {
        // first boot with this firmware: seed from the old single entry and from secrets.h
        String old_s = p.getString("ssid", ""), old_p = p.getString("pass", "");
        p.end();
        if (old_s.length()) add_saved(old_s.c_str(), old_p.c_str());
        if (strlen(WIFI_SSID)) add_saved(WIFI_SSID, WIFI_PASSWORD);
        save_all();
        return;
    }
    g_saved_n = p.getUChar("n", 0);
    if (g_saved_n > NET_MAX_SAVED) g_saved_n = NET_MAX_SAVED;
    char k[4];
    for (int i = 0; i < g_saved_n; i++) {
        snprintf(k, sizeof(k), "s%d", i);
        snprintf(g_saved[i].ssid, sizeof(g_saved[i].ssid), "%s", p.getString(k, "").c_str());
        k[0] = 'p';
        snprintf(g_saved[i].pass, sizeof(g_saved[i].pass), "%s", p.getString(k, "").c_str());
    }
    snprintf(g_last, sizeof(g_last), "%s", p.getString("last", "").c_str());
    p.end();
}

// ---- connection ------------------------------------------------------------------------------
void connect_to(int idx, int scan_idx = -1) {
    if (idx < 0 || idx >= g_saved_n) return;
    if (g_st == St::Connecting || g_st == St::Connected) WiFi.disconnect(false);
    snprintf(g_cur, sizeof(g_cur), "%s", g_saved[idx].ssid);
    const char* pass = g_saved[idx].pass[0] ? g_saved[idx].pass : nullptr;
    if (scan_idx >= 0)   // channel + BSSID known from the scan -> faster, and picks the strongest AP
        WiFi.begin(g_saved[idx].ssid, pass, g_scan[scan_idx].channel, g_scan_bssid[scan_idx]);
    else
        WiFi.begin(g_saved[idx].ssid, pass);
    g_st = St::Connecting;
    g_disc_reason = 0;
    g_attempt_start = millis();
    if (g_failures == 0) host_notify("#wifi connecting to '%s'", g_cur);   // first attempt of a streak only
}

int g_rr = 0;   // round robin for direct connects without scan
bool g_reported_none = false;

// Scan not usable: connect directly (WiFi.begin scans internally for that one SSID).
void connect_direct_next() {
    if (g_saved_n == 0) return;
    connect_to(g_rr++ % g_saved_n);
}

uint32_t retry_delay() {
    const bool ap_in_use = WiFi.softAPgetStationNum() > 0;   // scans disturb AP clients
    if (g_failures < 5) return ap_in_use ? 15000 : 3000;
    return ap_in_use ? 60000 : 20000;
}

void start_scan(bool auto_connect) {
    if (g_st == St::Connecting) WiFi.disconnect(false);   // a scan is not possible while connecting
    if (g_st != St::Connected) g_st = St::Scanning;
    g_scan_auto = auto_connect;
    g_scan_n = -1;
    WiFi.scanDelete();
    // 300 ms per channel: the Arduino library declares a scan failed after 20x this (6 s). With the AP on,
    // the radio returns to the AP channel between scan channels, so scans take longer than without AP.
    if (WiFi.scanNetworks(true, false, false, 300) == WIFI_SCAN_FAILED) {
        g_scan_n = -2;
        if (g_st == St::Scanning) g_st = St::Idle;
        if (auto_connect && g_st == St::Idle) connect_direct_next();
    }
}

void finish_scan(int count) {
    // collect, keep only the strongest BSSID per SSID, sort by RSSI
    int n = 0;
    for (int i = 0; i < count; i++) {
        String s = WiFi.SSID(i);
        if (s.length() == 0) continue;
        const int8_t rssi = static_cast<int8_t>(WiFi.RSSI(i));
        int j = 0;
        while (j < n && strcmp(g_scan[j].ssid, s.c_str()) != 0) j++;
        if (j < n && g_scan[j].rssi >= rssi) continue;
        if (j == n) {
            if (n >= NET_MAX_SCAN) continue;
            n++;
        }
        snprintf(g_scan[j].ssid, sizeof(g_scan[j].ssid), "%s", s.c_str());
        g_scan[j].rssi = rssi;
        g_scan[j].channel = static_cast<uint8_t>(WiFi.channel(i));
        g_scan[j].secured = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        g_scan[j].saved = find_saved(g_scan[j].ssid) >= 0;
        memcpy(g_scan_bssid[j], WiFi.BSSID(i), 6);
    }
    WiFi.scanDelete();
    for (int a = 1; a < n; a++)   // insertion sort by RSSI, strongest first
        for (int b = a; b > 0 && g_scan[b].rssi > g_scan[b - 1].rssi; b--) {
            std::swap(g_scan[b], g_scan[b - 1]);
            uint8_t t[6];
            memcpy(t, g_scan_bssid[b], 6);
            memcpy(g_scan_bssid[b], g_scan_bssid[b - 1], 6);
            memcpy(g_scan_bssid[b - 1], t, 6);
        }
    g_scan_n = n;

    if (g_st == St::Scanning) g_st = St::Idle;
    if (g_st != St::Idle) return;
    // not connected: connect to the strongest saved network that is in range
    for (int j = 0; j < n; j++) {
        const int idx = find_saved(g_scan[j].ssid);
        if (idx >= 0) {
            g_reported_none = false;
            connect_to(idx, j);
            return;
        }
    }
    if (g_saved_n > 0 && !g_reported_none) {
        g_reported_none = true;
        host_notify("#wifi no saved network in range (%d networks visible), retrying", n);
    }
    if (g_scan_auto) g_next_try = millis() + retry_delay();
}

void wifi_state_machine() {
    const uint32_t now = millis();
    const bool up = WiFi.status() == WL_CONNECTED;

    // scan finished?
    if (g_scan_n == -1) {
        const int c = WiFi.scanComplete();
        if (c >= 0) finish_scan(c);
        else if (c == WIFI_SCAN_FAILED) {
            esp_wifi_scan_stop();   // the library only gave up waiting; make sure the radio stops too
            WiFi.scanDelete();
            g_scan_n = -2;
            if (g_st == St::Scanning) {
                g_st = St::Idle;
                // don't depend on scans: try the saved networks directly, one after the other
                if (g_scan_auto) connect_direct_next();
                else g_next_try = now + retry_delay();
            }
        }
    }

    switch (g_st) {
        case St::Connected:
            if (!up) {
                host_notify("#wifi connection to '%s' lost, reconnecting", g_cur);
                // fast path: retry the same network directly
                const int idx = find_saved(g_cur);
                g_st = St::Idle;
                if (idx >= 0) connect_to(idx);
            }
            break;
        case St::Connecting:
            if (up) {
                g_st = St::Connected;
                g_failures = 0;
                if (strcmp(g_last, g_cur) != 0) {
                    snprintf(g_last, sizeof(g_last), "%s", g_cur);
                    save_last();
                }
                host_notify("#wifi connected '%s' ip=%s rssi=%d -> http://%s.local", g_cur,
                            WiFi.localIP().toString().c_str(), WiFi.RSSI(), MDNS_HOSTNAME);
            } else if (static_cast<int32_t>(millis() - g_attempt_start) > static_cast<int32_t>(CONNECT_TIMEOUT_MS)) {
                // note: millis(), not "now" - the attempt may have started after "now" was taken
                WiFi.disconnect(false);
                g_st = St::Idle;
                if (g_failures < 255) g_failures++;
                g_next_try = millis();   // scan right away: maybe another saved network is in range
                const uint8_t r = g_disc_reason;
                if (r)
                    host_notify("#wifi could not connect to '%s' (reason %u: %s)", g_cur, r,
                                WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(r)));
                else
                    host_notify("#wifi could not connect to '%s' (timeout)", g_cur);
            }
            break;
        case St::Idle:
            if (g_saved_n > 0 && static_cast<int32_t>(now - g_next_try) >= 0 && g_scan_n != -1) start_scan(true);
            break;
        case St::Scanning:
            break;
    }
}

void tcp_loop() {
    if (g_server.hasClient()) {
        WiFiClient incoming = g_server.available();
        if (g_fd >= 0 && g_client.connected()) {
            incoming.print("#err only one TCP client allowed\n\r");
            incoming.stop();
        } else {
            xSemaphoreTake(g_client_mtx, portMAX_DELAY);
            g_client.stop();
            g_client = incoming;
            g_client.setNoDelay(true);
            g_fd = g_client.fd();
            g_drop_client = false;
            xSemaphoreGive(g_client_mtx);
            g_line.reset();
        }
    }

    if (g_fd < 0) return;

    if (g_drop_client || !g_client.connected()) {
        xSemaphoreTake(g_client_mtx, portMAX_DELAY);
        g_fd = -1;
        g_client.stop();
        xSemaphoreGive(g_client_mtx);
        return;
    }

    int budget = 256;
    while (budget-- > 0 && g_client.available()) {
        int c = g_client.read();
        if (c < 0) break;
        if (g_line.feed(static_cast<char>(c))) cmd_handle_line(g_line.line(), SINK_TCP);
    }
}

}  // namespace

void net_init() {
    g_client_mtx = xSemaphoreCreateMutex();
    load_all();
    WiFi.persistent(false);          // credentials live in our own NVS namespace
    WiFi.setAutoReconnect(false);    // reconnects are handled by wifi_state_machine()
    WiFi.setHostname(MDNS_HOSTNAME);
    WiFi.mode(WIFI_AP_STA);          // AP is always on
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    WiFi.setSleep(false);            // lower latency for the live view
    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
        g_disc_reason = info.wifi_sta_disconnected.reason;   // e.g. 15 = wrong password, 201 = not found
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

    // fastest start: the last used network directly, without scanning
    const int idx = find_saved(g_last);
    if (idx >= 0) connect_to(idx);
    else g_next_try = millis();      // scan immediately

    if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        MDNS.addService("slcan", "tcp", TCP_PORT);
    }
    g_server.begin();
    g_server.setNoDelay(true);
}

void net_loop() {
    static uint32_t last = 0;
    if (millis() - last >= 200) {
        last = millis();
        wifi_state_machine();
    }
    tcp_loop();
}

void net_write(const char* buf, size_t len) {
    if (g_fd < 0 || !g_client_mtx) return;
    if (xSemaphoreTake(g_client_mtx, 0) != pdTRUE) return;  // never wait in the output path
    const int fd = g_fd;
    if (fd >= 0) {
        // MSG_DONTWAIT: a stalled client must never block the output task (and thus USB)
        ssize_t n = send(fd, buf, len, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) g_tcp_dropped += len;
            else g_drop_client = true;
        } else if (static_cast<size_t>(n) < len) {
            g_tcp_dropped += len - static_cast<size_t>(n);
        }
    }
    xSemaphoreGive(g_client_mtx);
}

void net_status(char* out, size_t cap) {
    const char* st = g_st == St::Connected ? "connected" : g_st == St::Connecting ? "connecting"
                     : g_st == St::Scanning ? "scanning" : (g_saved_n ? "idle" : "unconfigured");
    int n = snprintf(out, cap, "wifi=%s", st);
    if (g_st == St::Connected && n > 0 && static_cast<size_t>(n) < cap)
        n += snprintf(out + n, cap - n, " ip=%s rssi=%d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    if (n > 0 && static_cast<size_t>(n) < cap)
        snprintf(out + n, cap - n, " ap_ip=%s ap_clients=%d saved=%d tcp_client=%s tcp_dropped=%lu",
                 WiFi.softAPIP().toString().c_str(), WiFi.softAPgetStationNum(), g_saved_n,
                 g_fd >= 0 ? "yes" : "no", (unsigned long)g_tcp_dropped);
}

bool net_wifi_add(const char* ssid, const char* pass, char* err, size_t errcap) {
    const size_t sl = strlen(ssid), pl = pass ? strlen(pass) : 0;
    if (sl == 0 || sl > 32) {
        snprintf(err, errcap, "SSID must be 1..32 characters");
        return false;
    }
    if (pl > 64 || (pl > 0 && pl < 8)) {
        snprintf(err, errcap, "password must be empty (open network) or 8..64 characters");
        return false;
    }
    if (!add_saved(ssid, pass)) {
        snprintf(err, errcap, "max. %d saved networks, delete one first", NET_MAX_SAVED);
        return false;
    }
    save_all();
    g_failures = 0;
    connect_to(find_saved(ssid));
    return true;
}

bool net_wifi_delete(const char* ssid) {
    const int i = find_saved(ssid);
    if (i < 0) return false;
    for (int j = i; j < g_saved_n - 1; j++) g_saved[j] = g_saved[j + 1];
    g_saved_n--;
    if (strcmp(g_last, ssid) == 0) g_last[0] = '\0';
    save_all();
    for (int j = 0; j < (g_scan_n > 0 ? g_scan_n : 0); j++)
        if (strcmp(g_scan[j].ssid, ssid) == 0) g_scan[j].saved = false;
    if (strcmp(g_cur, ssid) == 0 && (g_st == St::Connected || g_st == St::Connecting)) {
        WiFi.disconnect(false);
        g_st = St::Idle;
        g_next_try = millis();   // look for another saved network
    }
    return true;
}

bool net_wifi_connect(const char* ssid) {
    const int i = find_saved(ssid);
    if (i < 0) return false;
    g_failures = 0;
    int scan_idx = -1;
    for (int j = 0; j < (g_scan_n > 0 ? g_scan_n : 0); j++)
        if (strcmp(g_scan[j].ssid, ssid) == 0) scan_idx = j;
    connect_to(i, scan_idx);
    return true;
}

bool net_wifi_scan_start() {
    if (g_scan_n == -1) return true;   // already running
    start_scan(false);
    return g_scan_n == -1;
}

int net_wifi_scan_result_count() { return g_scan_n; }
const NetScanEntry* net_wifi_scan_entry(int i) { return (i >= 0 && i < g_scan_n) ? &g_scan[i] : nullptr; }
int net_wifi_saved_count() { return g_saved_n; }
const char* net_wifi_saved_ssid(int i) { return (i >= 0 && i < g_saved_n) ? g_saved[i].ssid : ""; }
const char* net_wifi_current_ssid() { return g_st == St::Connected ? g_cur : ""; }

void net_wifi_state(char* out, size_t cap) {
    switch (g_st) {
        case St::Connected:
            snprintf(out, cap, "connected %d %s %s", WiFi.RSSI(), WiFi.localIP().toString().c_str(), g_cur);
            break;
        case St::Connecting: snprintf(out, cap, "connecting - - %s", g_cur); break;
        case St::Scanning: snprintf(out, cap, "scanning - - -"); break;
        default: snprintf(out, cap, "%s - - -", g_saved_n ? "idle" : "unconfigured"); break;
    }
}

#else  // !ENABLE_WIFI

#include <stdio.h>

void net_init() {}
void net_loop() {}
void net_write(const char*, size_t) {}
void net_status(char* out, size_t cap) { snprintf(out, cap, "wifi=disabled"); }
bool net_wifi_add(const char*, const char*, char* err, size_t cap) {
    snprintf(err, cap, "WLAN disabled in this build");
    return false;
}
bool net_wifi_delete(const char*) { return false; }
bool net_wifi_connect(const char*) { return false; }
bool net_wifi_scan_start() { return false; }
int net_wifi_scan_result_count() { return -2; }
const NetScanEntry* net_wifi_scan_entry(int) { return nullptr; }
int net_wifi_saved_count() { return 0; }
const char* net_wifi_saved_ssid(int) { return ""; }
const char* net_wifi_current_ssid() { return ""; }
void net_wifi_state(char* out, size_t cap) { snprintf(out, cap, "disabled - - -"); }

#endif
