#include "commands.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "can_bus.h"
#include "clock.h"
#include "config.h"
#include "health.h"
#include "host_io.h"
#include "net.h"
#include "rules.h"
#include "slcan_codec.h"
#include "sys.h"
#include "web.h"

namespace {

struct CmdMsg {
    uint8_t source;    // SINK_*
    uint32_t client;   // browser id for SINK_WEB
    char text[CMD_LINE_MAX];
};
QueueHandle_t g_cmd_q = nullptr;

void slcan_ok() { host_reply_raw("\r"); }
void slcan_err() { host_reply_raw("\a"); }

// remember which link switched the bus to ACTIVE, so the watchdog can revert if it disappears
void note_active_owner() { can_active_set_owner(host_reply_sink(), host_reply_client()); }

// ---- SLCAN (Lawicel) ---------------------------------------------------------------

void handle_slcan(char* line) {
    const char c = line[0];
    switch (c) {
        case 'O':  // open normal -> ACTIVE (refused while the STOP latch is set)
            if (!can_stop_latched() && can_set_mode(CanMode::Active)) { note_active_owner(); slcan_ok(); }
            else slcan_err();
            return;
        case 'L':  // open listen-only -> LISTEN
            can_set_mode(CanMode::Listen) ? slcan_ok() : slcan_err();
            return;
        case 'C':
            can_set_mode(CanMode::Closed) ? slcan_ok() : slcan_err();
            return;
        case 'S': {
            const uint32_t br = line[2] == '\0' ? slcan_bitrate_from_code(line[1]) : 0;
            (br && can_set_bitrate(br)) ? slcan_ok() : slcan_err();
            return;
        }
        case 't':
        case 'T':
        case 'r':
        case 'R': {
            CanFrame f;
            if (!slcan_parse_frame(line, f) || can_mode() != CanMode::Active) {
                slcan_err();
                return;
            }
            if (can_transmit(f, "slcan") == TxResult::Ok)
                host_reply_raw(f.ext ? "Z\r" : "z\r");
            else
                slcan_err();
            return;
        }
        case 'Z':
            if (line[1] == '0' || line[1] == '1') {
                host_set_slcan_timestamps(line[1] == '1');
                slcan_ok();
            } else {
                slcan_err();
            }
            return;
        case 'V':
            host_reply_raw("V1013\r");
            return;
        case 'v':
            host_reply_raw("v0100\r");
            return;
        case 'N':
            host_reply_raw("NFC01\r");
            return;
        case 'F': {
            char buf[8];
            snprintf(buf, sizeof(buf), "F%02X\r", can_take_slcan_flags());
            host_reply_raw(buf);
            return;
        }
        case 'M':   // acceptance code/mask: accepted, but everything is received anyway
        case 'm':
        case 'X':   // auto poll
        case 'W':   // filter mode
        case 'Q':   // auto startup
            slcan_ok();
            return;
        default:
            slcan_err();
            return;
    }
}

// ---- "#" extension commands ----------------------------------------------------

void print_help() {
    static const char* const lines[] = {
        "#help SLCAN: O(open active) L(open listen) C(close) S0-S8(bitrate, S4=125k) tIIILDD.. Z0/Z1 V N F",
        "#help #status | #stats [reset] | #mode listen|active|closed | #stop (emergency stop + latch) | #resume",
        "#help #send <id> <bytes>   e.g. #send 481 11 00 27  or  #send 481#110027   (ACTIVE only)",
        "#help #format slcan|text | #ts us|ms|off | #echo on|off | #bitrate <bps> | #limit <frames/s>",
        "#help #watchdog [secs] (ACTIVE -> LISTEN if the controlling TCP/web client drops; 0 = off)",
        "#help #health (heap, rx rate, drops, task stacks, reset reason) | #selftest flood <fps> <secs> | #selftest stop",
        "#help #rule add <name> match=<id>[/<mask>]:<bytes|?\?> send=<id>:<bytes|{bN}>[@ms] [send=..] [dlc=n] [once]",
        "#help #rule del|enable|disable <name|all> | #rule list | #rule clear | #rule save | #rule load",
        "#help #wifi | #wifi scan | #wifi scanresult | #wifi add \"<ssid>\" \"<pass>\" [\"<enterprise user>\"] | #wifi del|connect \"<ssid>\"",
        "#help #pins | #pins <tx> <rx> | #version | #reboot | #wifi show \"<ssid>\" (USB only)",
        "#help #log <tag|*> none|error|warn|info|debug|verbose (ESP-IDF log on the console UART, max. info, e.g. #log esp_netif info)",
        "#help #mark [note] (marker to all clients/logs) | #time | #time set <epoch_ms> (if no NTP)",
        "#help #replay <since_us> (frames/notifications from the RAM buffer, for reconnecting clients) | #ping",
    };
    for (const char* l : lines) host_reply("%s", l);
    host_reply("#ok help");
}

void print_status() {
    char net[160];
    net_status(net, sizeof(net));
    const char* fmt = host_format() == OutFormat::Text ? "text" : "slcan";
    host_reply("#status mode=%s stop_latch=%d stops=%lu watchdog=%lu bitrate=%lu format=%s slcan_ts=%s "
               "echo=%s rules=%d enabled=%d limit=%u/s busoff=%s uptime_us=%llu clock=%s board=%s fw=%s "
               "web_auth=%s ap_auth=%s %s",
               can_mode_name(can_mode()), can_stop_latched() ? 1 : 0, (unsigned long)can_stop_count(),
               (unsigned long)can_watchdog_secs(),
               (unsigned long)can_bitrate(), fmt, host_slcan_timestamps() ? "on" : "off",
               host_echo() ? "on" : "off", rules_count(), rules_enabled_count(), can_rate_limit(),
               can_bus_off() ? "yes" : "no", (unsigned long long)esp_timer_get_time(), clock_source_name(),
               CONFIG_IDF_TARGET, FW_VERSION, sys_web_login_is_default() ? "default" : "custom",
               sys_ap_password_is_default() ? "default" : "custom", net);
    host_reply("#ok status");
}

void print_stats() {
    CanStats s;
    HostStats h;
    can_get_stats(s);
    host_get_stats(h);
    host_reply("#stats rx=%lu tx=%lu tx_err=%lu lost_host=%lu lost_rxqueue=%lu lost_fifo=%lu msgs_lost=%lu "
               "rule_overflow=%lu tec=%lu rec=%lu bus_errors=%lu arb_lost=%lu tx_failed=%lu bus_off=%lu state=%s",
               (unsigned long)s.rx, (unsigned long)s.tx, (unsigned long)s.tx_errors,
               (unsigned long)h.frames_dropped, (unsigned long)s.rx_missed, (unsigned long)s.rx_overrun,
               (unsigned long)h.msgs_dropped, (unsigned long)rules_pending_overflows(), (unsigned long)s.tec,
               (unsigned long)s.rec, (unsigned long)s.bus_errors, (unsigned long)s.arb_lost,
               (unsigned long)s.tx_failed, (unsigned long)s.bus_off_count, s.state);
    // memory: free heap now, lowest free heap since boot, largest allocatable block
    host_reply("#stats heap_free=%lu heap_min=%lu heap_max_block=%lu", (unsigned long)heap_free(),
               (unsigned long)heap_min(), (unsigned long)heap_largest());
    host_reply("#ok stats");
}

bool parse_mode(const char* s, CanMode& m) {
    if (strcmp(s, "listen") == 0) m = CanMode::Listen;
    else if (strcmp(s, "active") == 0) m = CanMode::Active;
    else if (strcmp(s, "closed") == 0) m = CanMode::Closed;
    else return false;
    return true;
}

bool parse_on_off(const char* s, bool& v) {
    if (strcmp(s, "on") == 0 || strcmp(s, "1") == 0) v = true;
    else if (strcmp(s, "off") == 0 || strcmp(s, "0") == 0) v = false;
    else return false;
    return true;
}

// Splits args into tokens; "double quoted" tokens may contain spaces, \" and \\ are escapes.
// Works in place. Returns the number of tokens.
int split_args(char* s, char** argv, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ') s++;
        if (!*s) break;
        char* out = s;
        argv[n++] = out;
        if (*s == '"') {
            s++;
            while (*s && *s != '"') {
                if (*s == '\\' && s[1]) s++;
                *out++ = *s++;
            }
            if (*s == '"') s++;
        } else {
            while (*s && *s != ' ') *out++ = *s++;
        }
        const bool more = *s != '\0';
        if (more) s++;
        *out = '\0';
        if (!more) break;
    }
    return n;
}

// #wifi                          state + saved networks
// #wifi scan                     start a scan;  #wifi scanresult  -> results (or "running")
// #wifi add "<ssid>" "<pass>" ["<username>"]   save + connect (alias: set); with a username the
//                                network is WPA2-Enterprise (PEAP/TTLS, e.g. eduroam)
// #wifi del "<ssid>" | #wifi connect "<ssid>"
// Reply lines (SSID always last, may contain spaces):
//   #wifistate <connected|connecting|scanning|idle|unconfigured> <rssi|-> <ip|-> <ssid|->
//   #wifisaved <1 = current|0> <psk|enterprise|rejected> <enterprise username|-> <ssid>
//              (rejected = enterprise login refused by the server, not retried automatically)
//   #wifinet <rssi> <channel> <open|secured|enterprise> <1 = saved|0> <ssid>
void handle_wifi(char* args) {
    char* argv[4];
    const int argc = split_args(args, argv, 4);
    const char* sub = argc ? argv[0] : "";
    char buf[160];

    if (argc == 0) {
        net_wifi_state(buf, sizeof(buf));
        host_reply("#wifistate %s", buf);
        const char* cur = net_wifi_current_ssid();
        for (int i = 0; i < net_wifi_saved_count(); i++) {
            const char* s = net_wifi_saved_ssid(i);
            // enterprise: the stored login (never the password), so a wrong/autofilled username is visible
            char user[65];
            snprintf(user, sizeof(user), "%s", net_wifi_saved_user(i)[0] ? net_wifi_saved_user(i) : "-");
            for (char* p = user; *p; p++)
                if (*p == ' ') *p = '_';   // one token
            host_reply("#wifisaved %d %s %s %s", strcmp(s, cur) == 0 ? 1 : 0,
                       net_wifi_saved_rejected(i) ? "rejected" : net_wifi_saved_enterprise(i) ? "enterprise" : "psk",
                       user, s);
        }
        net_status(buf, sizeof(buf));
        host_reply("#ok wifi %s", buf);
    } else if (strcmp(sub, "scan") == 0) {
        if (net_wifi_scan_start()) host_reply("#ok wifi scan started");
        else host_reply("#err wifi scan could not start");
    } else if (strcmp(sub, "scanresult") == 0) {
        const int n = net_wifi_scan_result_count();
        if (n == -1) {
            host_reply("#ok wifi scan running");
            return;
        }
        for (int i = 0; i < n; i++) {
            const NetScanEntry* e = net_wifi_scan_entry(i);
            host_reply("#wifinet %d %u %s %d %s", e->rssi, e->channel,
                       e->enterprise ? "enterprise" : (e->secured ? "secured" : "open"), e->saved ? 1 : 0, e->ssid);
        }
        host_reply("#ok wifi scan %d", n < 0 ? 0 : n);
    } else if (strcmp(sub, "add") == 0 || strcmp(sub, "set") == 0) {
        if (argc < 2) {
            host_reply("#err wifi add \"<ssid>\" \"<password>\" [\"<enterprise username>\"]");
            return;
        }
        char err[96];
        const char* user = argc > 3 ? argv[3] : "";
        if (net_wifi_add(argv[1], argc > 2 ? argv[2] : "", user, err, sizeof(err)))
            host_reply("#ok wifi add %s%s (connecting)", argv[1], user[0] ? " (enterprise)" : "");
        else
            host_reply("#err wifi add: %s", err);
    } else if (strcmp(sub, "del") == 0 || strcmp(sub, "delete") == 0) {
        if (argc >= 2 && net_wifi_delete(argv[1])) host_reply("#ok wifi del %s", argv[1]);
        else host_reply("#err wifi del: no saved network '%s'", argc >= 2 ? argv[1] : "");
    } else if (strcmp(sub, "show") == 0) {
        // stored credentials of one network, to check them byte by byte. USB only: the password must
        // never travel over the network (TCP/web) or end up in the replay buffer.
        if (host_reply_sink() != SINK_SERIAL) {
            host_reply("#err wifi show: only over USB (the password is never sent over the network)");
            return;
        }
        int idx = -1;
        for (int i = 0; i < net_wifi_saved_count(); i++)
            if (argc >= 2 && strcmp(net_wifi_saved_ssid(i), argv[1]) == 0) idx = i;
        if (idx < 0) {
            host_reply("#err wifi show: no saved network '%s'", argc >= 2 ? argv[1] : "");
            return;
        }
        auto dump = [](const char* name, const char* v) {
            char hex[3 * 65 + 1];
            size_t n = 0;
            for (const unsigned char* p = reinterpret_cast<const unsigned char*>(v); *p && n + 4 < sizeof(hex); p++)
                n += snprintf(hex + n, sizeof(hex) - n, "%02X ", *p);
            hex[n] = '\0';
            host_reply("#wifishow %s=[%s] len=%u hex=%s", name, v, static_cast<unsigned>(strlen(v)), hex);
        };
        dump("user", net_wifi_saved_user(idx));
        dump("pass", net_wifi_saved_pass(idx));
        host_reply("#ok wifi show %s (%s)", argv[1], net_wifi_saved_enterprise(idx) ? "enterprise" : "psk");
    } else if (strcmp(sub, "connect") == 0) {
        if (argc >= 2 && net_wifi_connect(argv[1])) host_reply("#ok wifi connect %s", argv[1]);
        else host_reply("#err wifi connect: no saved network '%s'", argc >= 2 ? argv[1] : "");
    } else {
        host_reply("#err wifi: use #wifi | scan | scanresult | add | del | connect | show");
    }
}

// #pins           -> "#pins tx=13 rx=27 tx_allowed=.. rx_allowed=.."
// #pins <tx> <rx> -> save, re-open the bus on the new pins (LISTEN)
void handle_pins(char* args) {
    char* argv[3];
    const int argc = split_args(args, argv, 3);
    if (argc >= 2) {
        char* e1;
        char* e2;
        const long tx = strtol(argv[0], &e1, 10), rx = strtol(argv[1], &e2, 10);
        char err[160];
        if (*e1 || *e2) {
            host_reply("#err pins: #pins <tx> <rx>");
        } else if (can_set_pins(static_cast<int>(tx), static_cast<int>(rx), err, sizeof(err))) {
            host_reply("#ok pins tx=%ld rx=%ld (bus re-opened, mode=%s)", tx, rx, can_mode_name(can_mode()));
        } else {
            host_reply("#err pins: %s", err);
        }
        return;
    }
    int tx, rx;
    can_get_pins(tx, rx);
    char ta[100], ra[100];
    can_pins_allowed(ta, sizeof(ta), ra, sizeof(ra));
    host_reply("#pins tx=%d rx=%d tx_allowed=%s rx_allowed=%s", tx, rx, ta, ra);
    host_reply("#ok pins");
}

void handle_ext(char* line) {
    // split "cmd args..."
    char* cmd = line;
    char* args = line;
    while (*args && *args != ' ') args++;
    if (*args) *args++ = '\0';
    while (*args == ' ') args++;

    if (strcmp(cmd, "help") == 0) {
        print_help();
    } else if (strcmp(cmd, "status") == 0) {
        print_status();
    } else if (strcmp(cmd, "stats") == 0) {
        if (strcmp(args, "reset") == 0) {
            can_reset_stats();
            host_reset_stats();
            host_reply("#ok stats reset");
        } else {
            print_stats();
        }
    } else if (strcmp(cmd, "mode") == 0) {
        CanMode m;
        if (!parse_mode(args, m)) {
            host_reply("#err mode: use listen|active|closed");
        } else if (m == CanMode::Active && can_stop_latched()) {
            host_reply("#err mode active refused: STOP latched (stops=%lu) - send #resume first",
                       (unsigned long)can_stop_count());
        } else if (can_set_mode(m)) {
            if (m == CanMode::Active) note_active_owner();
            host_reply("#ok mode %s", can_mode_name(can_mode()));
        } else {
            host_reply("#err mode %s failed (now %s)", args, can_mode_name(can_mode()));
        }
    } else if (strcmp(cmd, "stop") == 0) {
        // "#stop" = emergency stop + latch; "#stop nolatch" = same without latch (a script's own cleanup)
        const bool latch = strcmp(args, "nolatch") != 0;
        can_emergency_stop(latch ? "command" : "cleanup", latch);
        const uint32_t t0 = now_ms();
        while (can_mode() != CanMode::Listen && now_ms() - t0 < 500) vTaskDelay(1);
        host_reply("#ok stop mode=%s latched=%d", can_mode_name(can_mode()), can_stop_latched() ? 1 : 0);
    } else if (strcmp(cmd, "resume") == 0) {
        can_resume();
        host_reply("#ok resume (mode=%s; switch to active explicitly if needed)", can_mode_name(can_mode()));
    } else if (strcmp(cmd, "send") == 0) {
        CanFrame f;
        if (!parse_frame_text(args, f)) {
            host_reply("#err send: bad frame '%s'", args);
        } else if (can_mode() != CanMode::Active) {
            host_reply("#err send: not ACTIVE (mode=%s)", can_mode_name(can_mode()));
        } else {
            TxResult r = can_transmit(f, "manual");
            if (r == TxResult::Ok) host_reply("#ok send");
            else host_reply("#err send: %s", tx_result_name(r));
        }
    } else if (strcmp(cmd, "rule") == 0) {
        rules_command(args);
    } else if (strcmp(cmd, "format") == 0) {
        if (strcmp(args, "slcan") == 0) host_set_format(OutFormat::Slcan);
        else if (strcmp(args, "text") == 0) host_set_format(OutFormat::Text);
        else {
            host_reply("#err format: use slcan|text");
            return;
        }
        host_reply("#ok format %s", args);
    } else if (strcmp(cmd, "ts") == 0) {
        if (strcmp(args, "us") == 0) {
            host_set_format(OutFormat::Text);
        } else if (strcmp(args, "ms") == 0) {
            host_set_format(OutFormat::Slcan);
            host_set_slcan_timestamps(true);
        } else if (strcmp(args, "off") == 0) {
            host_set_format(OutFormat::Slcan);
            host_set_slcan_timestamps(false);
        } else {
            host_reply("#err ts: use us|ms|off");
            return;
        }
        host_reply("#ok ts %s", args);
    } else if (strcmp(cmd, "echo") == 0) {
        bool v;
        if (!parse_on_off(args, v)) {
            host_reply("#err echo: use on|off");
            return;
        }
        host_set_echo(v);
        host_reply("#ok echo %s", v ? "on" : "off");
    } else if (strcmp(cmd, "bitrate") == 0) {
        uint32_t br = strtoul(args, nullptr, 10);
        if (can_set_bitrate(br)) host_reply("#ok bitrate %lu", (unsigned long)can_bitrate());
        else host_reply("#err bitrate: unsupported '%s' (10000..1000000, see S0-S8)", args);
    } else if (strcmp(cmd, "limit") == 0) {
        long n = strtol(args, nullptr, 10);
        if (n < 1 || n > 500) {
            host_reply("#err limit: 1..500 frames/s");
            return;
        }
        can_set_rate_limit(static_cast<uint16_t>(n));
        host_reply("#ok limit %u", can_rate_limit());
    } else if (strcmp(cmd, "watchdog") == 0) {
        if (args[0] != '\0') {
            long s = strtol(args, nullptr, 10);
            if (s < 0 || s > 3600) {
                host_reply("#err watchdog: 0..3600 s (0 = off)");
                return;
            }
            can_set_watchdog(static_cast<uint32_t>(s));
        }
        uint32_t w = can_watchdog_secs();
        if (w == 0) host_reply("#ok watchdog off");
        else host_reply("#ok watchdog %lus (ACTIVE reverts to LISTEN if the controlling TCP/web client "
                        "disconnects that long)", (unsigned long)w);
    } else if (strcmp(cmd, "health") == 0) {
        health_print();
    } else if (strcmp(cmd, "selftest") == 0) {
        // "#selftest flood <fps> <secs>" | "#selftest stop"
        char* sub = args;
        char* rest = sub;
        while (*rest && *rest != ' ') rest++;
        if (*rest) *rest++ = '\0';
        if (strcmp(sub, "stop") == 0) {
            selftest_stop();
            host_reply("#ok selftest stop");
        } else if (strcmp(sub, "flood") == 0) {
            unsigned fps = 0, secs = 0;
            sscanf(rest, "%u %u", &fps, &secs);
            char err[48];
            if (selftest_flood_start(fps, secs, err, sizeof(err)))
                host_reply("#ok selftest flood %u/s for %us (synthetic RX id=7FF, not on the bus)", fps, secs);
            else
                host_reply("#err selftest: %s", err);
        } else {
            host_reply("#err selftest: use 'flood <fps> <secs>' or 'stop'");
        }
    } else if (strcmp(cmd, "wifi") == 0) {
        handle_wifi(args);
    } else if (strcmp(cmd, "pins") == 0) {
        handle_pins(args);
    } else if (strcmp(cmd, "time") == 0) {
        // "#time" -> "#time <boot_us> <epoch_us|0> <ntp|host|none>"; "#time set <epoch_ms>" (ignored if NTP)
        if (strncmp(args, "set ", 4) == 0) {
            const uint64_t ms = strtoull(args + 4, nullptr, 10);
            if (!clock_set_from_host(ms)) {
                host_reply(clock_source() == ClockSource::Ntp ? "#ok time set ignored (NTP active)"
                                                               : "#err time set: invalid epoch ms");
                return;
            }
        }
        uint64_t boot, epoch;
        clock_now(boot, epoch);
        host_reply("#time %llu %llu %s", (unsigned long long)boot, (unsigned long long)epoch, clock_source_name());
        host_reply("#ok time");
    } else if (strcmp(cmd, "replay") == 0) {
        // "#replay <since_us>": frames/notifications from the RAM buffer after that device time
        host_replay(strtoull(args, nullptr, 10));
    } else if (strcmp(cmd, "mark") == 0) {
        // "#mark [@<boot_us>] [note]" -> broadcast "#mark <boot_us> <epoch_us|0> <note>" to all links
        uint64_t boot, epoch;
        clock_now(boot, epoch);
        char* note = args;
        if (*note == '@') {
            char* end;
            const uint64_t t = strtoull(note + 1, &end, 10);
            if (t > 0 && t <= boot) boot = t;   // time of the button press (client side)
            note = end;
            while (*note == ' ') note++;
        }
        for (char* p = note; *p; p++)
            if (static_cast<unsigned char>(*p) < 0x20) *p = ' ';   // keep it one line
        if (strlen(note) > 140) {   // cut at a UTF-8 character boundary (not inside "ü")
            size_t cut = 140;
            while (cut > 0 && (static_cast<unsigned char>(note[cut]) & 0xC0) == 0x80) cut--;
            note[cut] = '\0';
        }
        const uint64_t e = clock_epoch_of(boot);
        host_notify("#mark %llu %llu %s", (unsigned long long)boot, (unsigned long long)e, note);
        host_reply("#ok mark");
    } else if (strcmp(cmd, "version") == 0) {
        host_reply("#version franke-can fw=%s board=%s", FW_VERSION, CONFIG_IDF_TARGET);
        host_reply("#ok version");
    } else if (strcmp(cmd, "reboot") == 0) {
        sys_app_confirm();   // a deliberate reboot must not roll back a freshly installed firmware
        host_reply("#ok reboot");
        sys_request_reboot(300);   // from the main task, after the reply is out
    } else if (strcmp(cmd, "log") == 0) {
        // "#log <tag|*> <none|error|warn|info|debug|verbose>": ESP-IDF log output on the console UART
        // (classic ESP32: the USB port, mixed into the SLCAN stream - for debugging only). Off after a reboot.
        char* argv2[2];
        const int n = split_args(args, argv2, 2);
        static const char* const names[] = {"none", "error", "warn", "info", "debug", "verbose"};
        int lvl = -1;
        for (int i = 0; n == 2 && i < 6; i++)
            if (strcmp(argv2[1], names[i]) == 0) lvl = i;
        if (lvl < 0) {
            host_reply("#err log: #log <tag|*> none|error|warn|info|debug|verbose  (e.g. #log wpa debug)");
            return;
        }
        esp_log_level_set(argv2[0], static_cast<esp_log_level_t>(lvl));
        // the WPA supplicant prints keys and WLAN passwords (hex) at debug level - never go above info
        const bool wpa = strcmp(argv2[0], "wpa") == 0 || strcmp(argv2[0], "*") == 0;
        if (wpa && lvl > ESP_LOG_INFO) esp_log_level_set("wpa", ESP_LOG_INFO);
        host_reply("#ok log %s %s%s", argv2[0], names[lvl],
                   wpa && lvl > ESP_LOG_INFO ? " (wpa limited to info: debug prints keys and passwords)" : "");
    } else if (strcmp(cmd, "ping") == 0) {
        host_reply("#ok ping");    // keepalive / round-trip check (the web UI sends it every 20 s)
    } else {
        host_reply("#err unknown command '#%s' (try #help)", cmd);
    }
}

}  // namespace

void cmd_init() { g_cmd_q = xQueueCreate(CMD_QUEUE_LEN, sizeof(CmdMsg)); }

bool cmd_post(uint8_t source, uint32_t client, const char* line, TickType_t wait) {
    if (!g_cmd_q) return false;
    CmdMsg m;
    m.source = source;
    m.client = client;
    snprintf(m.text, sizeof(m.text), "%s", line);
    return xQueueSend(g_cmd_q, &m, wait) == pdTRUE;
}

bool cmd_process(TickType_t wait) {
    static CmdMsg m;   // main task only; keeps 330 bytes off its stack
    if (!g_cmd_q || xQueueReceive(g_cmd_q, &m, wait) != pdTRUE) return false;
    char* line = m.text;
    while (*line == ' ' || *line == '\t') line++;
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = '\0';
    if (n == 0) return true;
    host_set_reply_target(m.source, m.source == SINK_WEB ? m.client : 0);
    if (line[0] == '#') handle_ext(line + 1);
    else handle_slcan(line);
    host_set_reply_target(SINK_SERIAL, 0);
    return true;
}
