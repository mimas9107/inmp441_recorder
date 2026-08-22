idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_event nvs_flash esp_http_client esp_http_server esp_driver_i2s esp_driver_gpio)
