#include <cstdio>
#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "echo";

extern "C" void app_main(void)
{
    const uart_port_t uart_num = UART_NUM_0;
    const int buf_size = 1024;

    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {0, 0},
    };
    uart_param_config(uart_num, &uart_config);
    uart_driver_install(uart_num, buf_size * 2, 0, 0, nullptr, 0);

    ESP_LOGI(TAG, "UART echo started on UART0 @ 115200");

    uint8_t data[buf_size];
    while (true) {
        int len = uart_read_bytes(uart_num, data, sizeof(data), pdMS_TO_TICKS(100));
        if (len > 0) {
            uart_write_bytes(uart_num, data, len);
        }
    }
}
