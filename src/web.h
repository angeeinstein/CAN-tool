// Web interface (port 80, ESP-IDF http server): single page UI served from flash, WebSocket "/ws"
// speaking the same "#..." protocol as USB, and firmware upload (OTA, POST /update).
// All protected by WEB_USER/WEB_PASSWORD (HTTP basic auth; the WebSocket with a per-boot token).
//
// Sending to the browsers is done by one task ("web_tx"), with fixed buffers:
//   - live data (frames, notifications) is collected in a buffer and sent to all browsers every
//     WEB_WS_FLUSH_MS; if the buffer is full the rest is dropped and counted
//   - command replies go through a separate buffer, only to the browser that sent the command
//   - a browser that does not accept data within WEB_SEND_TIMEOUT_S is disconnected (it reconnects and
//     fetches the missed frames with #replay). Nothing is ever queued per browser on the heap.
#pragma once

#include <stddef.h>
#include <stdint.h>

void web_init();   // after net_init()

void web_write(const char* buf, size_t len);                    // live data, output task, never blocks
void web_reply(uint32_t client, const char* buf, size_t len);   // main task, may wait for that browser
bool web_has_clients();                                          // any authenticated browser
bool web_client_connected(uint32_t id);                          // ACTIVE watchdog

struct WebStats {
    uint32_t dropped_bytes;     // live data not delivered (buffer full / browser too slow), since boot
    uint32_t dropped_batches;   // live batches lost because a browser was disconnected while sending
    uint32_t slow_closes;       // browsers disconnected because they did not take data in time
    int clients;                // authenticated browsers
    uint32_t send_ms_peak;      // longest single send since the last web_reset_peaks()
};
void web_get_stats(WebStats& s);
void web_reset_peaks();
