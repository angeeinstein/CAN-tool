#include "web.h"

#include "config.h"

#if ENABLE_WIFI

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <esp_random.h>

#include "can_bus.h"
#include "commands.h"
#include "generated/web_index_html.h"   // generated from web/index.html by tools/embed_web.py
#include "host_io.h"
#include "net.h"

namespace {

AsyncWebServer g_server(80);
AsyncWebSocket g_ws("/ws");

char g_token[33];   // random per boot; the page fetches it (basic auth) and sends "#auth <token>"

// ---- authenticated WebSocket clients -----------------------------------------------
constexpr int MAX_WS = 4;
volatile uint32_t g_authed[MAX_WS] = {0, 0, 0, 0};   // client ids, 0 = free
volatile int g_authed_count = 0;

bool is_authed(uint32_t id) {
    for (int i = 0; i < MAX_WS; i++)
        if (g_authed[i] == id) return true;
    return false;
}
void set_authed(uint32_t id) {
    for (int i = 0; i < MAX_WS; i++)
        if (g_authed[i] == 0) {
            g_authed[i] = id;
            g_authed_count++;
            return;
        }
}
void remove_authed(uint32_t id) {
    for (int i = 0; i < MAX_WS; i++)
        if (g_authed[i] == id) {
            g_authed[i] = 0;
            g_authed_count--;
        }
}

// ---- outgoing buffer (filled by output task + command replies, sent by loop task) ----
SemaphoreHandle_t g_buf_mtx = nullptr;
// two buffers, swapped every WEB_WS_FLUSH_MS: one is filled while the other is sent (no copy).
// 6 KB per 50 ms = 120 KB/s, enough for a fully loaded 125 kbit/s bus in text format.
constexpr size_t WEB_BUF_SIZE = 6144;
char g_buf_a[WEB_BUF_SIZE];
char g_buf_b[WEB_BUF_SIZE];
char* g_buf = g_buf_a;   // currently filled
size_t g_buf_len = 0;
volatile uint32_t g_ws_dropped = 0;

// ---- incoming command lines (async_tcp task -> loop task) ------------------------------
struct CmdLine {
    uint32_t client;   // WebSocket client id - the reply goes only to this browser
    char text[320];
};
QueueHandle_t g_cmd_q = nullptr;
uint32_t g_reply_client = 0;   // set while a browser command is executed (loop task)

// ---- deferred actions (executed in the loop task) ----------------------------------------
volatile bool g_reboot_pending = false;
uint32_t g_reboot_at = 0;

// ---- OTA ------------------------------------------------------------------------------
bool g_ota_ok = false;
bool g_ota_auth = false;
char g_ota_err[96] = "";

bool check_auth(AsyncWebServerRequest* r) {
    if (r->authenticate(WEB_USER, WEB_PASSWORD)) return true;
    r->requestAuthentication();
    return false;
}

void on_ws_event(AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data,
                 size_t len) {
    if (type == WS_EVT_CONNECT) {
        if (g_ws.count() > MAX_WS) {
            client->close();
            return;
        }
        client->text("#hello send \"#auth <token>\"\n\r");
    } else if (type == WS_EVT_DISCONNECT) {
        remove_authed(client->id());
    } else if (type == WS_EVT_DATA) {
        AwsFrameInfo* info = static_cast<AwsFrameInfo*>(arg);
        // only complete, single-frame text messages (commands are short)
        if (!(info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)) return;
        CmdLine cl;
        cl.client = client->id();
        size_t pos = 0;
        for (size_t i = 0; i <= len; i++) {
            const char c = i < len ? static_cast<char>(data[i]) : '\n';
            if (c == '\n' || c == '\r') {
                if (pos == 0) continue;
                cl.text[pos] = '\0';
                pos = 0;
                if (strncmp(cl.text, "#auth ", 6) == 0) {
                    if (strcmp(cl.text + 6, g_token) == 0) {
                        if (!is_authed(client->id())) set_authed(client->id());
                        client->text("#ok auth\n\r");
                    } else {
                        client->text("#err auth\n\r");
                    }
                } else if (is_authed(client->id())) {
                    xQueueSend(g_cmd_q, &cl, 0);
                }
            } else if (pos < sizeof(cl.text) - 1) {
                cl.text[pos++] = c;
            }
        }
    }
}

void handle_update_upload(AsyncWebServerRequest* r, const String& filename, size_t index, uint8_t* data, size_t len,
                          bool final) {
    if (index == 0) {
        g_ota_ok = false;
        g_ota_err[0] = '\0';
        g_ota_auth = r->authenticate(WEB_USER, WEB_PASSWORD);
        if (!g_ota_auth) return;
        // sanity check of the image header: magic byte and chip id must match this board
        if (len < 16 || data[0] != 0xE9) {
            snprintf(g_ota_err, sizeof(g_ota_err), "not an ESP32 firmware image (.bin)");
            return;
        }
        const uint16_t chip = static_cast<uint16_t>(data[12] | (data[13] << 8));
        if (chip != CONFIG_IDF_FIRMWARE_CHIP_ID) {
            snprintf(g_ota_err, sizeof(g_ota_err), "image is for another chip (id %u, this board is %s)", chip,
                     CONFIG_IDF_TARGET);
            return;
        }
        can_emergency_stop("firmware-update", false);   // device reboots anyway, no latch
        host_notify("#ota start %s", filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            snprintf(g_ota_err, sizeof(g_ota_err), "Update.begin: %s", Update.errorString());
            return;
        }
    }
    if (!g_ota_auth || g_ota_err[0]) return;
    if (len && Update.write(data, len) != len) {
        snprintf(g_ota_err, sizeof(g_ota_err), "write: %s", Update.errorString());
        Update.abort();
        return;
    }
    if (final) {
        if (Update.end(true)) {
            g_ota_ok = true;
        } else {
            snprintf(g_ota_err, sizeof(g_ota_err), "end: %s", Update.errorString());
        }
    }
}

void handle_update_done(AsyncWebServerRequest* r) {
    if (!g_ota_auth) {
        r->requestAuthentication();
        return;
    }
    if (g_ota_ok) {
        r->send(200, "text/plain", "OK, rebooting");
        host_notify("#ota done, rebooting");
        g_reboot_at = millis() + 800;
        g_reboot_pending = true;
    } else {
        host_notify("#ota failed: %s", g_ota_err[0] ? g_ota_err : "no data");
        r->send(400, "text/plain", g_ota_err[0] ? g_ota_err : "no data received");
    }
}

}  // namespace

void web_init() {
    g_buf_mtx = xSemaphoreCreateMutex();
    g_cmd_q = xQueueCreate(16, sizeof(CmdLine));
    snprintf(g_token, sizeof(g_token), "%08lx%08lx%08lx%08lx", (unsigned long)esp_random(),
             (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random());

    g_ws.onEvent(on_ws_event);
    g_server.addHandler(&g_ws);

    g_server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
        if (!check_auth(r)) return;
        AsyncWebServerResponse* res =
            r->beginResponse(200, "text/html", WEB_INDEX_HTML_GZ, WEB_INDEX_HTML_GZ_LEN);
        res->addHeader("Content-Encoding", "gzip");
        res->addHeader("Cache-Control", "no-store");
        r->send(res);
    });

    g_server.on("/api/token", HTTP_GET, [](AsyncWebServerRequest* r) {
        if (!check_auth(r)) return;
        char json[200];
        snprintf(json, sizeof(json),
                 "{\"token\":\"%s\",\"fw\":\"%s\",\"board\":\"%s\",\"build\":\"%s %s\",\"ui\":\"%s\"}", g_token,
                 FW_VERSION, CONFIG_IDF_TARGET, __DATE__, __TIME__, WEB_UI_HASH);
        AsyncWebServerResponse* res = r->beginResponse(200, "application/json", json);
        res->addHeader("Cache-Control", "no-store");
        r->send(res);
    });

    // WLAN management runs through the "#wifi ..." commands over the WebSocket

    g_server.on("/update", HTTP_POST, handle_update_done, handle_update_upload);

    g_server.onNotFound([](AsyncWebServerRequest* r) { r->send(404, "text/plain", "not found"); });
    g_server.begin();
}

void web_loop() {
    // commands from browsers
    CmdLine cl;
    int budget = 8;
    while (budget-- > 0 && xQueueReceive(g_cmd_q, &cl, 0) == pdTRUE) {
        g_reply_client = cl.client;
        cmd_handle_line(cl.text, SINK_WEB);
        g_reply_client = 0;
    }

    if (g_reboot_pending && static_cast<int32_t>(millis() - g_reboot_at) >= 0) ESP.restart();

    // live data / replies to the browsers, batched
    static uint32_t last_flush = 0;
    static uint32_t last_cleanup = 0;
    const uint32_t now = millis();
    if (now - last_cleanup > 1000) {
        last_cleanup = now;
        g_ws.cleanupClients(MAX_WS);
    }
    if (now - last_flush < WEB_WS_FLUSH_MS) return;
    last_flush = now;

    // swap buffers: send the filled one, new data goes into the other (c->text() copies the data)
    char* out;
    size_t len;
    xSemaphoreTake(g_buf_mtx, portMAX_DELAY);
    out = g_buf;
    len = g_buf_len;
    g_buf = (g_buf == g_buf_a) ? g_buf_b : g_buf_a;
    g_buf_len = 0;
    xSemaphoreGive(g_buf_mtx);
    if (len == 0) return;

    for (int i = 0; i < MAX_WS; i++) {
        const uint32_t id = g_authed[i];
        if (!id) continue;
        AsyncWebSocketClient* c = g_ws.client(id);
        if (!c || c->status() != WS_CONNECTED) continue;
        if (c->canSend()) c->text(out, len);
        else g_ws_dropped += len;   // slow browser: drop this batch instead of piling up
    }
}

void web_write(const char* buf, size_t len) {
    if (!g_buf_mtx || g_authed_count <= 0) return;
    if (xSemaphoreTake(g_buf_mtx, pdMS_TO_TICKS(2)) != pdTRUE) {
        g_ws_dropped += len;
        return;
    }
    if (g_buf_len + len <= WEB_BUF_SIZE) {
        memcpy(g_buf + g_buf_len, buf, len);
        g_buf_len += len;
    } else {
        g_ws_dropped += len;
    }
    xSemaphoreGive(g_buf_mtx);
}

bool web_has_clients() { return g_authed_count > 0; }

void web_reply(const char* buf, size_t len) {
    // runs in the loop task (same as web_loop), so the client cannot be freed meanwhile
    if (!g_reply_client || len == 0) return;
    AsyncWebSocketClient* c = g_ws.client(g_reply_client);
    if (c && c->status() == WS_CONNECTED) c->text(buf, len);
}

#else  // !ENABLE_WIFI

void web_init() {}
void web_loop() {}
void web_write(const char*, size_t) {}
bool web_has_clients() { return false; }
void web_reply(const char*, size_t) {}

#endif
