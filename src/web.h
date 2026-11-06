// Web interface (port 80): single page UI served from flash, WebSocket "/ws" speaking the same
// "#..." protocol as USB, WLAN settings and firmware upload (OTA). All protected by WEB_USER/WEB_PASSWORD.
// Compiled to no-ops when ENABLE_WIFI=0.
#pragma once

#include <stddef.h>

void web_init();
void web_loop();                              // loop task: run queued browser commands, flush live data
void web_write(const char* buf, size_t len);  // any task: queue text for all authenticated browsers
void web_reply(const char* buf, size_t len);  // loop task: command reply, only to the browser that sent it
bool web_has_clients();
