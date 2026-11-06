#include "commands.h"

#include <Arduino.h>
#include <esp_timer.h>

#include "can_bus.h"
#include "clock.h"
#include "config.h"
#include "host_io.h"
#include "net.h"
#include "rules.h"
#include "slcan_codec.h"

namespace {

void slcan_ok() { host_reply_raw("\r"); }
void slcan_err() { host_reply_raw("\a"); }

// ---- SLCAN (Lawicel) ---------------------------------------------------------------

void handle_slcan(char* line) {
    const char c = line[0];
    switch (c) {
        case 'O':  // open normal -> ACTIVE (refused while the STOP latch is set)
            (!can_stop_latched() && can_set_mode(CanMode::Active)) ? slcan_ok() : slcan_err();
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
        "#help #rule add <name> match=<id>[/<mask>]:<bytes|?\?> send=<id>:<bytes|{bN}>[@ms] [send=..] [dlc=n] [once]",
        "#help #rule del|enable|disable <name|all> | #rule list | #rule clear | #rule save | #rule load",
        "#help #wifi | #wifi scan | #wifi scanresult | #wifi add \"<ssid>\" \"<pass>\" | #wifi del|connect \"<ssid>\"",
        "#help #pins | #pins <tx> <rx> | #version | #reboot",
        "#help #mark [note] (marker to all clients/logs) | #time | #time set <epoch_ms> (if no NTP)",
        "#help #replay <since_us> (frames/notifications from the RAM buffer, for reconnecting clients)",
    };
    for (const char* l : lines) host_reply("%s", l);
    host_reply("#ok help");
}

void print_status() {
    char net[160];
    net_status(net, sizeof(net));
    const char* fmt = host_format() == OutFormat::Text ? "text" : "slcan";
    host_reply("#status mode=%s stop_latch=%d stops=%lu bitrate=%lu format=%s slcan_ts=%s echo=%s rules=%d "
               "enabled=%d limit=%u/s busoff=%s uptime_us=%llu clock=%s board=%s fw=%s %s",
               can_mode_name(can_mode()), can_stop_latched() ? 1 : 0, (unsigned long)can_stop_count(),
               (unsigned long)can_bitrate(), fmt, host_slcan_timestamps() ? "on" : "off",
               host_echo() ? "on" : "off", rules_count(), rules_enabled_count(), can_rate_limit(),
               can_bus_off() ? "yes" : "no", (unsigned long long)esp_timer_get_time(), clock_source_name(),
               CONFIG_IDF_TARGET, FW_VERSION, net);
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
    host_reply("#stats heap_free=%lu heap_min=%lu heap_max_block=%lu", (unsigned long)ESP.getFreeHeap(),
               (unsigned long)ESP.getMinFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
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
// #wifi add "<ssid>" "<pass>"    save + connect (alias: set)
// #wifi del "<ssid>" | #wifi connect "<ssid>"
// Reply lines (SSID always last, may contain spaces):
//   #wifistate <connected|connecting|scanning|idle|unconfigured> <rssi|-> <ip|-> <ssid|->
//   #wifisaved <1 = current|0> <ssid>
//   #wifinet <rssi> <channel> <open|secured> <1 = saved|0> <ssid>
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
            host_reply("#wifisaved %d %s", strcmp(s, cur) == 0 ? 1 : 0, s);
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
            host_reply("#wifinet %d %u %s %d %s", e->rssi, e->channel, e->secured ? "secured" : "open",
                       e->saved ? 1 : 0, e->ssid);
        }
        host_reply("#ok wifi scan %d", n < 0 ? 0 : n);
    } else if (strcmp(sub, "add") == 0 || strcmp(sub, "set") == 0) {
        if (argc < 2) {
            host_reply("#err wifi add \"<ssid>\" \"<password>\"");
            return;
        }
        char err[96];
        if (net_wifi_add(argv[1], argc > 2 ? argv[2] : "", err, sizeof(err)))
            host_reply("#ok wifi add %s (connecting)", argv[1]);
        else
            host_reply("#err wifi add: %s", err);
    } else if (strcmp(sub, "del") == 0 || strcmp(sub, "delete") == 0) {
        if (argc >= 2 && net_wifi_delete(argv[1])) host_reply("#ok wifi del %s", argv[1]);
        else host_reply("#err wifi del: no saved network '%s'", argc >= 2 ? argv[1] : "");
    } else if (strcmp(sub, "connect") == 0) {
        if (argc >= 2 && net_wifi_connect(argv[1])) host_reply("#ok wifi connect %s", argv[1]);
        else host_reply("#err wifi connect: no saved network '%s'", argc >= 2 ? argv[1] : "");
    } else {
        host_reply("#err wifi: use #wifi | scan | scanresult | add | del | connect");
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
            host_reply("#ok mode %s", can_mode_name(can_mode()));
        } else {
            host_reply("#err mode %s failed (now %s)", args, can_mode_name(can_mode()));
        }
    } else if (strcmp(cmd, "stop") == 0) {
        // "#stop" = emergency stop + latch; "#stop nolatch" = same without latch (a script's own cleanup)
        const bool latch = strcmp(args, "nolatch") != 0;
        can_emergency_stop(latch ? "command" : "cleanup", latch);
        uint32_t t0 = millis();
        while (can_mode() != CanMode::Listen && millis() - t0 < 500) delay(1);
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
        if (strlen(note) > 140) note[140] = '\0';
        const uint64_t e = clock_epoch_of(boot);
        host_notify("#mark %llu %llu %s", (unsigned long long)boot, (unsigned long long)e, note);
        host_reply("#ok mark");
    } else if (strcmp(cmd, "version") == 0) {
        host_reply("#version franke-can fw=%s board=%s", FW_VERSION, CONFIG_IDF_TARGET);
        host_reply("#ok version");
    } else if (strcmp(cmd, "reboot") == 0) {
        host_reply("#ok reboot");
        delay(50);
        ESP.restart();
    } else {
        host_reply("#err unknown command '#%s' (try #help)", cmd);
    }
}

}  // namespace

void cmd_handle_line(char* line, uint8_t source) {
    while (*line == ' ' || *line == '\t') line++;
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = '\0';
    if (n == 0) return;
    host_set_reply_sink(source);
    if (line[0] == '#') handle_ext(line + 1);
    else handle_slcan(line);
    host_set_reply_sink(SINK_SERIAL);
}
