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

static const char* TAG = "SHT30";
static const char* TAG_CJ702 = "CJ702";
static const char* TAG_WIFI = "WiFi";
static const char* TAG_MQTT = "MQTT";

// SHT30 (UART1)
#define SHT30_UART      UART_NUM_1
#define TXD_PIN         4
#define RXD_PIN         5
#define BUF_SIZE        1024
#define RD_BUF_SIZE     256

static QueueHandle_t sht30_uart_queue;

static float s_humidity, s_temperature;
static SemaphoreHandle_t s_data_mutex;

// CJ702 (UART2)
#define CJ702_UART          UART_NUM_2
#define CJ702_RXD_PIN       3
#define CJ702_BUF_SIZE      1024
#define CJ702_RD_BUF_SIZE   256
#define CJ702_FRAME_SIZE    17

typedef struct {
    uint16_t eco2;
    uint16_t ech2o;
    uint16_t tvoc;
    uint16_t pm25;
    uint16_t pm10;
    float temperature;
    float humidity;
} cj702_data_t;

static cj702_data_t s_cj702_data;
static SemaphoreHandle_t s_cj702_data_mutex;
static QueueHandle_t cj702_uart_queue;

// ---- MQTT globals (defined early so read tasks can see them) ----

static esp_mqtt_client_handle_t mqtt_client;
static EventGroupHandle_t mqtt_event_group;
#define MQTT_CONNECTED_BIT BIT0

// ---- SHT30 ----

static void uart_init(void) {
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(SHT30_UART, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(SHT30_UART, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(SHT30_UART, BUF_SIZE, BUF_SIZE, 10, &sht30_uart_queue, 0));
}

static void parse_sht30_data(const char* str) {
    float humidity = 0, temperature = 0;
    if (sscanf(str, "R:%fRH %fC", &humidity, &temperature) == 2) {
        ESP_LOGI(TAG, "Humidity: %.1f%%RH, Temperature: %.1fC", humidity, temperature);
        xSemaphoreTake(s_data_mutex, portMAX_DELAY);
        s_humidity = humidity;
        s_temperature = temperature;
        xSemaphoreGive(s_data_mutex);
    }
    else {
        ESP_LOGW(TAG, "Parse failed: %s", str);
    }
}

static void sht30_read_task(void* pvParameters) {
    uart_write_bytes(SHT30_UART, "Auto\r\n", 6);

    uint8_t data[RD_BUF_SIZE];
    uart_event_t event;
    while (1) {
        if (xQueueReceive(sht30_uart_queue, &event, portMAX_DELAY)) {
            switch (event.type) {
            case UART_DATA:
                uart_read_bytes(SHT30_UART, data, event.size, pdMS_TO_TICKS(100));
                data[event.size] = '\0';
                if (strncmp((char*)data, "R:", 2) == 0) {
                    parse_sht30_data((char*)data);
#if CONFIG_SHT30_PUBLISH_IMMEDIATE
                    if (mqtt_client && mqtt_event_group && (xEventGroupGetBits(mqtt_event_group) & MQTT_CONNECTED_BIT)) {
                        char json[64];
                        xSemaphoreTake(s_data_mutex, portMAX_DELAY);
                        float h = s_humidity;
                        float t = s_temperature;
                        xSemaphoreGive(s_data_mutex);
                        snprintf(json, sizeof(json), "{\"humidity\":%.1f,\"temperature\":%.1f}", h, t);
                        esp_mqtt_client_publish(mqtt_client, CONFIG_MQTT_TOPIC, json, 0, 1, 0);
                        ESP_LOGI(TAG_MQTT, "Published to %s: %s", CONFIG_MQTT_TOPIC, json);
                    }
#endif
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

// ---- CJ702 ----

static void uart_init_cj702(void) {
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(CJ702_UART, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(CJ702_UART, UART_PIN_NO_CHANGE, CJ702_RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(CJ702_UART, CJ702_BUF_SIZE, CJ702_BUF_SIZE, 10, &cj702_uart_queue, 0));
}

static bool parse_cj702_frame(const uint8_t* buf, int len) {
    if (len < CJ702_FRAME_SIZE || buf[0] != 0x3C || buf[1] != 0x02) {
        return false;
    }

    uint8_t checksum = 0;
    for (int i = 0; i < CJ702_FRAME_SIZE - 1; i++) {
        checksum += buf[i];
    }
    if (checksum != buf[16]) {
        ESP_LOGW(TAG_CJ702, "Checksum mismatch: calc=0x%02X, recv=0x%02X", checksum, buf[16]);
        return false;
    }

    cj702_data_t d;
    d.eco2 = (buf[2] << 8) | buf[3];
    d.ech2o = (buf[4] << 8) | buf[5];
    d.tvoc = (buf[6] << 8) | buf[7];
    d.pm25 = (buf[8] << 8) | buf[9];
    d.pm10 = (buf[10] << 8) | buf[11];

    uint8_t temp_int = buf[12] & 0x7F;
    float temp_dec = buf[13] / 10.0f;
    d.temperature = (buf[12] & 0x80) ? -(temp_int + temp_dec) : (temp_int + temp_dec);

    d.humidity = buf[14] + buf[15] / 10.0f;

    ESP_LOGI(TAG_CJ702, "eCO2=%dppb eCH2O=%dppb TVOC=%dppb PM2.5=%dug/m3 PM10=%dug/m3 Temp=%.1fC Hum=%.1f%%",
        d.eco2, d.ech2o, d.tvoc, d.pm25, d.pm10, d.temperature, d.humidity);

    xSemaphoreTake(s_cj702_data_mutex, portMAX_DELAY);
    s_cj702_data = d;
    xSemaphoreGive(s_cj702_data_mutex);

    return true;
}

static void cj702_read_task(void* pvParameters) {
    uint8_t data[CJ702_RD_BUF_SIZE];
    uart_event_t event;
    while (1) {
        if (xQueueReceive(cj702_uart_queue, &event, portMAX_DELAY)) {
            switch (event.type) {
            case UART_DATA:
                uart_read_bytes(CJ702_UART, data, event.size, pdMS_TO_TICKS(100));
                if (event.size >= CJ702_FRAME_SIZE) {
                    parse_cj702_frame(data, event.size);
#if CONFIG_CJ702_PUBLISH_IMMEDIATE
                    if (mqtt_client && mqtt_event_group && (xEventGroupGetBits(mqtt_event_group) & MQTT_CONNECTED_BIT)) {
                        char json[128];
                        xSemaphoreTake(s_cj702_data_mutex, portMAX_DELAY);
                        cj702_data_t d = s_cj702_data;
                        xSemaphoreGive(s_cj702_data_mutex);
                        snprintf(json, sizeof(json),
                            "{\"eco2\":%d,\"ech2o\":%d,\"tvoc\":%d,\"pm25\":%d,\"pm10\":%d,\"temperature\":%.1f,\"humidity\":%.1f}",
                            d.eco2, d.ech2o, d.tvoc, d.pm25, d.pm10, d.temperature, d.humidity);
                        esp_mqtt_client_publish(mqtt_client, CONFIG_CJ702_TOPIC, json, 0, 1, 0);
                        ESP_LOGI(TAG_MQTT, "Published to %s: %s", CONFIG_CJ702_TOPIC, json);
                    }
#endif
                }
                break;
            case UART_FIFO_OVF:
            case UART_BUFFER_FULL:
                uart_flush_input(CJ702_UART);
                break;
            default:
                break;
            }
        }
    }
}

// ---- WiFi ----

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG_WIFI, "Disconnected, retrying...");
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG_WIFI, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void) {
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

// ---- MQTT ----

static void mqtt_event_handler(void* handler_args, esp_event_base_t base,
    int32_t event_id, void* event_data) {
    esp_mqtt_event_handle_t event = event_data;

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

static void mqtt_publish_task(void* pvParameters) {
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);

    mqtt_event_group = xEventGroupCreate();

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_MQTT_BROKER_URI,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    xEventGroupWaitBits(mqtt_event_group, MQTT_CONNECTED_BIT, false, true, portMAX_DELAY);

    int interval = CONFIG_MQTT_PUBLISH_INTERVAL * 1000;

    while (1) {
#if !CONFIG_SHT30_PUBLISH_IMMEDIATE
        const char* sht30_topic = CONFIG_MQTT_TOPIC;
        xSemaphoreTake(s_data_mutex, portMAX_DELAY);
        float h = s_humidity;
        float t = s_temperature;
        xSemaphoreGive(s_data_mutex);

        char sht30_json[64];
        snprintf(sht30_json, sizeof(sht30_json), "{\"humidity\":%.1f,\"temperature\":%.1f}", h, t);
        int sht30_msg_id = esp_mqtt_client_publish(mqtt_client, sht30_topic, sht30_json, 0, 1, 0);
        ESP_LOGI(TAG_MQTT, "Published to %s: %s (msg_id=%d)", sht30_topic, sht30_json, sht30_msg_id);
#endif

#if !CONFIG_CJ702_PUBLISH_IMMEDIATE
        const char* cj702_topic = CONFIG_CJ702_TOPIC;
        xSemaphoreTake(s_cj702_data_mutex, portMAX_DELAY);
        cj702_data_t d = s_cj702_data;
        xSemaphoreGive(s_cj702_data_mutex);

        char cj702_json[128];
        snprintf(cj702_json, sizeof(cj702_json),
            "{\"eco2\":%d,\"ech2o\":%d,\"tvoc\":%d,\"pm25\":%d,\"pm10\":%d,\"temperature\":%.1f,\"humidity\":%.1f}",
            d.eco2, d.ech2o, d.tvoc, d.pm25, d.pm10, d.temperature, d.humidity);
        int cj702_msg_id = esp_mqtt_client_publish(mqtt_client, cj702_topic, cj702_json, 0, 1, 0);
        ESP_LOGI(TAG_MQTT, "Published to %s: %s (msg_id=%d)", cj702_topic, cj702_json, cj702_msg_id);
#endif

        vTaskDelay(pdMS_TO_TICKS(interval));
    }
}

// ---- app_main ----

void app_main(void) {
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
    s_cj702_data_mutex = xSemaphoreCreateMutex();

    uart_init();
    uart_init_cj702();

    xTaskCreate(sht30_read_task, "sht30_read", 4096, NULL, 5, NULL);
    xTaskCreate(cj702_read_task, "cj702_read", 4096, NULL, 5, NULL);
    xTaskCreate(mqtt_publish_task, "mqtt_pub", 8192, NULL, 5, NULL);
}
