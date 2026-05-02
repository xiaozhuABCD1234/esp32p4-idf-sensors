#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "SHT30";

#define SHT30_UART      UART_NUM_1
#define TXD_PIN         4
#define RXD_PIN         5
#define BUF_SIZE        1024
#define RD_BUF_SIZE     256

static QueueHandle_t uart_queue;

static void uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(SHT30_UART, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(SHT30_UART, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(SHT30_UART, BUF_SIZE, BUF_SIZE, 10, &uart_queue, 0));
}

static void parse_sht30_data(const char *str)
{
    float humidity = 0, temperature = 0;
    if (sscanf(str, "R:%fRH %fC", &humidity, &temperature) == 2) {
        ESP_LOGI(TAG, "Humidity: %.1f%%RH, Temperature: %.1fC", humidity, temperature);
    } else {
        ESP_LOGW(TAG, "Parse failed: %s", str);
    }
}

void app_main(void)
{
    uart_init();

    uart_write_bytes(SHT30_UART, "Auto\r\n", 6);

    uint8_t data[RD_BUF_SIZE];
    uart_event_t event;
    while (1) {
        if (xQueueReceive(uart_queue, &event, portMAX_DELAY)) {
            switch (event.type) {
            case UART_DATA:
                uart_read_bytes(SHT30_UART, data, event.size, pdMS_TO_TICKS(100));
                data[event.size] = '\0';
                if (strncmp((char *)data, "R:", 2) == 0) {
                    parse_sht30_data((char *)data);
                }
                break;
            case UART_FIFO_OVF:
            case UART_BUFFER_FULL:
                uart_flush_input(SHT30_UART);
                break;
            default:
                break;
            }
        }
    }
}
