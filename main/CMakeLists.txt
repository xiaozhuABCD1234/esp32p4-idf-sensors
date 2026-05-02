idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_hosted esp_wifi_remote nvs_flash esp_event esp_netif esp_wifi)
