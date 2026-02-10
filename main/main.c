/*
 * INMP441 I2S Recorder - Managed Collection Mode (feature01a)
 * Controlled by Remote Server (Start/Stop) + Connection Watchdog
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_task_wdt.h"
#include "sdkconfig.h"

// WiFi & HTTP Includes
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"

static const char *TAG = "COLLECTOR";

/* Config from Kconfig */
#define WIFI_SSID       CONFIG_WIFI_SSID
#define WIFI_PASS       CONFIG_WIFI_PASSWORD
#define SERVER_URL      CONFIG_SERVER_URL

/* Audio Constants */
#define SAMPLE_RATE     16000
#define I2S_PORT        I2S_NUM_0
#define DMA_BUF_COUNT   4
#define DMA_BUF_LEN     256
#define RECORD_TIME_SEC CONFIG_RECORD_SECONDS

/* Globals */
static bool wifi_connected = false;
static bool server_alive = false;
static bool is_collecting = false;
static char esp_ip[16] = "0.0.0.0";

/* WAV header */
typedef struct __attribute__((packed)) {
    char riff_tag[4];
    uint32_t riff_size;
    char wave_tag[4];
    char fmt_tag[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data_tag[4];
    uint32_t data_size;
} wav_header_t;

static wav_header_t create_wav_header(uint32_t data_size)
{
    wav_header_t h = {
        .riff_tag = {'R','I','F','F'},
        .riff_size = data_size + 36,
        .wave_tag = {'W','A','V','E'},
        .fmt_tag  = {'f','m','t',' '},
        .fmt_size = 16,
        .audio_format = 1,
        .num_channels = 1,
        .sample_rate = SAMPLE_RATE,
        .byte_rate = SAMPLE_RATE * 2,
        .block_align = 2,
        .bits_per_sample = 16,
        .data_tag = {'d','a','t','a'},
        .data_size = data_size
    };
    return h;
}

/* WiFi Event Handler */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        esp_ip4addr_ntoa(&event->ip_info.ip, esp_ip, 16);
        ESP_LOGI(TAG, "WiFi Connected! IP: %s", esp_ip);
        wifi_connected = true;
    }
}

/* ESP32 HTTP Server for Controls */
static esp_err_t control_get_handler(httpd_req_t *req)
{
    char buf[128];
    size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len > 1) {
        if (httpd_req_get_url_query_str(req, buf, query_len) == ESP_OK) {
            char val[32];
            if (httpd_query_key_value(buf, "cmd", val, sizeof(val)) == ESP_OK) {
                if (strcmp(val, "start") == 0) {
                    if (server_alive) {
                        is_collecting = true;
                        ESP_LOGI(TAG, "Remote START");
                    } else {
                        ESP_LOGW(TAG, "Cannot start: Server not detected");
                    }
                } else if (strcmp(val, "stop") == 0) {
                    is_collecting = false;
                    ESP_LOGI(TAG, "Remote STOP");
                }
            }
        }
    }
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t control_uri = { .uri = "/control", .method = HTTP_GET, .handler = control_get_handler };
        httpd_register_uri_handler(server, &control_uri);
    }
}

/* Connection Watchdog Task */
void watchdog_task(void *arg)
{
    char ping_url[128];
    strncpy(ping_url, SERVER_URL, sizeof(ping_url));
    char *p = strstr(ping_url, "://");
    if (p) {
        p += 3;
        p = strchr(p, '/');
        if (p) *p = '\0';
    }
    // We just GET root to check if server is there
    
    while (1) {
        if (wifi_connected) {
            esp_http_client_config_t config = {
                .url = ping_url,
                .method = HTTP_METHOD_GET,
                .timeout_ms = 3000,
            };
            esp_http_client_handle_t client = esp_http_client_init(&config);
            esp_err_t err = esp_http_client_perform(client);
            
            if (err == ESP_OK) {
                if (!server_alive) ESP_LOGI(TAG, "Server connection recovered.");
                server_alive = true;
            } else {
                if (server_alive) ESP_LOGE(TAG, "Server connection lost!");
                server_alive = false;
                if (is_collecting) {
                    ESP_LOGW(TAG, "Stopping collection due to server disconnect.");
                    is_collecting = false;
                }
            }
            esp_http_client_cleanup(client);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void register_with_server(void)
{
    char reg_url[128];
    strncpy(reg_url, SERVER_URL, sizeof(reg_url));
    char *p = strstr(reg_url, "://");
    if (p) { p += 3; p = strchr(p, '/'); if (p) *p = '\0'; }
    strcat(reg_url, "/register");

    char post_data[64];
    snprintf(post_data, sizeof(post_data), "{\"ip\": \"%s\", \"id\": \"esp32_mic\"}", esp_ip);

    esp_http_client_config_t config = { .url = reg_url, .method = HTTP_METHOD_POST, .timeout_ms = 5000 };
    
    while (1) {
        if (wifi_connected) {
            char post_data[64];
            snprintf(post_data, sizeof(post_data), "{\"ip\": \"%s\", \"id\": \"esp32_mic\"}", esp_ip);

            esp_http_client_handle_t client = esp_http_client_init(&config);
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, post_data, strlen(post_data));
            esp_err_t err = esp_http_client_perform(client);
            if (err == ESP_OK && esp_http_client_get_status_code(client) == 200) {
                ESP_LOGI(TAG, "Registration successful!");
                esp_http_client_cleanup(client);
                break;
            }
            esp_http_client_cleanup(client);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void init_wifi(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    wifi_config_t wifi_config = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void init_i2s(void) {
    i2s_config_t cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = DMA_BUF_COUNT,
        .dma_buf_len = DMA_BUF_LEN,
        .use_apll = true,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0,
    };
    i2s_pin_config_t pin_cfg = {
        .bck_io_num = CONFIG_I2S_BCK_GPIO,
        .ws_io_num = CONFIG_I2S_WS_GPIO,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = CONFIG_I2S_DIN_GPIO,
        .mck_io_num = I2S_PIN_NO_CHANGE,
    };
    ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT, &cfg, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT, &pin_cfg));
}

static void upload_audio_to_server(const uint8_t *data, size_t len) {
    esp_http_client_config_t config = { .url = SERVER_URL, .method = HTTP_METHOD_POST, .timeout_ms = 10000 };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_post_field(client, (const char *)data, len);
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Upload failed, stopping...");
        is_collecting = false;
    }
    esp_http_client_cleanup(client);
}

void collection_task(void *arg)
{
    size_t bytes_read;
    int32_t *i2s_buff = malloc(DMA_BUF_LEN * 4 * sizeof(int32_t)); 
    size_t pcm_size = SAMPLE_RATE * 2 * RECORD_TIME_SEC;
    size_t header_size = sizeof(wav_header_t);
    size_t total_size = header_size + pcm_size;
    uint8_t *rec_buf = malloc(total_size);
    int16_t *pcm_start = (int16_t *)(rec_buf + header_size);

    register_with_server();

    while (1) {
        if (!is_collecting || !server_alive) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        ESP_LOGI(TAG, "Capturing...");
        int16_t *pcm_ptr = pcm_start;
        size_t recorded_samples = 0;
        size_t target_samples = pcm_size / 2;
        
        while (recorded_samples < target_samples && is_collecting && server_alive) {
             i2s_read(I2S_PORT, i2s_buff, DMA_BUF_LEN * 4, &bytes_read, portMAX_DELAY);
             int chunk_samples = bytes_read / 4;
             for (int i = 0; i < chunk_samples; i++) {
                int32_t s = i2s_buff[i] >> 11;
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                if (recorded_samples < target_samples) {
                    *pcm_ptr++ = (int16_t)s;
                    recorded_samples++;
                }
             }
        }
        
        if (is_collecting && server_alive) {
            wav_header_t header = create_wav_header(pcm_size);
            memcpy(rec_buf, &header, header_size);
            upload_audio_to_server(rec_buf, total_size);
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            ESP_LOGW(TAG, "Capture discarded.");
            i2s_zero_dma_buffer(I2S_PORT);
        }
    }
}

void app_main(void)
{
    esp_task_wdt_deinit();
    init_wifi();
    init_i2s();
    start_webserver();
    xTaskCreate(watchdog_task, "server_watch", 4096, NULL, 5, NULL);
    xTaskCreate(collection_task, "collect", 8192, NULL, 5, NULL);
}
