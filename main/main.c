/*
 * INMP441 I2S Microphone Recorder (RAM Buffer Version)
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

static const char *TAG = "RECORDER";

/* Pins from menuconfig */
#define I2S_WS_GPIO   CONFIG_I2S_WS_GPIO
#define I2S_DIN_GPIO  CONFIG_I2S_DIN_GPIO
#define I2S_BCK_GPIO  CONFIG_I2S_BCK_GPIO

#define SAMPLE_RATE     CONFIG_SAMPLE_RATE
#define RECORD_SECONDS  CONFIG_RECORD_SECONDS

#define I2S_PORT        I2S_NUM_0
#define DMA_BUF_COUNT   16
#define DMA_BUF_LEN     1024

#define BITS_PER_SAMPLE 16
#define NUM_CHANNELS    1
#define BYTES_PER_SAMPLE (BITS_PER_SAMPLE/8)
#define BYTE_RATE       (SAMPLE_RATE * NUM_CHANNELS * BYTES_PER_SAMPLE)
#define RECORD_SIZE     (BYTE_RATE * RECORD_SECONDS)

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
        .num_channels = NUM_CHANNELS,
        .sample_rate = SAMPLE_RATE,
        .byte_rate = BYTE_RATE,
        .block_align = NUM_CHANNELS * BYTES_PER_SAMPLE,
        .bits_per_sample = BITS_PER_SAMPLE,
        .data_tag = {'d','a','t','a'},
        .data_size = data_size
    };
    return h;
}

/* I2S (MCLK disabled) */
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
                // vTaskDelay(1); // Optional delay
            }
        }
    }

    printf("\n===WAV_END===\n");
}

/* Record to RAM and send */
static void record_and_send(void)
{
    // 1. Allocate RAM for Header + PCM Data
    size_t header_size = sizeof(wav_header_t);
    size_t pcm_size = RECORD_SIZE;
    size_t total_size = header_size + pcm_size;

    uint8_t *ram_buf = malloc(total_size);
    if (ram_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate RAM buffer! (Size: %d)", total_size);
        return;
    }

    // 2. Prepare Header
    wav_header_t header = create_wav_header(pcm_size);
    memcpy(ram_buf, &header, header_size);

    // 3. Record PCM
    int32_t *i2s_buf = malloc(DMA_BUF_LEN * 4);
    int16_t *pcm_ptr = (int16_t *)(ram_buf + header_size);
    
    size_t bytes_recorded = 0;
    size_t bytes_read;

    ESP_LOGI(TAG, "Recording to RAM (%d bytes)...", pcm_size);
    
    while (bytes_recorded < pcm_size) {
        i2s_read(I2S_PORT, i2s_buf, DMA_BUF_LEN * 4, &bytes_read, portMAX_DELAY);

        int samples = bytes_read / 4;
        for (int i = 0; i < samples; i++) {
            // INMP441 24-bit data in 32-bit slot (left aligned)
            // Shift >> 11 to increase volume (was >> 14 which was too quiet)
            int32_t s = i2s_buf[i] >> 11;
            
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            
            if (bytes_recorded < pcm_size) {
                 pcm_ptr[bytes_recorded / 2] = (int16_t)s;
                 bytes_recorded += 2;
            }
        }
    }
    ESP_LOGI(TAG, "Recording done.");
    
    i2s_driver_uninstall(I2S_PORT); // Stop I2S
    free(i2s_buf);

    // 4. Send Base64 from RAM
    ESP_LOGI(TAG, "Sending data...");
    send_ram_as_base64(ram_buf, total_size);

    free(ram_buf);
    ESP_LOGI(TAG, "Done.");
}

/* Main */
void app_main(void)
{
    esp_task_wdt_deinit();

    ESP_LOGI(TAG, "INMP441 Recorder Start");

    init_i2s();

    vTaskDelay(pdMS_TO_TICKS(300));

    record_and_send();
}

