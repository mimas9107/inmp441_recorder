/*
 * INMP441 I2S Recorder - Continuous Collection Mode (feature01a)
 * Used for dataset collection (sample1.wav, sample2.wav...)
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

static const char *TAG = "COLLECTOR";

/* Config from Kconfig */
#define WIFI_SSID       CONFIG_WIFI_SSID
#define WIFI_PASS       CONFIG_WIFI_PASSWORD
#define SERVER_URL      CONFIG_SERVER_URL

/* Pins */
#define I2S_WS_GPIO     CONFIG_I2S_WS_GPIO
#define I2S_DIN_GPIO    CONFIG_I2S_DIN_GPIO
#define I2S_BCK_GPIO    CONFIG_I2S_BCK_GPIO

/* Audio Constants */
#define SAMPLE_RATE     16000
#define I2S_PORT        I2S_NUM_0
#define DMA_BUF_COUNT   4
#define DMA_BUF_LEN     256

/* Collection Configuration */
#define RECORD_TIME_SEC     CONFIG_RECORD_SECONDS
#define PAUSE_BETWEEN_SEC   1  // Seconds to wait between samples

/* Globals */
static bool wifi_connected = false;

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
        ESP_LOGW(TAG, "WiFi Disconnected. Retrying...");
        wifi_connected = false;
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "WiFi Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
    }
}

/* WiFi Init */
static void init_wifi(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
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
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* I2S Init */
static void init_i2s(void)
{
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
        .bck_io_num = I2S_BCK_GPIO,
        .ws_io_num = I2S_WS_GPIO,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_DIN_GPIO,
        .mck_io_num = I2S_PIN_NO_CHANGE,
    };

    ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT, &cfg, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT, &pin_cfg));
    ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_PORT));
}

/* HTTP Upload */
static void upload_audio_to_server(const uint8_t *data, size_t len)
{
    if (!wifi_connected) {
        ESP_LOGE(TAG, "Cannot upload: WiFi not connected!");
        return;
    }

    esp_http_client_config_t config = {
        .url = SERVER_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_post_field(client, (const char *)data, len);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Upload Successful. Status = %d", esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "Upload failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

/* Main Collection Task */
void collection_task(void *arg)
{
    size_t bytes_read;
    int32_t *i2s_buff = malloc(DMA_BUF_LEN * 4 * sizeof(int32_t)); 
    
    size_t pcm_size = SAMPLE_RATE * 2 * RECORD_TIME_SEC;
    size_t header_size = sizeof(wav_header_t);
    size_t total_size = header_size + pcm_size;
    
    uint8_t *rec_buf = malloc(total_size);
    if (!rec_buf || !i2s_buff) {
        ESP_LOGE(TAG, "Failed to allocate memory!");
        vTaskDelete(NULL);
    }
    
    int16_t *pcm_start = (int16_t *)(rec_buf + header_size);
    int sample_index = 1;

    while (!wifi_connected) {
        ESP_LOGW(TAG, "Waiting for WiFi...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, ">>> COLLECTION STARTED. RECORD_TIME: %d sec", RECORD_TIME_SEC);

    while (1) {
        ESP_LOGI(TAG, ">>> Capturing Sample #%d...", sample_index);
        
        int16_t *pcm_ptr = pcm_start;
        size_t recorded_samples = 0;
        size_t target_samples = pcm_size / 2;
        
        while (recorded_samples < target_samples) {
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
        
        wav_header_t header = create_wav_header(pcm_size);
        memcpy(rec_buf, &header, header_size);
        
        ESP_LOGI(TAG, "Sample #%d captured. Uploading...", sample_index);
        upload_audio_to_server(rec_buf, total_size);
        
        ESP_LOGI(TAG, "Done. Next in %d sec...", PAUSE_BETWEEN_SEC);
        sample_index++;
        
        vTaskDelay(pdMS_TO_TICKS(PAUSE_BETWEEN_SEC * 1000));
    }
}

void app_main(void)
{
    esp_task_wdt_deinit();
    init_wifi();
    init_i2s();
    xTaskCreate(collection_task, "collect", 8192, NULL, 5, NULL);
}
