/*
 * INMP441 I2S Recorder with Auto-Calibration VAD and WiFi Upload
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

static const char *TAG = "RECORDER";

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

/* VAD & Calibration Configuration */
#define VAD_FRAME_SIZE      512  // Samples per frame (32ms)
#define CALIBRATION_SEC     10   // Warm-up time in seconds (User requested ~9s)
#define VAD_MARGIN          500  // Threshold = NoiseFloor + Margin
#define RECORD_TIME_SEC     CONFIG_RECORD_SECONDS

/* Globals */
static bool wifi_connected = false;
static int vad_threshold = 1000; // Default, will be updated by calibration

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
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi Init done. Connecting to %s...", WIFI_SSID);
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

/* HTTP Upload Task */
static void upload_audio_to_server(const uint8_t *data, size_t len)
{
    if (!wifi_connected) {
        ESP_LOGE(TAG, "Cannot upload: WiFi not connected!");
        return;
    }

    ESP_LOGI(TAG, "Uploading %d bytes to %s...", len, SERVER_URL);

    esp_http_client_config_t config = {
        .url = SERVER_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    // Set headers
    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    
    // Set post data
    esp_http_client_set_post_field(client, (const char *)data, len);

    // Perform request
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Upload Status = %d, content_length = %lld",
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
        // TODO: Read response (JSON command from server) here if needed
    } else {
        ESP_LOGE(TAG, "Upload failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

/* RMS Calc */
static float calculate_rms(int16_t *data, int samples)
{
    float sum = 0.0f;
    for (int i = 0; i < samples; i++) {
        float val = (float)data[i];
        sum += val * val;
    }
    return sqrtf(sum / samples);
}

/* Main VAD Task */
void vad_task(void *arg)
{
    size_t bytes_read;
    int32_t *i2s_buff = malloc(VAD_FRAME_SIZE * 4 * sizeof(int32_t)); 
    int16_t *vad_buff = malloc(VAD_FRAME_SIZE * sizeof(int16_t));   
    
    // Allocate RAM for recording
    size_t pcm_size = SAMPLE_RATE * 2 * RECORD_TIME_SEC;
    size_t header_size = sizeof(wav_header_t);
    size_t total_size = header_size + pcm_size;
    
    uint8_t *rec_buf = malloc(total_size);
    if (!rec_buf || !i2s_buff || !vad_buff) {
        ESP_LOGE(TAG, "Failed to allocate memory!");
        vTaskDelete(NULL);
    }
    
    int16_t *pcm_start = (int16_t *)(rec_buf + header_size);

    // --- PHASE 1: CALIBRATION ---
    ESP_LOGI(TAG, "Starting Calibration (%d seconds)... Please keep silent.", CALIBRATION_SEC);
    
    int calib_frames = (CALIBRATION_SEC * SAMPLE_RATE) / VAD_FRAME_SIZE;
    float sum_rms = 0.0f;
    int valid_frames = 0;

    for (int i = 0; i < calib_frames; i++) {
        i2s_read(I2S_PORT, i2s_buff, VAD_FRAME_SIZE * 4, &bytes_read, portMAX_DELAY);
        int samples = bytes_read / 4;
        
        for (int j = 0; j < samples; j++) {
            int32_t s = i2s_buff[j] >> 11;
            vad_buff[j] = (int16_t)s;
        }
        
        float rms = calculate_rms(vad_buff, samples);
        sum_rms += rms;
        valid_frames++;
        
        if (i % 50 == 0) {
            ESP_LOGI(TAG, "Calibrating... Current RMS: %.1f", rms);
        }
    }

    float noise_floor = sum_rms / valid_frames;
    vad_threshold = (int)(noise_floor + VAD_MARGIN);
    
    ESP_LOGI(TAG, ">>> Calibration Done. Noise Floor: %.1f, Threshold set to: %d", noise_floor, vad_threshold);
    
    // Optional: Wait for WiFi if not connected yet
    while (!wifi_connected) {
        ESP_LOGW(TAG, "Waiting for WiFi...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // --- PHASE 2: LISTENING ---
    ESP_LOGI(TAG, "VAD Listening...");

    while (1) {
        i2s_read(I2S_PORT, i2s_buff, VAD_FRAME_SIZE * 4, &bytes_read, portMAX_DELAY);
        int samples = bytes_read / 4;
        
        for (int i = 0; i < samples; i++) {
            int32_t s = i2s_buff[i] >> 11;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            vad_buff[i] = (int16_t)s;
        }
        
        float rms = calculate_rms(vad_buff, samples);
        
        if (rms > vad_threshold) {
            ESP_LOGI(TAG, ">>> Triggered! (RMS: %.1f) Recording...", rms);
            
            // Start Recording
            int16_t *pcm_ptr = pcm_start;
            size_t recorded_samples = 0;
            size_t target_samples = pcm_size / 2;
            
            // Pre-fill
            memcpy(pcm_ptr, vad_buff, samples * 2);
            pcm_ptr += samples;
            recorded_samples += samples;
            
            // Record Loop
            while (recorded_samples < target_samples) {
                 i2s_read(I2S_PORT, i2s_buff, VAD_FRAME_SIZE * 4, &bytes_read, portMAX_DELAY);
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
            
            // Prepare Header & Upload
            wav_header_t header = create_wav_header(pcm_size);
            memcpy(rec_buf, &header, header_size);
            
            ESP_LOGI(TAG, "Recording Done. Uploading...");
            upload_audio_to_server(rec_buf, total_size);
            
            // Cooldown
            i2s_zero_dma_buffer(I2S_PORT);
            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP_LOGI(TAG, "Resuming VAD...");
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    esp_task_wdt_deinit();
    
    // Init WiFi first so it connects while calibrating
    init_wifi();
    
    init_i2s();
    
    // Start VAD Task
    xTaskCreate(vad_task, "vad", 4096, NULL, 5, NULL);
}
