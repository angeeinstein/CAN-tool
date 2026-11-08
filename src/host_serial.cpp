#include "host_serial.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "commands.h"
#include "config.h"
#include "host_io.h"

#if CONFIG_IDF_TARGET_ESP32
#include <driver/uart.h>
#else
#include <driver/usb_serial_jtag.h>
#endif

namespace {

#if CONFIG_IDF_TARGET_ESP32
constexpr uart_port_t PORT = UART_NUM_0;

void driver_init() {
    uart_config_t c = {};
    c.baud_rate = HOST_SERIAL_BAUD;
    c.data_bits = UART_DATA_8_BITS;
    c.parity = UART_PARITY_DISABLE;
    c.stop_bits = UART_STOP_BITS_1;
    c.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    c.source_clk = UART_SCLK_DEFAULT;
    uart_driver_install(PORT, 1024, 8192, 0, nullptr, 0);
    uart_param_config(PORT, &c);
    uart_set_pin(PORT, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

int read_some(uint8_t* buf, size_t cap) { return uart_read_bytes(PORT, buf, cap, pdMS_TO_TICKS(20)); }
#else
void driver_init() {
    usb_serial_jtag_driver_config_t c = {};
    c.tx_buffer_size = 4096;
    c.rx_buffer_size = 1024;
    usb_serial_jtag_driver_install(&c);
}

int read_some(uint8_t* buf, size_t cap) { return usb_serial_jtag_read_bytes(buf, cap, pdMS_TO_TICKS(20)); }
#endif

void reader_task(void*) {
    LineBuffer line;
    uint8_t buf[128];
    for (;;) {
        const int n = read_some(buf, sizeof(buf));
        for (int i = 0; i < n; i++)
            if (line.feed(static_cast<char>(buf[i]))) cmd_post(SINK_SERIAL, 0, line.line(), portMAX_DELAY);
    }
}

}  // namespace

void host_serial_init() {
    driver_init();
    xTaskCreatePinnedToCore(reader_task, "serial_rx", 3072, nullptr, 4, nullptr, 0);
}

void host_serial_write(const char* buf, size_t len) {
    if (len == 0) return;
#if CONFIG_IDF_TARGET_ESP32
    uart_write_bytes(PORT, buf, len);
#else
    usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(2));
#endif
}
