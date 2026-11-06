// Status LED: short flash per received frame, solid in ACTIVE, fast blinking on bus-off.
#pragma once

void led_init();
void led_notify_rx();                       // any task, cheap
void led_set_mode(bool active, bool bus_off);
void led_update();                          // call from loop()
