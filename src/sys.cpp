#include "sys.h"

#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <stdlib.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <string.h>

uint64_t now_us() { return static_cast<uint64_t>(esp_timer_get_time()); }
uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// ---- NVS --------------------------------------------------------------------------------

Nvs::Nvs(const char* ns, bool writable) {
    open_ = nvs_open(ns, writable ? NVS_READWRITE : NVS_READONLY, &h_) == ESP_OK;
}

Nvs::~Nvs() {
    if (!open_) return;
    if (dirty_) nvs_commit(h_);
    nvs_close(h_);
}

bool Nvs::has(const char* key) {
    nvs_type_t t;
    return open_ && nvs_find_key(h_, key, &t) == ESP_OK;
}

uint8_t Nvs::get_u8(const char* key, uint8_t def) {
    uint8_t v;
    return (open_ && nvs_get_u8(h_, key, &v) == ESP_OK) ? v : def;
}

uint32_t Nvs::get_u32(const char* key, uint32_t def) {
    uint32_t v;
    return (open_ && nvs_get_u32(h_, key, &v) == ESP_OK) ? v : def;
}

int32_t Nvs::get_i32(const char* key, int32_t def) {
    int32_t v;
    return (open_ && nvs_get_i32(h_, key, &v) == ESP_OK) ? v : def;
}

size_t Nvs::get_str(const char* key, char* out, size_t cap, const char* def) {
    if (cap == 0) return 0;
    size_t len = 0;
    if (open_ && nvs_get_str(h_, key, nullptr, &len) == ESP_OK && len > 0) {
        if (len <= cap) {
            if (nvs_get_str(h_, key, out, &len) == ESP_OK) return strlen(out);
        } else {   // longer than the buffer: read into a temporary copy and truncate
            char* tmp = static_cast<char*>(malloc(len));
            if (tmp && nvs_get_str(h_, key, tmp, &len) == ESP_OK) {
                strncpy(out, tmp, cap - 1);
                out[cap - 1] = '\0';
                free(tmp);
                return strlen(out);
            }
            free(tmp);
        }
    }
    strncpy(out, def ? def : "", cap - 1);
    out[cap - 1] = '\0';
    return strlen(out);
}

size_t Nvs::blob_size(const char* key) {
    size_t len = 0;
    return (open_ && nvs_get_blob(h_, key, nullptr, &len) == ESP_OK) ? len : 0;
}

bool Nvs::get_blob(const char* key, void* out, size_t len) {
    size_t l = len;
    return open_ && nvs_get_blob(h_, key, out, &l) == ESP_OK && l == len;
}

bool Nvs::set_u8(const char* key, uint8_t v) {
    if (!open_ || nvs_set_u8(h_, key, v) != ESP_OK) return false;
    dirty_ = true;
    return true;
}

bool Nvs::set_u32(const char* key, uint32_t v) {
    if (!open_ || nvs_set_u32(h_, key, v) != ESP_OK) return false;
    dirty_ = true;
    return true;
}

bool Nvs::set_i32(const char* key, int32_t v) {
    if (!open_ || nvs_set_i32(h_, key, v) != ESP_OK) return false;
    dirty_ = true;
    return true;
}

bool Nvs::set_str(const char* key, const char* v) {
    if (!open_ || nvs_set_str(h_, key, v ? v : "") != ESP_OK) return false;
    dirty_ = true;
    return true;
}

bool Nvs::set_blob(const char* key, const void* data, size_t len) {
    if (!open_ || nvs_set_blob(h_, key, data, len) != ESP_OK) return false;
    dirty_ = true;
    return true;
}

bool Nvs::erase(const char* key) {
    if (!open_) return false;
    const esp_err_t e = nvs_erase_key(h_, key);
    if (e == ESP_OK) dirty_ = true;
    return e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND;
}

// ---- memory -------------------------------------------------------------------------------

uint32_t heap_free() { return esp_get_free_heap_size(); }
uint32_t heap_min() { return esp_get_minimum_free_heap_size(); }
uint32_t heap_largest() { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }

// ---- reboot -------------------------------------------------------------------------------

namespace {
volatile bool g_reboot = false;
volatile uint32_t g_reboot_at = 0;
}  // namespace

void sys_request_reboot(uint32_t delay_ms) {
    g_reboot_at = now_ms() + delay_ms;
    g_reboot = true;
}

bool sys_reboot_due() { return g_reboot && static_cast<int32_t>(now_ms() - g_reboot_at) >= 0; }

// ---- OTA rollback -------------------------------------------------------------------------

bool sys_app_pending_verify() {
    esp_ota_img_states_t st;
    const esp_partition_t* run = esp_ota_get_running_partition();
    return run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

bool sys_app_confirm() {
    return sys_app_pending_verify() && esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
}
