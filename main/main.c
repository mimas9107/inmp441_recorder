/*
 * INMP441 I2S Recorder with Simple VAD (RMS-based)
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdbool.h> // Include this
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_task_wdt.h"
#include "sdkconfig.h"

static const char *TAG = "RECORDER";

/* Pins from menuconfig */
#define I2S_WS_GPIO   CONFIG_I2S_WS_GPIO
#define I2S_DIN_GPIO  CONFIG_I2S_DIN_GPIO
#define I2S_BCK_GPIO  CONFIG_I2S_BCK_GPIO

#define SAMPLE_RATE     16000
#define I2S_PORT        I2S_NUM_0
#define DMA_BUF_COUNT   8
#define DMA_BUF_LEN     512

/* VAD Configuration */
#define VAD_FRAME_SIZE      512  // Samples per frame
#define VAD_THRESHOLD       1000 // RMS Threshold (Adjust based on noise floor)
#define VAD_SILENCE_FRAMES  30   // How many silent frames to stop recording (approx 1 sec)
#define RECORD_TIME_SEC     3    // Fixed recording time after trigger

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

/* Calculate RMS of a frame */
static float calculate_rms(int16_t *data, int samples)
{
    float sum = 0.0f;
    for (int i = 0; i < samples; i++) {
        float val = (float)data[i];
        sum += val * val;
    }
    return sqrtf(sum / samples);
}

/* Send RAM buffer as Base64 */
static void send_ram_as_base64(const uint8_t *data, size_t len)
{
    printf("\n===WAV_START===\n");
    printf("SIZE:%d\n", len);

    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    uint8_t buf[3];
    int cnt = 0;
    
    for (size_t i = 0; i < len; i += 3) {
        size_t n = 0;
        buf[0] = data[i]; n++;
        if (i+1 < len) { buf[1] = data[i+1]; n++; }
        if (i+2 < len) { buf[2] = data[i+2]; n++; }

        char o[4];
        o[0] = tbl[buf[0] >> 2];
        o[1] = tbl[((buf[0] & 3) << 4) | (n > 1 ? buf[1] >> 4 : 0)];
        o[2] = n > 1 ? tbl[((buf[1] & 0xF) << 2) | (n > 2 ? buf[2] >> 6 : 0)] : '=';
        o[3] = n > 2 ? tbl[buf[2] & 0x3F] : '=';

        for (int j = 0; j < 4; j++) {
            putchar(o[j]);
            if (++cnt % 128 == 0) {
                // vTaskDelay(1); 
            }
        }
    }

    printf("\n===WAV_END===\n");
}

/* Main VAD Task */
void vad_task(void *arg)
{
    size_t bytes_read;
    int32_t *i2s_buff = malloc(VAD_FRAME_SIZE * 4 * sizeof(int32_t)); 
    int16_t *vad_buff = malloc(VAD_FRAME_SIZE * sizeof(int16_t));   
    
    // Allocate RAM for recording (3 seconds)
    // 16000 * 2 * 3 = 96000 bytes. Plus header 44 bytes.
    size_t pcm_size = SAMPLE_RATE * 2 * RECORD_TIME_SEC;
    size_t header_size = sizeof(wav_header_t);
    size_t total_size = header_size + pcm_size;
    
    uint8_t *rec_buf = malloc(total_size);
    if (!rec_buf || !i2s_buff || !vad_buff) {
        ESP_LOGE(TAG, "Failed to allocate memory!");
        vTaskDelete(NULL);
    }
    
    // Initialize recording buffer pointer
    int16_t *pcm_start = (int16_t *)(rec_buf + header_size);

    ESP_LOGI(TAG, "VAD Listening... (Threshold: %d)", VAD_THRESHOLD);

    while (1) {
        // 1. Read Frame
        i2s_read(I2S_PORT, i2s_buff, VAD_FRAME_SIZE * 4, &bytes_read, portMAX_DELAY);
        int samples = bytes_read / 4;
        
        // 2. Convert & RMS
        for (int i = 0; i < samples; i++) {
            int32_t s = i2s_buff[i] >> 11; // Gain adjusted
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            vad_buff[i] = (int16_t)s;
        }
        
        float rms = calculate_rms(vad_buff, samples);
        
        // 3. Trigger Logic
        if (rms > VAD_THRESHOLD) {
            ESP_LOGI(TAG, ">>> VAD Triggered! (RMS: %.1f) Recording %d sec...", rms, RECORD_TIME_SEC);
            
            // Start Recording Process
            // Reset PCM pointer
            int16_t *pcm_ptr = pcm_start;
            size_t recorded_samples = 0;
            size_t target_samples = pcm_size / 2;
            
            // Pre-fill with current frame
            memcpy(pcm_ptr, vad_buff, samples * 2);
            pcm_ptr += samples;
            recorded_samples += samples;
            
            // Continue recording loop
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
            
            // Prepare Header
            wav_header_t header = create_wav_header(pcm_size);
            memcpy(rec_buf, &header, header_size);
            
            ESP_LOGI(TAG, "Recording Done. Sending...");
            send_ram_as_base64(rec_buf, total_size);
            ESP_LOGI(TAG, "Sent. Resuming VAD...");
            
            // Clear buffer to avoid re-triggering immediately
            i2s_zero_dma_buffer(I2S_PORT);
            
            // Wait a bit to let noise settle
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        
        vTaskDelay(pdMS_TO_TICKS(10)); // Slight delay to yield
    }
}

void app_main(void)
{
    esp_task_wdt_deinit();
    init_i2s();
    
    xTaskCreate(vad_task, "vad", 4096, NULL, 5, NULL);
}
