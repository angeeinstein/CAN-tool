#include "rules.h"

#include <Preferences.h>
#include <esp_timer.h>

#include <memory>
#include <new>

#include "can_bus.h"
#include "config.h"
#include "host_io.h"
#include "rule_engine.h"

namespace {

RuleSet g_rules;
SemaphoreHandle_t g_rules_mtx = nullptr;

struct Pending {
    uint64_t due_us;
    uint32_t seq;
    CanFrame frame;
    char reason[RULE_NAME_LEN];
    bool used;
};

Pending g_pending[RULE_PENDING_MAX];
portMUX_TYPE g_pend_mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t g_pend_seq = 0;
volatile uint32_t g_pend_overflow = 0;
volatile uint32_t g_rule_skipped = 0;
TaskHandle_t g_sched_task = nullptr;

constexpr uint32_t BUSY_WAIT_US = 1500;   // below this, wait actively for sub-ms precision

struct EmitCtx {
    uint64_t ts_us;
    bool added;
};

void emit_cb(void* vctx, const Rule& rule, const RuleAction& action, const CanFrame& frame) {
    EmitCtx* ctx = static_cast<EmitCtx*>(vctx);
    bool ok = false;
    portENTER_CRITICAL(&g_pend_mux);
    for (auto& p : g_pending) {
        if (!p.used) {
            p.used = true;
            p.due_us = ctx->ts_us + static_cast<uint64_t>(action.delay_ms) * 1000ULL;
            p.seq = ++g_pend_seq;
            p.frame = frame;
            memcpy(p.reason, rule.name, sizeof(p.reason));
            ok = true;
            break;
        }
    }
    portEXIT_CRITICAL(&g_pend_mux);
    if (ok) {
        ctx->added = true;
    } else {
        g_pend_overflow++;
        host_notify("#warn rule %s: action queue full, frame dropped", rule.name);
    }
}

void sched_task(void*) {
    for (;;) {
        // find the earliest pending action
        int best = -1;
        uint64_t due = 0;
        uint32_t seq = 0;
        portENTER_CRITICAL(&g_pend_mux);
        for (int i = 0; i < RULE_PENDING_MAX; i++) {
            const Pending& p = g_pending[i];
            if (!p.used) continue;
            if (best < 0 || p.due_us < due || (p.due_us == due && p.seq < seq)) {
                best = i;
                due = p.due_us;
                seq = p.seq;
            }
        }
        portEXIT_CRITICAL(&g_pend_mux);

        if (best < 0) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        const int64_t wait = static_cast<int64_t>(due) - esp_timer_get_time();
        if (wait > static_cast<int64_t>(BUSY_WAIT_US)) {
            // sleep, but wake early if a new (possibly earlier) action arrives
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((wait - 1000) / 1000));
            continue;
        }
        if (wait > 0) delayMicroseconds(static_cast<uint32_t>(wait));

        Pending p;
        bool have = false;
        portENTER_CRITICAL(&g_pend_mux);
        if (g_pending[best].used && g_pending[best].seq == seq) {
            p = g_pending[best];
            g_pending[best].used = false;
            have = true;
        }
        portEXIT_CRITICAL(&g_pend_mux);
        if (have) can_transmit(p.frame, p.reason);
    }
}

// ---- NVS -------------------------------------------------------------------------

constexpr const char* NVS_NS = "franke";
constexpr const char* NVS_KEY = "rules";

constexpr size_t BLOB_SIZE = RULE_MAX * (RULE_NAME_LEN + RULE_SRC_LEN + 4) + 1;

void cmd_save() {
    // format per rule: "<name>\t<1|0>\t<src>\n"  (buffer only allocated while saving)
    std::unique_ptr<char[]> buf(new (std::nothrow) char[BLOB_SIZE]);
    if (!buf) {
        host_reply("#err rule save: out of memory");
        return;
    }
    char* blob = buf.get();
    size_t len = 0;
    int n = 0;
    xSemaphoreTake(g_rules_mtx, portMAX_DELAY);
    for (int i = 0; i < g_rules.count(); i++) {
        const Rule& r = g_rules.at(i);
        len += snprintf(blob + len, BLOB_SIZE - len, "%s\t%d\t%s\n", r.name, r.enabled ? 1 : 0, r.src);
        n++;
    }
    xSemaphoreGive(g_rules_mtx);

    Preferences prefs;
    if (!prefs.begin(NVS_NS, false)) {
        host_reply("#err rule save: NVS not available");
        return;
    }
    bool ok = (len == 0) ? (prefs.remove(NVS_KEY) || true) : (prefs.putBytes(NVS_KEY, blob, len) == len);
    prefs.end();
    if (ok)
        host_reply("#ok rule save %d", n);
    else
        host_reply("#err rule save failed");
}

void cmd_load() {
    std::unique_ptr<char[]> buf(new (std::nothrow) char[BLOB_SIZE]);   // only while loading
    if (!buf) {
        host_reply("#err rule load: out of memory");
        return;
    }
    char* blob = buf.get();
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) {
        host_reply("#err rule load: nothing saved");
        return;
    }
    size_t len = prefs.getBytesLength(NVS_KEY);
    if (len == 0 || len >= BLOB_SIZE) {
        prefs.end();
        host_reply("#err rule load: nothing saved");
        return;
    }
    prefs.getBytes(NVS_KEY, blob, len);
    prefs.end();
    blob[len] = '\0';

    RuleSet* tmp = new RuleSet();   // parse into a temporary set, then swap in
    int n = 0, failed = 0;
    char* line = blob;
    while (line && *line) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char* t1 = strchr(line, '\t');
        char* t2 = t1 ? strchr(t1 + 1, '\t') : nullptr;
        if (t1 && t2) {
            *t1 = '\0';
            *t2 = '\0';
            char def[RULE_NAME_LEN + RULE_SRC_LEN + 2];
            snprintf(def, sizeof(def), "%s %s", line, t2 + 1);
            char err[80];
            if (tmp->add(def, err, sizeof(err))) {
                tmp->set_enabled(line, t1[1] == '1');
                n++;
            } else {
                failed++;
                host_reply("#warn rule load %s: %s", line, err);
            }
        }
        line = nl ? nl + 1 : nullptr;
    }
    rules_clear_pending();
    xSemaphoreTake(g_rules_mtx, portMAX_DELAY);
    g_rules = *tmp;
    xSemaphoreGive(g_rules_mtx);
    delete tmp;
    host_reply("#ok rule load %d%s", n, failed ? " (some failed)" : "");
}

void cmd_list() {
    int n;
    xSemaphoreTake(g_rules_mtx, portMAX_DELAY);
    n = g_rules.count();
    xSemaphoreGive(g_rules_mtx);
    for (int i = 0; i < n; i++) {
        Rule r;
        bool have = false;
        xSemaphoreTake(g_rules_mtx, portMAX_DELAY);
        if (i < g_rules.count()) {
            r = g_rules.at(i);
            have = true;
        }
        xSemaphoreGive(g_rules_mtx);
        if (!have) break;
        const char* st = !r.enabled ? "off" : (r.spent ? "spent" : "on");
        host_reply("#rule %s %s hits=%lu %s", r.name, st, (unsigned long)r.hits, r.src);
    }
    host_reply("#ok rule list %d (rules act only in ACTIVE mode, now %s)", n, can_mode_name(can_mode()));
}

}  // namespace

void rules_init() {
    g_rules_mtx = xSemaphoreCreateMutex();
    for (auto& p : g_pending) p.used = false;
    xTaskCreatePinnedToCore(sched_task, "rule_tx", 4096, nullptr, configMAX_PRIORITIES - 1, &g_sched_task,
                            portNUM_PROCESSORS > 1 ? 1 : tskNO_AFFINITY);
}

void rules_on_frame(const CanFrame& f, uint64_t ts_us) {
    if (xSemaphoreTake(g_rules_mtx, pdMS_TO_TICKS(2)) != pdTRUE) {
        g_rule_skipped++;
        return;
    }
    EmitCtx ctx{ts_us, false};
    g_rules.process(f, emit_cb, &ctx);
    xSemaphoreGive(g_rules_mtx);
    if (ctx.added && g_sched_task) xTaskNotifyGive(g_sched_task);
}

void rules_clear_pending() {
    portENTER_CRITICAL(&g_pend_mux);
    for (auto& p : g_pending) p.used = false;
    portEXIT_CRITICAL(&g_pend_mux);
    if (g_sched_task) xTaskNotifyGive(g_sched_task);
}

void rules_disable_all() {
    if (!g_rules_mtx) return;
    xSemaphoreTake(g_rules_mtx, portMAX_DELAY);
    g_rules.set_all_enabled(false);
    xSemaphoreGive(g_rules_mtx);
}

int rules_count() {
    xSemaphoreTake(g_rules_mtx, portMAX_DELAY);
    int n = g_rules.count();
    xSemaphoreGive(g_rules_mtx);
    return n;
}

int rules_enabled_count() {
    xSemaphoreTake(g_rules_mtx, portMAX_DELAY);
    int n = g_rules.enabled_count();
    xSemaphoreGive(g_rules_mtx);
    return n;
}

uint32_t rules_pending_overflows() { return g_pend_overflow; }

void rules_command(char* args) {
    while (*args == ' ') args++;
    char* rest = args;
    while (*rest && *rest != ' ') rest++;
    if (*rest) *rest++ = '\0';
    while (*rest == ' ') rest++;
    const char* sub = args;

    // STOP latch: rules can be listed, disabled and deleted, but not added or (re)enabled
    if (can_stop_latched() &&
        (strcmp(sub, "add") == 0 || strcmp(sub, "enable") == 0 || strcmp(sub, "load") == 0)) {
        host_reply("#err rule %s refused: STOP latched (stops=%lu) - send #resume first", sub,
                   (unsigned long)can_stop_count());
        return;
    }
    if (strcmp(sub, "add") == 0) {
        char err[96];
        xSemaphoreTake(g_rules_mtx, portMAX_DELAY);
        bool ok = g_rules.add(rest, err, sizeof(err));
        xSemaphoreGive(g_rules_mtx);
        if (ok) {
            char name[RULE_NAME_LEN];
            sscanf(rest, "%15s", name);
            host_reply("#ok rule add %s", name);
        } else {
            host_reply("#err rule add: %s", err);
        }
    } else if (strcmp(sub, "del") == 0) {
        xSemaphoreTake(g_rules_mtx, portMAX_DELAY);
        bool ok = g_rules.remove(rest);
        xSemaphoreGive(g_rules_mtx);
        if (ok) host_reply("#ok rule del %s", rest);
        else host_reply("#err rule del: no rule '%s'", rest);
    } else if (strcmp(sub, "list") == 0 || *sub == '\0') {
        cmd_list();
    } else if (strcmp(sub, "enable") == 0 || strcmp(sub, "disable") == 0) {
        const bool en = sub[0] == 'e';
        bool ok;
        xSemaphoreTake(g_rules_mtx, portMAX_DELAY);
        if (strcmp(rest, "all") == 0) {
            g_rules.set_all_enabled(en);
            ok = true;
        } else {
            ok = g_rules.set_enabled(rest, en);
        }
        xSemaphoreGive(g_rules_mtx);
        if (ok) host_reply("#ok rule %s %s", sub, rest);
        else host_reply("#err rule %s: no rule '%s'", sub, rest);
    } else if (strcmp(sub, "clear") == 0) {
        rules_clear_pending();
        xSemaphoreTake(g_rules_mtx, portMAX_DELAY);
        g_rules.clear();
        xSemaphoreGive(g_rules_mtx);
        host_reply("#ok rule clear");
    } else if (strcmp(sub, "save") == 0) {
        cmd_save();
    } else if (strcmp(sub, "load") == 0) {
        cmd_load();
    } else {
        host_reply("#err rule: unknown subcommand '%s' (add|del|list|enable|disable|clear|save|load)", sub);
    }
}
