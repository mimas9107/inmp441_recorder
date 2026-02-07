/*
 * INMP441 I2S Microphone Recorder
 * Records audio to SPIFFS and transmits via Serial for debugging
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/unistd.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_err.h"
#include "sdkconfig.h"

static const char *TAG = "INMP441";

/* I2S Configuration from Kconfig */
#define I2S_BCK_GPIO        CONFIG_I2S_BCK_GPIO
#define I2S_WS_GPIO         CONFIG_I2S_WS_GPIO
#define I2S_DIN_GPIO        CONFIG_I2S_DIN_GPIO
#define SAMPLE_RATE         CONFIG_SAMPLE_RATE
#define RECORD_SECONDS      CONFIG_RECORD_SECONDS

/* Audio format */
#define BITS_PER_SAMPLE     16
#define NUM_CHANNELS        1
#define DMA_BUF_COUNT       8
#define DMA_BUF_LEN         1024

/* Calculate sizes */
#define BYTES_PER_SAMPLE    (BITS_PER_SAMPLE / 8)
#define BYTE_RATE           (SAMPLE_RATE * NUM_CHANNELS * BYTES_PER_SAMPLE)
#define RECORD_SIZE         (BYTE_RATE * RECORD_SECONDS)

/* WAV Header structure */
typedef struct __attribute__((packed)) {
    char riff_tag[4];           // "RIFF"
    uint32_t riff_size;         // File size - 8
    char wave_tag[4];           // "WAVE"
    char fmt_tag[4];            // "fmt "
    uint32_t fmt_size;          // 16 for PCM
    uint16_t audio_format;      // 1 for PCM
    uint16_t num_channels;      // 1 for mono
    uint32_t sample_rate;       // e.g. 16000
    uint32_t byte_rate;         // sample_rate * num_channels * bits_per_sample/8
    uint16_t block_align;       // num_channels * bits_per_sample/8
    uint16_t bits_per_sample;   // e.g. 16
    char data_tag[4];           // "data"
    uint32_t data_size;         // Audio data size
} wav_header_t;

static i2s_chan_handle_t rx_handle = NULL;

/* Create WAV header */
static wav_header_t create_wav_header(uint32_t data_size)
{
    wav_header_t header = {
        .riff_tag = {'R', 'I', 'F', 'F'},
        .riff_size = data_size + 36,
        .wave_tag = {'W', 'A', 'V', 'E'},
        .fmt_tag = {'f', 'm', 't', ' '},
        .fmt_size = 16,
        .audio_format = 1,
        .num_channels = NUM_CHANNELS,
        .sample_rate = SAMPLE_RATE,
        .byte_rate = BYTE_RATE,
        .block_align = NUM_CHANNELS * BYTES_PER_SAMPLE,
        .bits_per_sample = BITS_PER_SAMPLE,
        .data_tag = {'d', 'a', 't', 'a'},
        .data_size = data_size
    };
    return header;
}

/* Initialize SPIFFS */
static esp_err_t init_spiffs(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount SPIFFS");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS: total=%d, used=%d", total, used);
    }

    return ESP_OK;
}

/* Initialize I2S for INMP441 (Standard mode) */
static esp_err_t init_i2s(void)
{
    ESP_LOGI(TAG, "Initializing I2S for INMP441");
    ESP_LOGI(TAG, "  BCK: GPIO%d, WS: GPIO%d, DIN: GPIO%d", I2S_BCK_GPIO, I2S_WS_GPIO, I2S_DIN_GPIO);
    ESP_LOGI(TAG, "  Sample Rate: %d Hz, Bits: %d", SAMPLE_RATE, BITS_PER_SAMPLE);

    /* Create I2S channel */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = DMA_BUF_LEN;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    /* Configure I2S Standard mode for INMP441
     * INMP441 outputs 32-bit words with 24-bit data, left-justified (MSB first)
     * We use I2S_STD_PHILIPS format with 32-bit slot width
     */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_GPIO,
            .ws = I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    /* INMP441 L/R pin: LOW = Left channel, HIGH = Right channel
     * If your INMP441 L/R is tied to GND, use left slot
     * If tied to VDD, use right slot
     */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));

    ESP_LOGI(TAG, "I2S initialized successfully");
    return ESP_OK;
}

/* Record audio to SPIFFS */
static esp_err_t record_audio(const char *filename)
{
    ESP_LOGI(TAG, "Recording %d seconds to %s", RECORD_SECONDS, filename);

    /* Delete existing file */
    struct stat st;
    if (stat(filename, &st) == 0) {
        unlink(filename);
    }

    /* Open file */
    FILE *f = fopen(filename, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return ESP_FAIL;
    }

    /* Write WAV header (will update later) */
    wav_header_t header = create_wav_header(RECORD_SIZE);
    fwrite(&header, sizeof(header), 1, f);

    /* Allocate buffer for I2S read (32-bit samples from INMP441) */
    size_t buf_size = DMA_BUF_LEN * 4;  // 32-bit = 4 bytes per sample
    int32_t *i2s_buf = (int32_t *)malloc(buf_size);
    if (!i2s_buf) {
        ESP_LOGE(TAG, "Failed to allocate I2S buffer");
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    /* Allocate buffer for converted 16-bit samples */
    int16_t *pcm_buf = (int16_t *)malloc(DMA_BUF_LEN * 2);
    if (!pcm_buf) {
        ESP_LOGE(TAG, "Failed to allocate PCM buffer");
        free(i2s_buf);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    uint32_t bytes_written = 0;
    size_t bytes_read = 0;
    int sample_count = 0;

    ESP_LOGI(TAG, "Starting recording... (speak now!)");

    while (bytes_written < RECORD_SIZE) {
        /* Read 32-bit samples from I2S */
        esp_err_t ret = i2s_channel_read(rx_handle, i2s_buf, buf_size, &bytes_read, 1000);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "I2S read failed: %s", esp_err_to_name(ret));
            continue;
        }

        int samples_read = bytes_read / 4;  // 32-bit = 4 bytes

        /* Convert 32-bit to 16-bit
         * INMP441 outputs 24-bit data in 32-bit word, left-justified (MSB first)
         * Data is in bits [31:8], bits [7:0] are zeros
         * We shift right by 16 to get the top 16 bits
         */
        for (int i = 0; i < samples_read; i++) {
            /* Shift right by 14 to preserve more dynamic range (24-bit -> 16-bit) */
            pcm_buf[i] = (int16_t)(i2s_buf[i] >> 14);
        }

        /* Print first few samples for debugging */
        if (sample_count < 3) {
            ESP_LOGI(TAG, "Raw[0-3]: 0x%08lX, 0x%08lX, 0x%08lX, 0x%08lX",
                     (long)i2s_buf[0], (long)i2s_buf[1], (long)i2s_buf[2], (long)i2s_buf[3]);
            ESP_LOGI(TAG, "PCM[0-3]: %d, %d, %d, %d",
                     pcm_buf[0], pcm_buf[1], pcm_buf[2], pcm_buf[3]);
            sample_count++;
        }

        /* Calculate how many bytes to write */
        size_t pcm_bytes = samples_read * 2;  // 16-bit = 2 bytes
        if (bytes_written + pcm_bytes > RECORD_SIZE) {
            pcm_bytes = RECORD_SIZE - bytes_written;
        }

        /* Write to file */
        fwrite(pcm_buf, 1, pcm_bytes, f);
        bytes_written += pcm_bytes;

        /* Progress indicator */
        static int last_percent = -1;
        int percent = (bytes_written * 100) / RECORD_SIZE;
        if (percent != last_percent && percent % 20 == 0) {
            ESP_LOGI(TAG, "Recording: %d%%", percent);
            last_percent = percent;
        }
    }

    ESP_LOGI(TAG, "Recording complete! Total bytes: %lu", (unsigned long)bytes_written);

    free(i2s_buf);
    free(pcm_buf);
    fclose(f);

    return ESP_OK;
}

/* Send file via Serial as hex dump (for debugging) */
static void send_file_hex(const char *filename)
{
    FILE *f = fopen(filename, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        return;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    ESP_LOGI(TAG, "File size: %ld bytes", file_size);

    /* Send start marker */
    printf("\n===WAV_START===\n");
    printf("SIZE:%ld\n", file_size);

    /* Send file content as base64 */
    static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t buf[3];
    size_t bytes_read;
    int line_count = 0;

    while ((bytes_read = fread(buf, 1, 3, f)) > 0) {
        char out[5] = {0};

        out[0] = base64_table[buf[0] >> 2];
        out[1] = base64_table[((buf[0] & 0x03) << 4) | (bytes_read > 1 ? (buf[1] >> 4) : 0)];
        out[2] = bytes_read > 1 ? base64_table[((buf[1] & 0x0F) << 2) | (bytes_read > 2 ? (buf[2] >> 6) : 0)] : '=';
        out[3] = bytes_read > 2 ? base64_table[buf[2] & 0x3F] : '=';

        printf("%s", out);
        line_count += 4;
        if (line_count >= 76) {
            printf("\n");
            line_count = 0;
        }
    }

    if (line_count > 0) {
        printf("\n");
    }
    printf("===WAV_END===\n\n");

    fclose(f);

    ESP_LOGI(TAG, "File sent via Serial. Use the Python script to decode.");
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== INMP441 Recorder ===");
    ESP_LOGI(TAG, "This will record %d seconds of audio and send via Serial", RECORD_SECONDS);

    /* Initialize SPIFFS */
    ESP_ERROR_CHECK(init_spiffs());

    /* Initialize I2S */
    ESP_ERROR_CHECK(init_i2s());

    /* Wait a moment for I2S to stabilize */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Record audio */
    const char *filename = "/spiffs/record.wav";
    ESP_ERROR_CHECK(record_audio(filename));

    /* Disable I2S after recording */
    ESP_ERROR_CHECK(i2s_channel_disable(rx_handle));
    ESP_ERROR_CHECK(i2s_del_channel(rx_handle));

    /* Send file via Serial */
    ESP_LOGI(TAG, "Sending WAV file via Serial...");
    send_file_hex(filename);

    ESP_LOGI(TAG, "Done! You can now use the Python script to receive the WAV file.");
    ESP_LOGI(TAG, "Or reflash to record again.");
}
