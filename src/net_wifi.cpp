// WLAN management: AP + STA, saved networks, scan, connect state machine, mDNS.
// All management functions and net_tick() run in the main task; the WLAN event handler (event loop
// task) only records what happened in a few volatile flags, so there is no shared-state locking.
#include <esp_eap_client.h>
#include <esp_event.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mdns.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <utility>

#include "config.h"
#include "host_io.h"
#include "net.h"
#include "sys.h"

namespace {

// ---- saved networks ------------------------------------------------------------------
struct Saved {
    char ssid[33];
    char pass[65];
    char user[65];   // "" = normal WLAN; set = WPA2-Enterprise login
    bool rejected;   // enterprise login refused by the server: no automatic retries (RAM only)
};
Saved g_saved[NET_MAX_SAVED];
int g_saved_n = 0;
char g_last[33] = {0};   // last successfully used network

// ---- scan results ---------------------------------------------------------------------
NetScanEntry g_scan[NET_MAX_SCAN];
uint8_t g_scan_bssid[NET_MAX_SCAN][6];
int g_scan_n = -2;           // -1 running, -2 none / failed, >= 0 results
uint32_t g_scan_started = 0;
constexpr uint32_t SCAN_TIMEOUT_MS = 15000;
constexpr int SCAN_RECORDS_MAX = 32;   // AP records fetched from the driver (temporary heap)

// ---- connection state machine -------------------------------------------------------------
enum class St : uint8_t { Idle, Connecting, Connected, Scanning };
St g_st = St::Idle;
char g_cur[33] = {0};        // network being connected / connected
uint32_t g_attempt_start = 0;
uint32_t g_next_try = 0;
uint8_t g_failures = 0;
bool g_scan_auto = false;    // scan started by the reconnect logic (connect afterwards)
bool g_reported_none = false;
int g_rr = 0;                // round robin for direct connects without a usable scan
constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t CONNECT_TIMEOUT_EAP_MS = 30000;   // enterprise: TLS handshake with the RADIUS server
uint32_t g_attempt_disc = 0;   // g_disc_count when the current attempt started

// set by the event handler (event loop task)
volatile bool g_sta_up = false;          // associated and got an IP
volatile bool g_scan_done = false;
volatile uint8_t g_disc_reason = 0;      // last STA disconnect reason (0 = none)
std::atomic<uint32_t> g_disc_count{0};   // number of STA disconnect events (an attempt ends with one)

esp_netif_t* g_sta_netif = nullptr;
esp_netif_t* g_ap_netif = nullptr;

const char* reason_name(uint8_t r) {
    switch (r) {
        case WIFI_REASON_UNSPECIFIED: return "unspecified (AP refused / busy)";
        case WIFI_REASON_NOT_AUTHED: return "not authenticated";
        case WIFI_REASON_NOT_ASSOCED: return "not associated";
        case WIFI_REASON_ASSOC_TOOMANY: return "AP has too many clients";
        case WIFI_REASON_MIC_FAILURE: return "MIC failure";
        case WIFI_REASON_AUTH_EXPIRE: return "auth expired";
        case WIFI_REASON_AUTH_LEAVE: return "auth leave";
        case WIFI_REASON_ASSOC_LEAVE: return "assoc leave";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4-way handshake timeout (wrong password?)";
        case WIFI_REASON_802_1X_AUTH_FAILED: return "802.1X auth failed (wrong username/password?)";
        case WIFI_REASON_BEACON_TIMEOUT: return "beacon timeout";
        case WIFI_REASON_NO_AP_FOUND: return "network not found";
        case WIFI_REASON_AUTH_FAIL: return "auth failed";
        case WIFI_REASON_ASSOC_FAIL: return "assoc failed";
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "handshake timeout (wrong password?)";
        case WIFI_REASON_CONNECTION_FAIL: return "connection failed";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "no AP with compatible security";
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "no AP with the required security";
        default: return "see ESP-IDF wifi_err_reason_t";
    }
}

void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            g_sta_up = false;
            g_disc_reason = static_cast<wifi_event_sta_disconnected_t*>(data)->reason;
            g_disc_count++;
        } else if (id == WIFI_EVENT_SCAN_DONE) {
            g_scan_done = true;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        g_sta_up = true;
    }
}

// ---- NVS (namespace "wifi"; same keys as the firmware before) ----------------------------
void save_all() {
    Nvs p("wifi", true);
    if (!p.ok()) return;
    p.set_u8("n", static_cast<uint8_t>(g_saved_n));
    char k[12];   // "s<index>" - room for any int, keeps -Wformat-truncation quiet
    for (int i = 0; i < NET_MAX_SAVED; i++) {
        snprintf(k, sizeof(k), "s%d", i);
        if (i < g_saved_n) p.set_str(k, g_saved[i].ssid);
        else p.erase(k);
        k[0] = 'p';
        if (i < g_saved_n) p.set_str(k, g_saved[i].pass);
        else p.erase(k);
        k[0] = 'u';
        if (i < g_saved_n && g_saved[i].user[0]) p.set_str(k, g_saved[i].user);
        else p.erase(k);
    }
    p.set_str("last", g_last);
}

void save_last() {
    Nvs p("wifi", true);
    p.set_str("last", g_last);
}

int find_saved(const char* ssid) {
    for (int i = 0; i < g_saved_n; i++)
        if (strcmp(g_saved[i].ssid, ssid) == 0) return i;
    return -1;
}

bool add_saved(const char* ssid, const char* pass, const char* user) {
    int i = find_saved(ssid);
    if (i < 0) {
        if (g_saved_n >= NET_MAX_SAVED) return false;
        i = g_saved_n++;
    }
    snprintf(g_saved[i].ssid, sizeof(g_saved[i].ssid), "%s", ssid);
    snprintf(g_saved[i].pass, sizeof(g_saved[i].pass), "%s", pass ? pass : "");
    snprintf(g_saved[i].user, sizeof(g_saved[i].user), "%s", user ? user : "");
    g_saved[i].rejected = false;
    return true;
}

void load_all() {
    bool seed = false;
    {
        Nvs p("wifi", false);
        if (!p.has("n")) {
            seed = true;   // first boot: seed from secrets.h (and the single entry of very old firmware)
            char s[33], pw[65];
            p.get_str("ssid", s, sizeof(s));
            p.get_str("pass", pw, sizeof(pw));
            if (s[0]) add_saved(s, pw, "");
        } else {
            g_saved_n = p.get_u8("n", 0);
            if (g_saved_n > NET_MAX_SAVED) g_saved_n = NET_MAX_SAVED;
            char k[12];   // "s<index>" - room for any int, keeps -Wformat-truncation quiet
            for (int i = 0; i < g_saved_n; i++) {
                snprintf(k, sizeof(k), "s%d", i);
                p.get_str(k, g_saved[i].ssid, sizeof(g_saved[i].ssid));
                k[0] = 'p';
                p.get_str(k, g_saved[i].pass, sizeof(g_saved[i].pass));
                k[0] = 'u';   // missing on networks saved by older firmware -> "" (normal WLAN)
                p.get_str(k, g_saved[i].user, sizeof(g_saved[i].user));
            }
            p.get_str("last", g_last, sizeof(g_last));
        }
    }
    if (seed) {
        if (strlen(WIFI_SSID)) add_saved(WIFI_SSID, WIFI_PASSWORD, WIFI_USERNAME);
        save_all();
    }
}

// ---- connection ------------------------------------------------------------------------------
// Ends a running connection (or attempt) and a running scan, so the driver accepts a new connect.
// esp_wifi_disconnect() only starts the disconnect; wait for its event (max. 1 s).
void stop_sta_activity() {
    if (g_scan_n == -1) {   // a scan blocks connecting
        esp_wifi_scan_stop();
        esp_wifi_clear_ap_list();
        g_scan_n = -2;
        g_scan_done = false;
        if (g_st == St::Scanning) g_st = St::Idle;
    }
    if (g_st == St::Connecting || g_st == St::Connected) {
        const uint32_t before = g_disc_count;
        if (esp_wifi_disconnect() == ESP_OK)
            for (int i = 0; i < 100 && g_disc_count == before; i++) vTaskDelay(pdMS_TO_TICKS(10));
        g_st = St::Idle;
    }
}

void attempt_failed(const char* why, uint32_t retry_ms) {
    g_st = St::Idle;
    if (g_failures < 255) g_failures++;
    g_next_try = now_ms() + retry_ms;
    const int idx = find_saved(g_cur);
    if (idx >= 0 && g_saved[idx].user[0])   // enterprise: the TLS login needs a lot of heap - show it
        host_notify("#wifi could not connect to '%s' (%s) heap_free=%lu heap_min=%lu heap_max_block=%lu", g_cur, why,
                    (unsigned long)heap_free(), (unsigned long)heap_min(), (unsigned long)heap_largest());
    else
        host_notify("#wifi could not connect to '%s' (%s)", g_cur, why);
}

void connect_to(int idx, int scan_idx = -1) {
    if (idx < 0 || idx >= g_saved_n) return;
    stop_sta_activity();
    const Saved& s = g_saved[idx];
    snprintf(g_cur, sizeof(g_cur), "%s", s.ssid);

    wifi_config_t c = {};
    memcpy(c.sta.ssid, s.ssid, strnlen(s.ssid, sizeof(c.sta.ssid)));
    if (scan_idx >= 0) {   // channel + BSSID known from the scan -> faster, and it is the strongest AP
        c.sta.channel = g_scan[scan_idx].channel;
        c.sta.bssid_set = true;
        memcpy(c.sta.bssid, g_scan_bssid[scan_idx], 6);
    } else {               // let the driver look on all channels and take the strongest AP of this network
        c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        c.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    }
    // every step is checked: a refused call would otherwise only show up as a silent timeout
    const char* step = "set_config";
    esp_err_t e;
    if (s.user[0]) {
        // the TLS login needs ~40 KB of heap at its peak; the replay ring is not needed while the station
        // connection is down anyway (clients on the old network are gone) -> lend it to the login
        host_replay_release();
        // WPA2-Enterprise (PEAP/TTLS), same as the proven display firmware: login as identity and
        // username, no CA certificate (server not verified), certificate time check off (no clock yet).
        c.sta.threshold.authmode = WIFI_AUTH_WPA2_ENTERPRISE;
        e = esp_wifi_set_config(WIFI_IF_STA, &c);
        if (e == ESP_OK) {
            step = "eap credentials";
            e = esp_eap_client_set_identity(reinterpret_cast<const unsigned char*>(s.user), strlen(s.user));
        }
        if (e == ESP_OK)
            e = esp_eap_client_set_username(reinterpret_cast<const unsigned char*>(s.user), strlen(s.user));
        if (e == ESP_OK)
            e = esp_eap_client_set_password(reinterpret_cast<const unsigned char*>(s.pass), strlen(s.pass));
        if (e == ESP_OK) e = esp_eap_client_set_disable_time_check(true);
        if (e == ESP_OK) {
            step = "enterprise_enable";
            e = esp_wifi_sta_enterprise_enable();
        }
    } else {
        memcpy(c.sta.password, s.pass, strnlen(s.pass, sizeof(c.sta.password)));
        c.sta.threshold.authmode = s.pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
        esp_wifi_sta_enterprise_disable();   // stays on otherwise after an enterprise network
        e = esp_wifi_set_config(WIFI_IF_STA, &c);
    }
    // first attempt of a streak only
    if (g_failures == 0) host_notify("#wifi connecting to '%s'%s", g_cur, s.user[0] ? " (enterprise)" : "");
    g_disc_reason = 0;
    g_sta_up = false;
    if (e == ESP_OK) {
        step = "connect";
        e = esp_wifi_connect();
    }
    if (e != ESP_OK) {
        char why[64];
        snprintf(why, sizeof(why), "%s: %s", step, esp_err_to_name(e));
        attempt_failed(why, 3000);
        return;
    }
    g_attempt_disc = g_disc_count;
    g_st = St::Connecting;
    g_attempt_start = now_ms();
}

// Scan not usable: connect directly (the driver searches all channels for that one SSID).
void connect_direct_next() {
    for (int tries = 0; tries < g_saved_n; tries++) {
        const int i = g_rr++ % g_saved_n;
        if (!g_saved[i].rejected) {
            connect_to(i);
            return;
        }
    }
}

int ap_clients() {
    wifi_sta_list_t l = {};
    return esp_wifi_ap_get_sta_list(&l) == ESP_OK ? l.num : 0;
}

uint32_t retry_delay() {
    const bool ap_in_use = ap_clients() > 0;   // scans make AP clients stall briefly
    if (g_failures < 5) return ap_in_use ? 15000 : 3000;
    return ap_in_use ? 60000 : 20000;
}

void start_scan(bool auto_connect) {
    if (g_st == St::Connecting) esp_wifi_disconnect();   // a scan is not possible while connecting
    if (g_st != St::Connected) g_st = St::Scanning;
    g_scan_auto = auto_connect;
    g_scan_done = false;
    wifi_scan_config_t sc = {};
    sc.show_hidden = false;
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    if (esp_wifi_scan_start(&sc, false) == ESP_OK) {
        g_scan_n = -1;
        g_scan_started = now_ms();
    } else {
        g_scan_n = -2;
        if (g_st == St::Scanning) g_st = St::Idle;
        if (auto_connect && g_st == St::Idle) connect_direct_next();
    }
}

void finish_scan() {
    uint16_t count = SCAN_RECORDS_MAX;
    wifi_ap_record_t* recs = static_cast<wifi_ap_record_t*>(calloc(count, sizeof(wifi_ap_record_t)));
    if (!recs || esp_wifi_scan_get_ap_records(&count, recs) != ESP_OK) count = 0;
    if (!recs) esp_wifi_clear_ap_list();   // frees the driver's list
    // keep only the strongest BSSID per SSID, sorted by RSSI
    int n = 0;
    for (int i = 0; i < count; i++) {
        const char* ssid = reinterpret_cast<const char*>(recs[i].ssid);
        if (ssid[0] == '\0') continue;
        int j = 0;
        while (j < n && strcmp(g_scan[j].ssid, ssid) != 0) j++;
        if (j < n && g_scan[j].rssi >= recs[i].rssi) continue;
        if (j == n) {
            if (n >= NET_MAX_SCAN) continue;
            n++;
        }
        snprintf(g_scan[j].ssid, sizeof(g_scan[j].ssid), "%s", ssid);
        g_scan[j].rssi = recs[i].rssi;
        g_scan[j].channel = recs[i].primary;
        g_scan[j].secured = recs[i].authmode != WIFI_AUTH_OPEN;
        g_scan[j].enterprise = recs[i].authmode == WIFI_AUTH_WPA2_ENTERPRISE ||
                               recs[i].authmode == WIFI_AUTH_WPA3_ENT_192;
        g_scan[j].saved = find_saved(g_scan[j].ssid) >= 0;
        memcpy(g_scan_bssid[j], recs[i].bssid, 6);
    }
    free(recs);
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
        if (idx >= 0 && !g_saved[idx].rejected) {
            g_reported_none = false;
            connect_to(idx, j);
            return;
        }
    }
    if (g_saved_n > 0 && !g_reported_none) {
        g_reported_none = true;
        host_notify("#wifi no saved network in range (%d networks visible), retrying", n);
    }
    if (g_scan_auto) g_next_try = now_ms() + retry_delay();
}

bool sta_ip(char* out, size_t cap) {
    esp_netif_ip_info_t ip;
    if (!g_sta_netif || esp_netif_get_ip_info(g_sta_netif, &ip) != ESP_OK) return false;
    snprintf(out, cap, IPSTR, IP2STR(&ip.ip));
    return true;
}

int sta_rssi() {
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

void start_mdns() {
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set("Franke CAN");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    mdns_service_add(nullptr, "_slcan", "_tcp", TCP_PORT, nullptr, 0);
}

}  // namespace

void net_init() {
    load_all();

    g_sta_netif = esp_netif_create_default_wifi_sta();
    g_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(g_sta_netif, MDNS_HOSTNAME);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   // credentials live in our own NVS namespace
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr);

    esp_wifi_set_mode(WIFI_MODE_APSTA);       // the access point is always on
    wifi_config_t ap = {};
    const size_t sl = strlen(AP_SSID) < sizeof(ap.ap.ssid) ? strlen(AP_SSID) : sizeof(ap.ap.ssid);
    memcpy(ap.ap.ssid, AP_SSID, sl);
    ap.ap.ssid_len = static_cast<uint8_t>(sl);
    const size_t pl = strlen(AP_PASSWORD) < sizeof(ap.ap.password) ? strlen(AP_PASSWORD) : sizeof(ap.ap.password) - 1;
    memcpy(ap.ap.password, AP_PASSWORD, pl);
    ap.ap.authmode = pl >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);            // lower latency for the live view

    start_mdns();
    net_tcp_init();

    // fastest start: the last used network directly, without scanning
    const int idx = find_saved(g_last);
    if (idx >= 0) connect_to(idx);
    else g_next_try = now_ms();               // scan right away
}

void net_tick() {
    const uint32_t now = now_ms();

    // replay ring lent to an enterprise login: take it back as soon as no login is running
    if (g_st != St::Connecting) host_replay_restore();

    // scan finished / stuck?
    if (g_scan_n == -1) {
        if (g_scan_done) {
            g_scan_done = false;
            finish_scan();
        } else if (now - g_scan_started > SCAN_TIMEOUT_MS) {
            esp_wifi_scan_stop();
            esp_wifi_clear_ap_list();
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
            if (!g_sta_up) {
                host_notify("#wifi connection to '%s' lost, reconnecting", g_cur);
                // fast path: retry the same network directly
                const int idx = find_saved(g_cur);
                g_st = St::Idle;
                if (idx >= 0) connect_to(idx);
            }
            break;
        case St::Connecting:
            if (g_sta_up) {
                g_st = St::Connected;
                g_failures = 0;
                if (strcmp(g_last, g_cur) != 0) {
                    snprintf(g_last, sizeof(g_last), "%s", g_cur);
                    save_last();
                }
                char ip[16] = "?";
                sta_ip(ip, sizeof(ip));
                host_notify("#wifi connected '%s' ip=%s rssi=%d -> http://%s.local", g_cur, ip, sta_rssi(),
                            MDNS_HOSTNAME);
            } else if (g_disc_count != g_attempt_disc) {
                // the driver gave up on this attempt (it does not retry by itself): fail right away and
                // scan at once - maybe another saved network is in range
                char why[96];
                const uint8_t r = g_disc_reason;
                snprintf(why, sizeof(why), "reason %u: %s", r, reason_name(r));
                attempt_failed(why, 0);
                // Enterprise login refused: do not retry by itself. University networks often lock the account
                // after a few failed logins (Active Directory) - that would also lock out the user's other devices.
                const int idx = find_saved(g_cur);
                if (idx >= 0 && g_saved[idx].user[0] && r == WIFI_REASON_802_1X_AUTH_FAILED) {
                    g_saved[idx].rejected = true;
                    host_notify("#wifi '%s': login rejected by the server - not retried automatically. Check "
                                "username/password (save the network again) or press Connect to retry", g_cur);
                }
            } else {
                const int idx = find_saved(g_cur);
                const uint32_t limit = (idx >= 0 && g_saved[idx].user[0]) ? CONNECT_TIMEOUT_EAP_MS : CONNECT_TIMEOUT_MS;
                if (static_cast<int32_t>(now_ms() - g_attempt_start) > static_cast<int32_t>(limit)) {
                    stop_sta_activity();   // waits for the disconnect event, so it can't end the next attempt
                    attempt_failed("timeout", 0);
                }
            }
            break;
        case St::Idle:
            if (g_saved_n > 0 && static_cast<int32_t>(now - g_next_try) >= 0 && g_scan_n != -1) start_scan(true);
            break;
        case St::Scanning:
            break;
    }
}

void net_status(char* out, size_t cap) {
    const char* st = g_st == St::Connected ? "connected" : g_st == St::Connecting ? "connecting"
                     : g_st == St::Scanning ? "scanning" : (g_saved_n ? "idle" : "unconfigured");
    int n = snprintf(out, cap, "wifi=%s", st);
    char ip[16];
    if (g_st == St::Connected && sta_ip(ip, sizeof(ip)) && n > 0 && static_cast<size_t>(n) < cap)
        n += snprintf(out + n, cap - n, " ip=%s rssi=%d", ip, sta_rssi());
    char ap_ip[16] = "-";
    esp_netif_ip_info_t ai;
    if (g_ap_netif && esp_netif_get_ip_info(g_ap_netif, &ai) == ESP_OK) snprintf(ap_ip, sizeof(ap_ip), IPSTR, IP2STR(&ai.ip));
    if (n > 0 && static_cast<size_t>(n) < cap)
        n += snprintf(out + n, cap - n, " ap_ip=%s ap_clients=%d saved=%d", ap_ip, ap_clients(), g_saved_n);
    if (n > 0 && static_cast<size_t>(n) < cap) net_tcp_status(out + n, cap - n);
}

bool net_wifi_add(const char* ssid, const char* pass, const char* user, char* err, size_t errcap) {
    const size_t sl = strlen(ssid), pl = pass ? strlen(pass) : 0, ul = user ? strlen(user) : 0;
    if (sl == 0 || sl > 32) {
        snprintf(err, errcap, "SSID must be 1..32 characters");
        return false;
    }
    if (ul > 0) {   // enterprise
        if (ul > 64 || pl == 0 || pl > 64) {
            snprintf(err, errcap, "enterprise: username and password must be 1..64 characters");
            return false;
        }
    } else if (pl > 64 || (pl > 0 && pl < 8)) {
        snprintf(err, errcap, "password must be empty (open network) or 8..64 characters");
        return false;
    }
    if (!add_saved(ssid, pass, user)) {
        snprintf(err, errcap, "max. %d saved networks, delete one first", NET_MAX_SAVED);
        return false;
    }
    save_all();
    g_failures = 0;
    int scan_idx = -1;
    for (int j = 0; j < (g_scan_n > 0 ? g_scan_n : 0); j++) {
        if (strcmp(g_scan[j].ssid, ssid) == 0) {
            scan_idx = j;
            g_scan[j].saved = true;
        }
    }
    connect_to(find_saved(ssid), scan_idx);
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
        esp_wifi_disconnect();
        g_st = St::Idle;
        g_next_try = now_ms();   // look for another saved network
    }
    return true;
}

bool net_wifi_connect(const char* ssid) {
    const int i = find_saved(ssid);
    if (i < 0) return false;
    g_saved[i].rejected = false;   // explicit retry
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
bool net_wifi_saved_enterprise(int i) { return i >= 0 && i < g_saved_n && g_saved[i].user[0]; }
const char* net_wifi_saved_user(int i) { return (i >= 0 && i < g_saved_n) ? g_saved[i].user : ""; }
const char* net_wifi_saved_pass(int i) { return (i >= 0 && i < g_saved_n) ? g_saved[i].pass : ""; }
bool net_wifi_saved_rejected(int i) { return i >= 0 && i < g_saved_n && g_saved[i].rejected; }
const char* net_wifi_current_ssid() { return g_st == St::Connected ? g_cur : ""; }

void net_wifi_state(char* out, size_t cap) {
    switch (g_st) {
        case St::Connected: {
            char ip[16] = "-";
            sta_ip(ip, sizeof(ip));
            snprintf(out, cap, "connected %d %s %s", sta_rssi(), ip, g_cur);
            break;
        }
        case St::Connecting: snprintf(out, cap, "connecting - - %s", g_cur); break;
        case St::Scanning: snprintf(out, cap, "scanning - - -"); break;
        default: snprintf(out, cap, "%s - - -", g_saved_n ? "idle" : "unconfigured"); break;
    }
}
