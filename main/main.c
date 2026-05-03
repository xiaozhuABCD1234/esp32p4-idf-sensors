#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_hosted.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "mqtt_client.h"

static const char *TAG = "SHT30";
static const char *TAG_WIFI = "WiFi";
static const char *TAG_MQTT = "MQTT";

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

static float s_humidity, s_temperature;
static SemaphoreHandle_t s_data_mutex;

static void parse_sht30_data(const char *str)
{
    float humidity = 0, temperature = 0;
    if (sscanf(str, "R:%fRH %fC", &humidity, &temperature) == 2) {
        ESP_LOGI(TAG, "Humidity: %.1f%%RH, Temperature: %.1fC", humidity, temperature);
        xSemaphoreTake(s_data_mutex, portMAX_DELAY);
        s_humidity = humidity;
        s_temperature = temperature;
        xSemaphoreGive(s_data_mutex);
    } else {
        ESP_LOGW(TAG, "Parse failed: %s", str);
    }
}

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG_WIFI, "Disconnected, retrying...");
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG_WIFI, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                        ESP_EVENT_ANY_ID,
                        &wifi_event_handler,
                        NULL,
                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                        IP_EVENT_STA_GOT_IP,
                        &wifi_event_handler,
                        NULL,
                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG_WIFI, "Connecting to AP...");
}

static EventGroupHandle_t mqtt_event_group;
#define MQTT_CONNECTED_BIT BIT0

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG_MQTT, "Connected to broker");
        xEventGroupSetBits(mqtt_event_group, MQTT_CONNECTED_BIT);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG_MQTT, "Disconnected from broker");
        xEventGroupClearBits(mqtt_event_group, MQTT_CONNECTED_BIT);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG_MQTT, "Publish acknowledged, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG_MQTT, "MQTT error");
        break;
    default:
        break;
    }
}

static void mqtt_publish_task(void *pvParameters)
{
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);

    mqtt_event_group = xEventGroupCreate();

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_MQTT_BROKER_URI,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    xEventGroupWaitBits(mqtt_event_group, MQTT_CONNECTED_BIT, false, true, portMAX_DELAY);

    char topic[64];
    const char *base_topic = CONFIG_MQTT_TOPIC;
    int interval = CONFIG_MQTT_PUBLISH_INTERVAL * 1000;

    while (1) {
        xSemaphoreTake(s_data_mutex, portMAX_DELAY);
        float h = s_humidity;
        float t = s_temperature;
        xSemaphoreGive(s_data_mutex);

        char json[64];
        snprintf(json, sizeof(json), "{\"humidity\":%.1f,\"temperature\":%.1f}", h, t);
        int msg_id = esp_mqtt_client_publish(client, base_topic, json, 0, 1, 0);
        ESP_LOGI(TAG_MQTT, "Published to %s: %s (msg_id=%d)", base_topic, json, msg_id);

        vTaskDelay(pdMS_TO_TICKS(interval));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_hosted_init());

    esp_netif_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_sta();

    s_data_mutex = xSemaphoreCreateMutex();

    uart_init();
    uart_write_bytes(SHT30_UART, "Auto\r\n", 6);

    xTaskCreate(mqtt_publish_task, "mqtt_pub", 8192, NULL, 5, NULL);

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
