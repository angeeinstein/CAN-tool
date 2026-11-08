// USB host link: UART0 behind the USB bridge (classic ESP32, HOST_SERIAL_BAUD) or the chip's own
// USB-Serial/JTAG port (S3/C3). A reader task posts complete lines to the command queue (SINK_SERIAL).
#pragma once

#include <stddef.h>

void host_serial_init();
// Writes all bytes (UART: waits for space in the 8 KB TX buffer). On USB-Serial/JTAG without a host
// reading, data is dropped after a few ms so the output task never stalls.
void host_serial_write(const char* buf, size_t len);
