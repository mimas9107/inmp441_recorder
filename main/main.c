/*
 * INMP441 I2S Microphone Recorder (Stable Version)
 */

#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_spiffs.h"
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
#define DMA_BUF_COUNT   8
#define DMA_BUF_LEN     512

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

/* SPIFFS */
static void init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
    ESP_LOGI(TAG, "SPIFFS mounted");
}

/* I2S (MCLK disabled) */
static void init_i2s(void)
{
    ESP_LOGI(TAG, "Init I2S");

    i2s_config_t cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = DMA_BUF_COUNT,
        .dma_buf_len = DMA_BUF_LEN,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 1,
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

/* Record */
static void record_audio(const char *filename)
{
    struct stat st;
    if (stat(filename, &st) == 0) unlink(filename);

    FILE *f = fopen(filename, "wb");
    wav_header_t header = create_wav_header(RECORD_SIZE);
    fwrite(&header, sizeof(header), 1, f);

    int32_t *i2s_buf = malloc(DMA_BUF_LEN * 4);
    int16_t *pcm_buf = malloc(DMA_BUF_LEN * 2);

    size_t bytes_written = 0;
    size_t bytes_read;

    ESP_LOGI(TAG, "Recording...");
    //static  int once=0;
    while (bytes_written < RECORD_SIZE) {
        i2s_read(I2S_PORT, i2s_buf, DMA_BUF_LEN * 4, &bytes_read, portMAX_DELAY);

        int samples = bytes_read / 4;
        for (int i = 0; i < samples; i++) {
            //pcm_buf[i] = (int16_t)(i2s_buf[i] >> 14);
	    int32_t s = i2s_buf[i] >> 8;     // 24bit 對齊
            s *= 1;                        // software gain
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            pcm_buf[i] = (int16_t)s;

        }
	//static int once=0;
	//if(!once){
	//  ESP_LOGI(TAG, "raw=%ld pcm=%d", i2s_buf[0], pcm_buf[0]);
	//  once=1;
	//}

        size_t pcm_bytes = samples * 2;
        if (bytes_written + pcm_bytes > RECORD_SIZE)
            pcm_bytes = RECORD_SIZE - bytes_written;

        fwrite(pcm_buf, 1, pcm_bytes, f);
        bytes_written += pcm_bytes;
    }

    fclose(f);
    free(i2s_buf);
    free(pcm_buf);

    ESP_LOGI(TAG, "Record done");
}

/* Send base64 safely */
static void send_file_hex(const char *filename)
{
    FILE *f = fopen(filename, "rb");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    printf("\n===WAV_START===\n");
    printf("SIZE:%ld\n", size);

    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    uint8_t buf[3];
    size_t n;
    int cnt = 0;

    while ((n = fread(buf, 1, 3, f)) > 0) {
        char o[4];
        o[0] = tbl[buf[0] >> 2];
        o[1] = tbl[((buf[0] & 3) << 4) | (n > 1 ? buf[1] >> 4 : 0)];
        o[2] = n > 1 ? tbl[((buf[1] & 0xF) << 2) | (n > 2 ? buf[2] >> 6 : 0)] : '=';
        o[3] = n > 2 ? tbl[buf[2] & 0x3F] : '=';

        for (int i = 0; i < 4; i++) {
            putchar(o[i]);
            if (++cnt % 512 == 0) {
                vTaskDelay(1);
                //esp_task_wdt_reset();
            }
        }
    }

    printf("\n===WAV_END===\n");
    fclose(f);
}

/* Main */
void app_main(void)
{
    esp_task_wdt_deinit();

    ESP_LOGI(TAG, "INMP441 Recorder Start");

    init_spiffs();
    init_i2s();

    vTaskDelay(pdMS_TO_TICKS(300));

    const char *file = "/spiffs/record.wav";
    record_audio(file);

    i2s_driver_uninstall(I2S_PORT);

    ESP_LOGI(TAG, "Sending file...");
    send_file_hex(file);

    ESP_LOGI(TAG, "Done");
}

