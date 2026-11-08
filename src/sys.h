// Small system helpers used everywhere: time, NVS settings, heap figures, deferred reboot.
#pragma once

#include <nvs.h>
#include <stddef.h>
#include <stdint.h>

// ---- time ----------------------------------------------------------------------------
uint64_t now_us();   // µs since boot (esp_timer, monotonic)
uint32_t now_ms();   // ms since boot (wraps after 49 days - compare with (int32_t)(a - b))

// ---- NVS settings ------------------------------------------------------------------------
// One namespace, opened for the lifetime of the object; changes are committed in the destructor.
// Types match what the former Arduino "Preferences" library stored (bool = u8, uint = u32, int = i32,
// string = str, bytes = blob), so settings saved by older firmware read back unchanged.
class Nvs {
public:
    Nvs(const char* ns, bool writable);
    ~Nvs();
    Nvs(const Nvs&) = delete;
    Nvs& operator=(const Nvs&) = delete;

    bool ok() const { return open_; }
    bool has(const char* key);

    uint8_t get_u8(const char* key, uint8_t def);
    bool get_bool(const char* key, bool def) { return get_u8(key, def ? 1 : 0) != 0; }
    uint32_t get_u32(const char* key, uint32_t def);
    int32_t get_i32(const char* key, int32_t def);
    // copies the string (truncated to cap-1) into out; def if missing. Returns the length.
    size_t get_str(const char* key, char* out, size_t cap, const char* def = "");
    size_t blob_size(const char* key);   // 0 if missing
    bool get_blob(const char* key, void* out, size_t len);

    bool set_u8(const char* key, uint8_t v);
    bool set_bool(const char* key, bool v) { return set_u8(key, v ? 1 : 0); }
    bool set_u32(const char* key, uint32_t v);
    bool set_i32(const char* key, int32_t v);
    bool set_str(const char* key, const char* v);
    bool set_blob(const char* key, const void* data, size_t len);
    bool erase(const char* key);   // true also if it did not exist

private:
    nvs_handle_t h_ = 0;
    bool open_ = false;
    bool dirty_ = false;
};

// ---- memory ----------------------------------------------------------------------------
uint32_t heap_free();       // free heap now
uint32_t heap_min();        // lowest free heap since boot
uint32_t heap_largest();    // largest block that can be allocated

// ---- reboot ----------------------------------------------------------------------------
// Reboots from the main task after delay_ms, so replies already written still get out.
void sys_request_reboot(uint32_t delay_ms);
bool sys_reboot_due();      // main task: true when it is time to restart

// ---- OTA rollback ------------------------------------------------------------------------
// A firmware installed by OTA starts "pending verify": if the device resets before it is confirmed, the
// bootloader goes back to the previous firmware. The main task confirms it after APP_VALID_AFTER_S;
// a deliberate #reboot or a new upload confirms it too (the user evidently works with it).
bool sys_app_pending_verify();
bool sys_app_confirm();     // true if it was pending and is confirmed now
