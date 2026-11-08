#include "web.h"

#include <atomic>
#include <errno.h>
#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <mbedtls/base64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "can_bus.h"
#include "commands.h"
#include "config.h"
#include "generated/web_index_html.h"   // generated from web/index.html by tools/embed_web.py
#include "host_io.h"
#include "sys.h"

namespace {

httpd_handle_t g_server = nullptr;
TaskHandle_t g_tx_task = nullptr;
char g_token[33];              // random per boot; the page fetches it (basic auth) and sends "#auth <token>"
char g_basic_auth[160];        // expected "Authorization" header value

// ---- WebSocket clients ------------------------------------------------------------------
// The http server task adds a client after the handshake and removes it in close_fn. The web_tx task
// writes to the socket itself; a socket it is writing to is only closed after that write ("busy"),
// so a socket is never closed (and its number reused) under a running send.
constexpr int MAX_SLOTS = WEB_MAX_CLIENTS + 2;   // a few unauthenticated ones may be connecting
struct Client {
    uint32_t id;          // 0 = free slot
    int fd;
    bool authed;
    bool busy;            // web_tx is sending to it
    bool close_pending;   // http server closed the session while busy -> web_tx closes the socket
};
Client g_clients[MAX_SLOTS];
SemaphoreHandle_t g_cli_mtx = nullptr;
uint32_t g_next_id = 0;
std::atomic<int> g_authed_count{0};

// ---- live data: filled by the output task, sent every WEB_WS_FLUSH_MS -------------------------------
// Two buffers: one is filled while the other is sent. HDR bytes in front for the WebSocket header.
constexpr size_t HDR = 4;
constexpr size_t LIVE_SIZE = 4096;   // 40 KB/s at 10 batches/s (400 frames/s in text = ~18 KB/s)
char g_live_a[HDR + LIVE_SIZE];
char g_live_b[HDR + LIVE_SIZE];
char* g_live = g_live_a;             // currently filled
size_t g_live_len = 0;
SemaphoreHandle_t g_live_mtx = nullptr;

// ---- command replies: filled by the main task for one browser at a time ---------------------------
constexpr size_t REPLY_SIZE = 4096;
char g_reply[HDR + REPLY_SIZE];
size_t g_reply_len = 0;
uint32_t g_reply_owner = 0;
SemaphoreHandle_t g_reply_mtx = nullptr;

// ---- statistics / state ---------------------------------------------------------------------------
std::atomic<uint32_t> g_dropped_bytes{0};
std::atomic<uint32_t> g_dropped_batches{0};
std::atomic<uint32_t> g_slow_closes{0};
std::atomic<uint32_t> g_send_ms_peak{0};
volatile bool g_ota_active = false;   // firmware upload running: live data paused

// Copies len bytes and replaces every byte that is not part of a valid UTF-8 sequence with '?'.
// Browsers close a WebSocket that receives a text frame with invalid UTF-8 - one raw Latin-1 byte (e.g. in
// an SSID or a marker note) would disconnect every browser, and #replay would repeat it after reconnect.
void utf8_copy(char* dst, const char* src, size_t len) {
    size_t i = 0;
    while (i < len) {
        const uint8_t c = static_cast<uint8_t>(src[i]);
        if (c < 0x80) {
            dst[i] = src[i];
            i++;
            continue;
        }
        size_t n = 0;
        uint8_t lo = 0x80, hi = 0xBF;   // allowed range of the 2nd byte (excludes overlong / surrogates)
        if (c >= 0xC2 && c <= 0xDF) n = 1;
        else if (c >= 0xE0 && c <= 0xEF) {
            n = 2;
            if (c == 0xE0) lo = 0xA0;
            if (c == 0xED) hi = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            n = 3;
            if (c == 0xF0) lo = 0x90;
            if (c == 0xF4) hi = 0x8F;
        }
        bool ok = n > 0;   // valid lead byte
        if (ok) {
            for (size_t k = 1; k <= n; k++) {
                if (i + k >= len) {   // sequence cut off at the end of the buffer
                    ok = false;
                    break;
                }
                const uint8_t b = static_cast<uint8_t>(src[i + k]);
                if (k == 1 ? (b < lo || b > hi) : (b < 0x80 || b > 0xBF)) {
                    ok = false;
                    break;
                }
            }
        }
        if (ok) {
            memcpy(dst + i, src + i, n + 1);
            i += n + 1;
        } else {
            dst[i++] = '?';
        }
    }
}

Client* find_id(uint32_t id) {
    for (auto& c : g_clients)
        if (id && c.id == id) return &c;
    return nullptr;
}

void free_slot(Client& c) {
    if (c.authed) g_authed_count--;
    c = Client{};
}

// Writes one complete WebSocket text frame. payload must have HDR free bytes in front of it.
// Returns false on error / timeout (SO_SNDTIMEO = WEB_SEND_TIMEOUT_S, set by the http server).
bool ws_send_frame(int fd, char* payload, size_t len) {
    char* frame;
    size_t hl;
    if (len <= 125) {
        hl = 2;
        frame = payload - hl;
        frame[1] = static_cast<char>(len);
    } else {   // len <= 65535 (our buffers are smaller)
        hl = 4;
        frame = payload - hl;
        frame[1] = 126;
        frame[2] = static_cast<char>((len >> 8) & 0xff);
        frame[3] = static_cast<char>(len & 0xff);
    }
    frame[0] = static_cast<char>(0x81);   // FIN + text
    const uint32_t t0 = now_ms();
    size_t off = 0, total = hl + len;
    while (off < total) {
        const ssize_t n = send(fd, frame + off, total - off, 0);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    const uint32_t dt = now_ms() - t0;
    if (dt > g_send_ms_peak) g_send_ms_peak = dt;
    return true;
}

// Sends to one client with the busy protocol. Returns false if the client is gone or the send failed.
bool send_to(uint32_t id, char* payload, size_t len) {
    xSemaphoreTake(g_cli_mtx, portMAX_DELAY);
    Client* c = find_id(id);
    if (!c || !c->authed || c->close_pending) {
        xSemaphoreGive(g_cli_mtx);
        return false;
    }
    c->busy = true;
    const int fd = c->fd;
    xSemaphoreGive(g_cli_mtx);

    const bool ok = ws_send_frame(fd, payload, len);
    if (!ok) shutdown(fd, SHUT_RDWR);   // the http server notices and closes the session (close_fn)

    xSemaphoreTake(g_cli_mtx, portMAX_DELAY);
    c = find_id(id);
    if (c) {
        c->busy = false;
        if (c->close_pending) {
            close(c->fd);
            free_slot(*c);
        }
    }
    xSemaphoreGive(g_cli_mtx);
    if (!ok) g_slow_closes++;
    return ok;
}

void flush_replies() {
    xSemaphoreTake(g_reply_mtx, portMAX_DELAY);
    if (g_reply_len) {
        send_to(g_reply_owner, g_reply + HDR, g_reply_len);   // a failed send drops the reply (client is gone)
        g_reply_len = 0;
        g_reply_owner = 0;
    }
    xSemaphoreGive(g_reply_mtx);
}

void flush_live() {
    xSemaphoreTake(g_live_mtx, portMAX_DELAY);
    char* out = g_live;
    const size_t len = g_live_len;
    g_live = (g_live == g_live_a) ? g_live_b : g_live_a;
    g_live_len = 0;
    xSemaphoreGive(g_live_mtx);
    if (len == 0 || g_ota_active) return;

    uint32_t ids[MAX_SLOTS];
    int n = 0;
    xSemaphoreTake(g_cli_mtx, portMAX_DELAY);
    for (auto& c : g_clients)
        if (c.id && c.authed && !c.close_pending) ids[n++] = c.id;
    xSemaphoreGive(g_cli_mtx);
    for (int i = 0; i < n; i++) {
        // the header is written in front of the payload for every client (same bytes each time)
        if (!send_to(ids[i], out + HDR, len)) g_dropped_batches++;
    }
}

void tx_task(void*) {
    uint32_t last_flush = now_ms();
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WEB_WS_FLUSH_MS));
        flush_replies();
        const uint32_t now = now_ms();
        if (now - last_flush >= WEB_WS_FLUSH_MS) {
            last_flush = now;
            flush_live();
        }
    }
}

// ---- http server callbacks ------------------------------------------------------------------------

// Called by the http server for every closed session (HTTP and WebSocket). We close the socket.
void on_close(httpd_handle_t, int fd) {
    bool deferred = false;
    xSemaphoreTake(g_cli_mtx, portMAX_DELAY);
    for (auto& c : g_clients) {   // all slots of this socket (there should only ever be one)
        if (c.id && c.fd == fd) {
            if (c.busy) {
                c.close_pending = true;   // web_tx closes the socket after its send
                deferred = true;
            } else {
                free_slot(c);
            }
        }
    }
    xSemaphoreGive(g_cli_mtx);
    if (!deferred) close(fd);
}

bool check_auth(httpd_req_t* r) {
    char hdr[sizeof(g_basic_auth)];
    if (httpd_req_get_hdr_value_str(r, "Authorization", hdr, sizeof(hdr)) == ESP_OK && strcmp(hdr, g_basic_auth) == 0)
        return true;
    httpd_resp_set_status(r, "401 Unauthorized");
    httpd_resp_set_hdr(r, "WWW-Authenticate", "Basic realm=\"franke-can\"");
    httpd_resp_sendstr(r, "authentication required");
    return false;
}

esp_err_t h_index(httpd_req_t* r) {
    if (!check_auth(r)) return ESP_OK;
    httpd_resp_set_type(r, "text/html");
    httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, reinterpret_cast<const char*>(WEB_INDEX_HTML_GZ), WEB_INDEX_HTML_GZ_LEN);
}

esp_err_t h_token(httpd_req_t* r) {
    if (!check_auth(r)) return ESP_OK;
    char json[220];
    snprintf(json, sizeof(json), "{\"token\":\"%s\",\"fw\":\"%s\",\"board\":\"%s\",\"build\":\"%s %s\",\"ui\":\"%s\"}",
             g_token, FW_VERSION, CONFIG_IDF_TARGET, __DATE__, __TIME__, WEB_UI_HASH);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, json);
}

// Sends a short text frame from the http server task (only before the client is authenticated, so
// web_tx never writes to the same socket at the same time).
void ws_text(httpd_req_t* r, const char* s) {
    httpd_ws_frame_t f = {};
    f.type = HTTPD_WS_TYPE_TEXT;
    f.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(s));
    f.len = strlen(s);
    httpd_ws_send_frame(r, &f);
}

esp_err_t h_ws(httpd_req_t* r) {
    const int fd = httpd_req_to_sockfd(r);
    if (r->method == HTTP_GET) {   // handshake done: new connection
        // httpd also calls this handler for a plain "GET /ws" without the upgrade headers: no slot for it
        if (httpd_ws_get_fd_info(r->handle, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "WebSocket only");
            return ESP_FAIL;
        }
        bool added = false;
        xSemaphoreTake(g_cli_mtx, portMAX_DELAY);
        for (auto& c : g_clients)   // never two slots for one socket
            if (c.id && c.fd == fd && !c.busy) free_slot(c);
        for (auto& c : g_clients) {
            if (c.id == 0) {
                c = Client{};
                c.id = ++g_next_id ? g_next_id : ++g_next_id;
                c.fd = fd;
                added = true;
                break;
            }
        }
        xSemaphoreGive(g_cli_mtx);
        if (!added) {
            ws_text(r, "#err too many connections\n\r");
            return ESP_FAIL;   // closes the connection
        }
        ws_text(r, "#hello send \"#auth <token>\"\n\r");
        return ESP_OK;
    }

    httpd_ws_frame_t f = {};
    if (httpd_ws_recv_frame(r, &f, 0) != ESP_OK) return ESP_FAIL;
    if (f.len == 0) return ESP_OK;
    if (f.len > 1024) return ESP_FAIL;   // commands are short; anything else is not ours
    char buf[1025];
    f.payload = reinterpret_cast<uint8_t*>(buf);
    if (httpd_ws_recv_frame(r, &f, f.len) != ESP_OK) return ESP_FAIL;
    if (f.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;
    buf[f.len] = '\0';

    uint32_t id = 0;
    bool authed = false;
    xSemaphoreTake(g_cli_mtx, portMAX_DELAY);
    for (auto& c : g_clients)
        if (c.id && c.fd == fd) {
            id = c.id;
            authed = c.authed;
        }
    xSemaphoreGive(g_cli_mtx);
    if (!id) return ESP_FAIL;

    char* save = nullptr;
    for (char* line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(nullptr, "\r\n", &save)) {
        if (strncmp(line, "#auth ", 6) == 0) {
            if (authed) continue;
            if (strcmp(line + 6, g_token) != 0) {
                ws_text(r, "#err auth\n\r");
                continue;
            }
            ws_text(r, "#ok auth\n\r");   // sent before "authed" is set: web_tx does not write yet
            xSemaphoreTake(g_cli_mtx, portMAX_DELAY);
            Client* c = find_id(id);
            if (c && !c->authed) {
                c->authed = true;
                g_authed_count++;
                authed = true;
            }
            xSemaphoreGive(g_cli_mtx);
        } else if (authed) {
            if (!cmd_post(SINK_WEB, id, line, 0)) g_dropped_bytes += strlen(line);   // queue full: very rare
        }
    }
    return ESP_OK;
}

// ---- firmware upload --------------------------------------------------------------------------------
// POST /update with the image as the raw body (web UI) or as multipart/form-data (curl -F). The image is
// checked (ESP image magic, chip, project) before anything is written. While it runs, live data to the
// browsers is paused. A broken upload leaves the running firmware untouched.

esp_err_t ota_fail(httpd_req_t* r, esp_ota_handle_t h, const char* why) {
    if (h) esp_ota_abort(h);
    g_ota_active = false;
    host_notify("#ota failed: %s", why);
    httpd_resp_set_status(r, "400 Bad Request");
    httpd_resp_sendstr(r, why);
    return ESP_OK;
}

// receive exactly len bytes (or fewer at the end); tolerates a few receive timeouts (slow WLAN)
int recv_some(httpd_req_t* r, char* buf, size_t len) {
    for (int tries = 0; tries < 4; tries++) {
        const int n = httpd_req_recv(r, buf, len);
        if (n != HTTPD_SOCK_ERR_TIMEOUT) return n;
    }
    return HTTPD_SOCK_ERR_TIMEOUT;
}

bool image_ok(const uint8_t* d, size_t len, char* err, size_t cap) {
    constexpr size_t DESC_OFS = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (len < DESC_OFS + sizeof(esp_app_desc_t) || d[0] != ESP_IMAGE_HEADER_MAGIC) {
        snprintf(err, cap, "not an ESP32 firmware image (.bin)");
        return false;
    }
    const esp_image_header_t* ih = reinterpret_cast<const esp_image_header_t*>(d);
    if (ih->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        snprintf(err, cap, "image is for another chip (id %u, this board is %s)", ih->chip_id, CONFIG_IDF_TARGET);
        return false;
    }
    esp_app_desc_t desc;
    memcpy(&desc, d + DESC_OFS, sizeof(desc));
    char name[33];
    snprintf(name, sizeof(name), "%.32s", desc.project_name);
    // our project; "arduino-lib-builder" = the former Arduino firmware (allows going back)
    if (desc.magic_word == ESP_APP_DESC_MAGIC_WORD && strcmp(name, "franke_can") != 0 &&
        strcmp(name, "arduino-lib-builder") != 0) {
        snprintf(err, cap, "image is another project ('%s'), not the franke-can firmware", name);
        return false;
    }
    return true;
}

esp_err_t h_update(httpd_req_t* r) {
    if (!check_auth(r)) return ESP_OK;
    size_t remaining = r->content_len;

    // multipart: skip the part header now, and do not write the closing "\r\n--<boundary>--\r\n"
    char ctype[160] = "";
    httpd_req_get_hdr_value_str(r, "Content-Type", ctype, sizeof(ctype));
    size_t tail = 0;
    const char* b = strstr(ctype, "boundary=");
    const bool multipart = strncmp(ctype, "multipart/form-data", 19) == 0 && b;
    if (multipart) tail = 4 + strlen(b + 9);   // "\r\n--" + boundary (a trailing "--\r\n" is harmless)

    constexpr size_t BUF = 4096;
    char* buf = static_cast<char*>(malloc(BUF));
    if (!buf) return ota_fail(r, 0, "out of memory");

    // first chunk: (multipart header) + image header
    size_t have = 0;
    while (have < BUF && remaining > 0) {
        const int n = recv_some(r, buf + have, (BUF - have) < remaining ? BUF - have : remaining);
        if (n <= 0) {
            free(buf);
            return ota_fail(r, 0, "upload connection lost");
        }
        have += n;
        remaining -= n;
        if (have >= 1024 || remaining == 0) break;
    }
    size_t start = 0;
    if (multipart) {
        const char* p = static_cast<const char*>(memmem(buf, have, "\r\n\r\n", 4));
        if (!p) {
            free(buf);
            return ota_fail(r, 0, "bad multipart upload");
        }
        start = (p - buf) + 4;
    }
    char err[96];
    if (!image_ok(reinterpret_cast<uint8_t*>(buf + start), have - start, err, sizeof(err))) {
        free(buf);
        return ota_fail(r, 0, err);
    }

    // a firmware still "pending verify" cannot be updated (ESP_ERR_OTA_ROLLBACK_INVALID_STATE); a new
    // upload means the running one is accepted
    sys_app_confirm();
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    esp_ota_handle_t h = 0;
    const esp_err_t be = part ? esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) : ESP_ERR_NOT_FOUND;
    if (be != ESP_OK) {
        free(buf);
        snprintf(err, sizeof(err), "esp_ota_begin: %s", esp_err_to_name(be));
        return ota_fail(r, 0, err);
    }
    can_emergency_stop("firmware-update", false);   // device reboots anyway, no latch
    g_ota_active = true;
    const size_t image_len = (have - start) + remaining - (tail <= remaining ? tail : 0);
    host_notify("#ota start %u bytes -> %s", static_cast<unsigned>(image_len), part->label);

    size_t written = 0;
    auto write = [&](const char* d, size_t n) -> bool {
        if (written + n > image_len) n = image_len - written;   // multipart trailer
        if (n == 0) return true;
        written += n;
        return esp_ota_write(h, d, n) == ESP_OK;
    };
    if (!write(buf + start, have - start)) {
        free(buf);
        return ota_fail(r, h, "flash write failed");
    }
    while (remaining > 0) {
        const int n = recv_some(r, buf, remaining < BUF ? remaining : BUF);
        if (n <= 0) {
            free(buf);
            return ota_fail(r, h, "upload connection lost");
        }
        remaining -= n;
        if (!write(buf, n)) {
            free(buf);
            return ota_fail(r, h, "flash write failed");
        }
    }
    free(buf);
    const esp_err_t e = esp_ota_end(h);   // verifies the image (segments, checksum, SHA-256)
    if (e != ESP_OK) {
        g_ota_active = false;
        host_notify("#ota failed: image invalid (%s)", esp_err_to_name(e));
        httpd_resp_set_status(r, "400 Bad Request");
        return httpd_resp_sendstr(r, "image invalid");
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) return ota_fail(r, 0, "cannot select the new firmware");
    httpd_resp_sendstr(r, "OK, rebooting");
    host_notify("#ota done (%u bytes), rebooting", static_cast<unsigned>(written));
    sys_request_reboot(800);
    return ESP_OK;
}

}  // namespace

void web_init() {
    g_cli_mtx = xSemaphoreCreateMutex();
    g_live_mtx = xSemaphoreCreateMutex();
    g_reply_mtx = xSemaphoreCreateMutex();
    snprintf(g_token, sizeof(g_token), "%08lx%08lx%08lx%08lx", (unsigned long)esp_random(),
             (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random());
    // "Basic " + base64(user:password)
    char up[96];
    const int ul = snprintf(up, sizeof(up), "%s:%s", WEB_USER, WEB_PASSWORD);
    size_t olen = 0;
    strcpy(g_basic_auth, "Basic ");
    mbedtls_base64_encode(reinterpret_cast<unsigned char*>(g_basic_auth + 6), sizeof(g_basic_auth) - 7, &olen,
                          reinterpret_cast<const unsigned char*>(up), ul);
    g_basic_auth[6 + olen] = '\0';

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_open_sockets = 10;          // + 3 internal sockets, see CONFIG_LWIP_MAX_SOCKETS
    cfg.lru_purge_enable = true;        // a new connection closes the least recently active one
    cfg.send_wait_timeout = WEB_SEND_TIMEOUT_S;
    cfg.recv_wait_timeout = 5;
    cfg.stack_size = 5120;
    cfg.max_uri_handlers = 6;
    cfg.close_fn = on_close;
    if (httpd_start(&g_server, &cfg) != ESP_OK) {
        host_notify("#error web server did not start");
        return;
    }
    const httpd_uri_t index = {"/", HTTP_GET, h_index, nullptr, false, false, nullptr};
    const httpd_uri_t token = {"/api/token", HTTP_GET, h_token, nullptr, false, false, nullptr};
    const httpd_uri_t update = {"/update", HTTP_POST, h_update, nullptr, false, false, nullptr};
    const httpd_uri_t ws = {"/ws", HTTP_GET, h_ws, nullptr, true, false, nullptr};
    httpd_register_uri_handler(g_server, &index);
    httpd_register_uri_handler(g_server, &token);
    httpd_register_uri_handler(g_server, &update);
    httpd_register_uri_handler(g_server, &ws);

    xTaskCreatePinnedToCore(tx_task, "web_tx", 3072, nullptr, 4, &g_tx_task, 0);
}

void web_write(const char* buf, size_t len) {
    if (!g_live_mtx || g_authed_count <= 0 || g_ota_active) return;
    if (xSemaphoreTake(g_live_mtx, pdMS_TO_TICKS(2)) != pdTRUE) {
        g_dropped_bytes += len;
        return;
    }
    if (g_live_len + len <= LIVE_SIZE) {
        utf8_copy(g_live + HDR + g_live_len, buf, len);
        g_live_len += len;
    } else {
        g_dropped_bytes += len;
    }
    xSemaphoreGive(g_live_mtx);
}

void web_reply(uint32_t client, const char* buf, size_t len) {
    if (!g_reply_mtx || !client) return;
    // split long replies; wait while the buffer holds data of another browser or is full
    while (len > 0) {
        const size_t part = len < REPLY_SIZE ? len : REPLY_SIZE;
        bool done = false;
        for (int waited = 0; waited < 5000 && !done; waited += 2) {
            if (!web_client_connected(client)) return;   // gone: nothing to deliver
            xSemaphoreTake(g_reply_mtx, portMAX_DELAY);
            if ((g_reply_len == 0 || g_reply_owner == client) && g_reply_len + part <= REPLY_SIZE) {
                utf8_copy(g_reply + HDR + g_reply_len, buf, part);
                g_reply_len += part;
                g_reply_owner = client;
                done = true;
            }
            xSemaphoreGive(g_reply_mtx);
            if (g_tx_task) xTaskNotifyGive(g_tx_task);
            if (!done) vTaskDelay(pdMS_TO_TICKS(2));
        }
        if (!done) return;   // browser does not take data: web_tx disconnects it
        buf += part;
        len -= part;
    }
}

bool web_has_clients() { return g_authed_count > 0; }

bool web_client_connected(uint32_t id) {
    if (!g_cli_mtx || !id) return false;
    xSemaphoreTake(g_cli_mtx, portMAX_DELAY);
    const Client* c = find_id(id);
    const bool ok = c && c->authed && !c->close_pending;
    xSemaphoreGive(g_cli_mtx);
    return ok;
}

void web_get_stats(WebStats& s) {
    s.dropped_bytes = g_dropped_bytes;
    s.dropped_batches = g_dropped_batches;
    s.slow_closes = g_slow_closes;
    s.clients = g_authed_count;
    s.send_ms_peak = g_send_ms_peak;
}

void web_reset_peaks() { g_send_ms_peak = 0; }
