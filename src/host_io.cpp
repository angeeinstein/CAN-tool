#include "host_io.h"

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "host_serial.h"
#include "net.h"
#include "slcan_codec.h"
#include "sys.h"
#include "web.h"

namespace {

constexpr size_t REASON_LEN = 16;
constexpr size_t MSG_LEN = 200;   // fits "#mark <boot_us> <epoch_us> <note up to 140 chars>"

struct FrameEvent {
    uint64_t ts_us;
    CanFrame frame;
    bool is_tx;
    char reason[REASON_LEN];
};

struct MsgEvent {
    char text[MSG_LEN];
};

QueueHandle_t g_frame_q = nullptr;
QueueHandle_t g_msg_q = nullptr;
SemaphoreHandle_t g_out_mtx = nullptr;

volatile OutFormat g_format = OutFormat::Slcan;
volatile bool g_slcan_ts = false;
volatile bool g_echo = false;
std::atomic<uint32_t> g_frames_dropped{0};
std::atomic<uint32_t> g_msgs_dropped{0};
uint8_t g_reply_sinks = SINK_SERIAL;
uint32_t g_reply_client = 0;   // browser of the command being executed

// ---- replay buffer: recent frames + notifications in RAM, so a client can fetch what it missed ----
struct ReplayFrame {          // 32 bytes
    uint64_t ts_us;
    uint32_t id;
    uint8_t dlc;
    uint8_t flags;            // bit0 tx, bit1 ext, bit2 rtr
    uint8_t data[8];
    char reason[10];          // tx reason (rule name), truncated
};
struct ReplayMsg {
    uint64_t ts_us;
    char text[MSG_LEN];
};
ReplayFrame* g_rf = nullptr;  // ring of REPLAY_FRAMES (nullptr while released, see host_replay_release)
uint64_t g_rf_lost_until = 0; // frames before this time are gone (ring was released) -> reported as gap
ReplayMsg* g_rm = nullptr;    // ring of REPLAY_MSGS
uint32_t g_rf_head = 0, g_rf_count = 0;   // head = next write index
uint32_t g_rm_head = 0, g_rm_count = 0;
portMUX_TYPE g_replay_mux = portMUX_INITIALIZER_UNLOCKED;

void replay_push_frame(const CanFrame& f, uint64_t ts, bool tx, const char* reason) {
    ReplayFrame r;
    r.ts_us = ts;
    r.id = f.id;
    r.dlc = f.dlc;
    r.flags = (tx ? 1 : 0) | (f.ext ? 2 : 0) | (f.rtr ? 4 : 0);
    memcpy(r.data, f.data, 8);
    snprintf(r.reason, sizeof(r.reason), "%s", reason ? reason : "");
    portENTER_CRITICAL(&g_replay_mux);
    if (g_rf) {   // checked under the lock: the ring may be released by another task
        g_rf[g_rf_head] = r;
        g_rf_head = (g_rf_head + 1) % REPLAY_FRAMES;
        if (g_rf_count < REPLAY_FRAMES) g_rf_count++;
    }
    portEXIT_CRITICAL(&g_replay_mux);
}

void replay_push_msg(const char* text, uint64_t ts) {
    if (!g_rm) return;
    portENTER_CRITICAL(&g_replay_mux);
    ReplayMsg& m = g_rm[g_rm_head];
    m.ts_us = ts;
    strncpy(m.text, text, MSG_LEN - 1);
    m.text[MSG_LEN - 1] = '\0';
    g_rm_head = (g_rm_head + 1) % REPLAY_MSGS;
    if (g_rm_count < REPLAY_MSGS) g_rm_count++;
    portEXIT_CRITICAL(&g_replay_mux);
}

// Writes to the selected links. Caller must hold g_out_mtx.
// reply = command reply: may wait for the link instead of dropping, and for the web it goes only to
// the browser that sent the command.
void write_sinks(uint8_t sinks, const char* buf, size_t len, bool reply = false) {
    if (len == 0) return;
    if (sinks & SINK_SERIAL) host_serial_write(buf, len);
    if (sinks & SINK_TCP) net_tcp_write(buf, len, reply);
    if (sinks & SINK_WEB) {
        if (reply) web_reply(g_reply_client, buf, len);
        else web_write(buf, len);
    }
}

// Command reply to the requesting link. A browser reply goes through the web module's own per-browser
// buffer and may wait for that browser - without holding g_out_mtx, so USB/TCP output keeps running.
void write_reply(const char* buf, size_t len) {
    if (g_reply_sinks == SINK_WEB) {
        web_reply(g_reply_client, buf, len);
        return;
    }
    xSemaphoreTake(g_out_mtx, portMAX_DELAY);
    write_sinks(g_reply_sinks, buf, len, true);
    xSemaphoreGive(g_out_mtx);
}

// "#rx/#tx ..." text line incl. terminator
size_t format_text(const FrameEvent& e, char* out, size_t cap) {
    size_t n = text_format_frame(e.is_tx ? "tx" : "rx", e.ts_us, e.frame, e.is_tx ? e.reason : nullptr, out,
                                 cap - 2);
    if (n == 0) return 0;
    out[n++] = '\n';
    out[n++] = '\r';
    return n;
}

// Line for USB/TCP according to the format setting
size_t format_main(const FrameEvent& e, char* out, size_t cap) {
    if (e.is_tx || g_format == OutFormat::Text) return format_text(e, out, cap);
    const uint16_t ts_ms = static_cast<uint16_t>((e.ts_us / 1000ULL) % 60000ULL);
    size_t n = slcan_format_frame(e.frame, g_slcan_ts, ts_ms, out, cap - 1);
    if (n == 0) return 0;
    out[n++] = '\r';
    return n;
}

struct Batch {
    char buf[2048];
    size_t used = 0;
    uint8_t sinks;
    explicit Batch(uint8_t s) : sinks(s) {}
    void flush() {
        if (used == 0) return;
        xSemaphoreTake(g_out_mtx, portMAX_DELAY);
        write_sinks(sinks, buf, used);
        xSemaphoreGive(g_out_mtx);
        used = 0;
    }
    void append(const char* s, size_t n) {
        if (used + n > sizeof(buf)) flush();
        memcpy(buf + used, s, n);
        used += n;
    }
};

void output_task(void*) {
    static Batch main_out(SINK_SERIAL | SINK_TCP);
    static Batch web_out(SINK_WEB);
    FrameEvent fe;
    MsgEvent me;

    for (;;) {
        const bool web = web_has_clients();
        // notifications first (rare, but important: bus-off, emergency stop ...) -> all links
        while (xQueueReceive(g_msg_q, &me, 0) == pdTRUE) {
            size_t n = strnlen(me.text, MSG_LEN);
            main_out.append(me.text, n);
            main_out.append("\n\r", 2);
            if (web) {
                web_out.append(me.text, n);
                web_out.append("\n\r", 2);
            }
        }
        // wait up to 5 ms for frames, then drain what is there (bounded batch)
        if (xQueueReceive(g_frame_q, &fe, pdMS_TO_TICKS(5)) == pdTRUE) {
            int budget = 64;
            do {
                char line[96];
                main_out.append(line, format_main(fe, line, sizeof(line)));
                if (web) web_out.append(line, format_text(fe, line, sizeof(line)));
            } while (--budget > 0 && xQueueReceive(g_frame_q, &fe, 0) == pdTRUE);
        }
        main_out.flush();
        web_out.flush();
    }
}

}  // namespace

void host_init() {
    g_frame_q = xQueueCreate(HOST_FRAME_QUEUE_LEN, sizeof(FrameEvent));
    g_msg_q = xQueueCreate(HOST_MSG_QUEUE_LEN, sizeof(MsgEvent));
    g_out_mtx = xSemaphoreCreateMutex();
    g_rf = static_cast<ReplayFrame*>(calloc(REPLAY_FRAMES, sizeof(ReplayFrame)));
    g_rm = static_cast<ReplayMsg*>(calloc(REPLAY_MSGS, sizeof(ReplayMsg)));
    // above the web server (5) and the web sender (4): frames must keep flowing to USB/TCP
    xTaskCreatePinnedToCore(output_task, "host_out", 4096, nullptr, 6, nullptr,
                            portNUM_PROCESSORS > 1 ? 0 : tskNO_AFFINITY);
}

void host_replay_release() {
    portENTER_CRITICAL(&g_replay_mux);
    ReplayFrame* p = g_rf;
    g_rf = nullptr;
    g_rf_count = g_rf_head = 0;
    portEXIT_CRITICAL(&g_replay_mux);
    if (p) {
        free(p);
        host_notify("#info replay buffer released for the WLAN login (%u KB); frames until it is back cannot be "
                    "replayed", static_cast<unsigned>(REPLAY_FRAMES * sizeof(ReplayFrame) / 1024));
    }
}

bool host_replay_restore() {
    if (g_rf) return true;
    ReplayFrame* p = static_cast<ReplayFrame*>(calloc(REPLAY_FRAMES, sizeof(ReplayFrame)));
    if (!p) return false;   // heap still short: the caller tries again later
    portENTER_CRITICAL(&g_replay_mux);
    g_rf_count = g_rf_head = 0;
    g_rf_lost_until = now_us();
    g_rf = p;
    portEXIT_CRITICAL(&g_replay_mux);
    return true;
}

void host_set_reply_target(uint8_t sink, uint32_t web_client) {
    g_reply_sinks = sink;
    g_reply_client = web_client;
}
uint8_t host_reply_sink() { return g_reply_sinks; }
uint32_t host_reply_client() { return g_reply_client; }

void host_emit_rx(const CanFrame& f, uint64_t ts_us) {
    FrameEvent e;
    e.ts_us = ts_us;
    e.frame = f;
    e.is_tx = false;
    e.reason[0] = '\0';
    replay_push_frame(f, ts_us, false, nullptr);
    if (xQueueSend(g_frame_q, &e, 0) != pdTRUE) g_frames_dropped++;
}

void host_emit_tx(const CanFrame& f, uint64_t ts_us, const char* reason) {
    FrameEvent e;
    e.ts_us = ts_us;
    e.frame = f;
    e.is_tx = true;
    snprintf(e.reason, sizeof(e.reason), "%s", reason ? reason : "");
    replay_push_frame(f, ts_us, true, reason);
    if (xQueueSend(g_frame_q, &e, 0) != pdTRUE) g_frames_dropped++;
}

void host_notify(const char* fmt, ...) {
    MsgEvent m;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m.text, sizeof(m.text), fmt, ap);
    va_end(ap);
    replay_push_msg(m.text, now_us());
    if (!g_msg_q || xQueueSend(g_msg_q, &m, 0) != pdTRUE) g_msgs_dropped++;
}

void host_replay(uint64_t since_us) {
    // Everything after since_us, merged by time, sent in ~2 KB chunks:
    //   #replay begin since=<us> now=<us>
    //   #replay gap <from_us> <to_us>        (only if the buffer does not reach back far enough)
    //   #prx/#ptx <us> <id> <dlc> <data> [reason]   (same as #rx/#tx)
    //   #pmsg <us> <original notification line>
    //   #ok replay frames=<n> msgs=<m>
    const uint64_t now = now_us();
    char chunk[2200];
    size_t used = 0;
    auto flush = [&]() {
        if (!used) return;
        chunk[used] = '\0';
        host_reply_raw(chunk);
        used = 0;
    };
    auto add = [&](const char* line, size_t n) {
        if (used + n + 2 >= sizeof(chunk) - 1) flush();
        memcpy(chunk + used, line, n);
        used += n;
        chunk[used++] = '\n';
        chunk[used++] = '\r';
    };
    char line[MSG_LEN + 32];
    int n = snprintf(line, sizeof(line), "#replay begin since=%llu now=%llu", (unsigned long long)since_us,
                     (unsigned long long)now);
    add(line, n);

    // oldest buffered frame -> gap detection (only when the ring has wrapped)
    portENTER_CRITICAL(&g_replay_mux);
    const uint32_t f_count = g_rf ? g_rf_count : 0, f_head = g_rf_head, m_count = g_rm_count, m_head = g_rm_head;
    const uint64_t oldest = f_count ? g_rf[(f_head + REPLAY_FRAMES - f_count) % REPLAY_FRAMES].ts_us : now;
    const uint64_t lost_until = g_rf ? g_rf_lost_until : now;   // released right now: nothing is buffered
    portEXIT_CRITICAL(&g_replay_mux);
    uint64_t gap_to = 0;
    if (f_count == REPLAY_FRAMES && oldest > since_us) gap_to = oldest;   // ring wrapped
    if (lost_until > since_us && lost_until > gap_to) gap_to = lost_until;  // ring was released meanwhile
    if (gap_to) {
        n = snprintf(line, sizeof(line), "#replay gap %llu %llu", (unsigned long long)since_us,
                     (unsigned long long)gap_to);
        add(line, n);
    }

    // merge both rings by timestamp. Entries are copied one at a time under the lock; entries
    // overwritten meanwhile (very old ones) are simply skipped via the timestamp check.
    uint32_t fi = 0, mi = 0, nf = 0, nm = 0;
    const uint32_t f_start = (f_head + REPLAY_FRAMES - f_count) % REPLAY_FRAMES;
    const uint32_t m_start = (m_head + REPLAY_MSGS - m_count) % REPLAY_MSGS;
    ReplayFrame rf;
    ReplayMsg rm;
    bool have_f = false, have_m = false;
    uint64_t last_f = 0;
    for (;;) {
        while (!have_f && fi < f_count) {
            portENTER_CRITICAL(&g_replay_mux);
            const bool there = g_rf != nullptr;
            if (there) rf = g_rf[(f_start + fi) % REPLAY_FRAMES];
            portEXIT_CRITICAL(&g_replay_mux);
            fi++;
            if (!there) {   // released during this replay
                fi = f_count;
                break;
            }
            if (rf.ts_us > since_us && rf.ts_us <= now && rf.ts_us >= last_f) have_f = true;
        }
        while (!have_m && mi < m_count) {
            portENTER_CRITICAL(&g_replay_mux);
            rm = g_rm[(m_start + mi) % REPLAY_MSGS];
            portEXIT_CRITICAL(&g_replay_mux);
            mi++;
            if (rm.ts_us > since_us && rm.ts_us <= now) have_m = true;
        }
        if (!have_f && !have_m) break;
        if (have_f && (!have_m || rf.ts_us <= rm.ts_us)) {
            CanFrame f;
            f.id = rf.id;
            f.dlc = rf.dlc;
            f.ext = rf.flags & 2;
            f.rtr = rf.flags & 4;
            memcpy(f.data, rf.data, 8);
            const size_t len = text_format_frame((rf.flags & 1) ? "ptx" : "prx", rf.ts_us, f,
                                                 (rf.flags & 1) ? rf.reason : nullptr, line, sizeof(line));
            if (len) add(line, len);
            last_f = rf.ts_us;
            have_f = false;
            nf++;
        } else {
            n = snprintf(line, sizeof(line), "#pmsg %llu %s", (unsigned long long)rm.ts_us, rm.text);
            add(line, static_cast<size_t>(n) < sizeof(line) ? n : sizeof(line) - 1);
            have_m = false;
            nm++;
        }
    }
    n = snprintf(line, sizeof(line), "#ok replay frames=%lu msgs=%lu", (unsigned long)nf, (unsigned long)nm);
    add(line, n);
    flush();
}

void host_reply(const char* fmt, ...) {
    char buf[512];   // #status is ~300 characters
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t len = static_cast<size_t>(n) < sizeof(buf) - 2 ? static_cast<size_t>(n) : sizeof(buf) - 3;
    buf[len++] = '\n';
    buf[len++] = '\r';
    write_reply(buf, len);
}

void host_reply_raw(const char* s) { write_reply(s, strlen(s)); }

void host_set_format(OutFormat f) { g_format = f; }
OutFormat host_format() { return g_format; }
void host_set_slcan_timestamps(bool on) { g_slcan_ts = on; }
bool host_slcan_timestamps() { return g_slcan_ts; }
void host_set_echo(bool on) { g_echo = on; }
bool host_echo() { return g_echo; }

void host_get_stats(HostStats& s) {
    s.frames_dropped = g_frames_dropped;
    s.msgs_dropped = g_msgs_dropped;
}

void host_reset_stats() {
    g_frames_dropped = 0;
    g_msgs_dropped = 0;
}
