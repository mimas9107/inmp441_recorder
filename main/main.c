/*
 * INMP441 I2S Microphone Recorder with ESP-SR (WakeNet)
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_task_wdt.h"
#include "sdkconfig.h"

// ESP-SR Includes
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

static const char *TAG = "RECORDER";

/* Pins from menuconfig */
#define I2S_WS_GPIO   CONFIG_I2S_WS_GPIO
#define I2S_DIN_GPIO  CONFIG_I2S_DIN_GPIO
#define I2S_BCK_GPIO  CONFIG_I2S_BCK_GPIO

#define SAMPLE_RATE     16000 // ESP-SR requires 16kHz
#define I2S_PORT        I2S_NUM_0

// AFE Configuration
#define AFE_TASK_CORE   1

static esp_afe_sr_iface_t *afe_handle = NULL;
static esp_afe_sr_data_t *afe_data = NULL;
static srmodel_list_t *models = NULL; // Model list handle

/* I2S Init */
static void init_i2s(void)
{
    ESP_LOGI(TAG, "Init I2S");

    i2s_config_t cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // Mono
        .communication_format = I2S_COMM_FORMAT_STAND_I2S, // Philips Standard
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,   // Reduced from 8 to save RAM
        .dma_buf_len = 256,   // Reduced from 512 to save RAM
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

/* Feed Task: Read I2S -> Feed AFE */
void feed_task(void *arg)
{
    size_t bytes_read;
    // Use smaller stack/heap for buffers
    int32_t i2s_buff[256]; // 1KB on stack
    int16_t feed_buff[256]; // 512B on stack
    
    while (1) {
        // Read from I2S
        i2s_read(I2S_PORT, i2s_buff, 256 * 4, &bytes_read, portMAX_DELAY);
        
        int samples = bytes_read / 4;
        
        // Convert 32-bit (24-bit aligned) to 16-bit
        for (int i = 0; i < samples; i++) {
            // AFE expects clean 16-bit PCM.
            // >> 14 is standard for INMP441 (24->16 bit with ~12dB gain).
            int32_t s = i2s_buff[i] >> 14; 
            
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            feed_buff[i] = (int16_t)s;
        }

        // Feed to AFE
        afe_handle->feed(afe_data, feed_buff);
    }
    
    vTaskDelete(NULL);
}

/* Detect Task: Fetch AFE -> Handle Result */
void detect_task(void *arg)
{
    while (1) {
        afe_fetch_result_t* res = afe_handle->fetch(afe_data); 
        if (!res || res->ret_value == ESP_FAIL) {
            continue;
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, ">>> WAKE WORD DETECTED! <<<");
            // Here we will trigger recording later
        }
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_task_wdt_deinit();
    
    // 1. Init I2S
    init_i2s();

    // 2. Load Models
    models = esp_srmodel_init("model"); // "model" is the partition label
    if (models == NULL) {
        ESP_LOGE(TAG, "Failed to load models from partition 'model'");
        // Proceeding anyway might crash if wakenet is enabled
    }
    
    // 3. Init AFE (ESP-SR)
    afe_config_t afe_config = AFE_CONFIG_DEFAULT();
    
    // Disable AEC and SE to save RAM and because we only have 1 mic (no ref)
    afe_config.aec_init = false;
    afe_config.se_init = false;
    afe_config.vad_init = true;
    afe_config.wakenet_init = true;
    
    // Memory Config for WROOM (No PSRAM)
    afe_config.memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_INTERNAL;
    
    // Critical: Reduce Ring Buffer size to avoid "Memory exhausted"
    // Default is 50, which is too large for WROOM with WakeNet.
    afe_config.afe_ringbuf_size = 10; 
    
    // Auto-select wake word model
    char *wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    if (wn_name) {
        afe_config.wakenet_model_name = wn_name;
        ESP_LOGI(TAG, "Using WakeNet Model: %s", wn_name);
    } else {
        ESP_LOGW(TAG, "No WakeNet model found! VAD only mode.");
        afe_config.wakenet_init = false;
    }
    
    afe_config.voice_communication_init = false; 
    afe_config.voice_communication_agc_init = false;
    
    // Critical: Configure for 1 Mic, 0 Ref
    afe_config.pcm_config.total_ch_num = 1;
    afe_config.pcm_config.mic_num = 1;
    afe_config.pcm_config.ref_num = 0;
    
    // Use v1 implementation for ESP32
    afe_handle = &esp_afe_sr_v1;

    afe_data = afe_handle->create_from_config(&afe_config);
    
    if (!afe_data) {
        ESP_LOGE(TAG, "Failed to create AFE handle");
        return;
    }

    ESP_LOGI(TAG, "AFE Initialized. Listening for Wake Word...");

    // 4. Start Tasks
    xTaskCreatePinnedToCore(&feed_task, "feed", 3072, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(&detect_task, "detect", 3072, NULL, 5, NULL, 1);
}
