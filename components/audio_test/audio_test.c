/*
 * SPDX-License-Identifier: MIT
 *
 * 48 kHz / 32-bit stereo sine test for the CS43131 path.
 *
 * The CS43131 is configured as ASP slave; ESP32-S3 generates BCLK/LRCK and
 * sends 32-bit I2S samples on GPIO39.
 */

#include "audio_test.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "audio_test";

#define I2S_TEST_PORT          I2S_NUM_0
#define I2S_TEST_BCK_GPIO      GPIO_NUM_40
#define I2S_TEST_WS_GPIO       GPIO_NUM_38
#define I2S_TEST_DOUT_GPIO     GPIO_NUM_39
#define I2S_TEST_RATE          48000
#define I2S_TEST_FRAMES        256
#define I2S_TEST_AMPLITUDE     (0.18f)

typedef enum {
    TONE_CMD_NONE = 0,
    TONE_CMD_PLAY,
    TONE_CMD_STOP,
} tone_cmd_t;

static i2s_chan_handle_t s_tx;
static TaskHandle_t s_task;
static volatile uint32_t s_frequency;
static volatile uint32_t s_duration_ms;
static volatile tone_cmd_t s_command;
static volatile bool s_stop_requested;
static volatile bool s_playing;

static esp_err_t create_i2s_tx(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_TEST_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = (uint32_t)10;
    chan_cfg.dma_frame_num = 256;
    chan_cfg.auto_clear = true;

    /* Without auto-clear, a TX DMA underrun can repeat the last block and
     * leave a continuous tone after the software buffer has ended. */
    chan_cfg.auto_clear_after_cb = true;
    chan_cfg.auto_clear_before_cb = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s new channel");


    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg.sample_rate_hz = 44100;
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_PLL_240M;
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std_cfg.slot_cfg.ws_width = I2S_SLOT_BIT_WIDTH_32BIT;
    std_cfg.slot_cfg.ws_pol = false;
    std_cfg.slot_cfg.bit_shift = true;
    std_cfg.slot_cfg.left_align = false;
    std_cfg.slot_cfg.big_endian = false;
    std_cfg.slot_cfg.bit_order_lsb = false;
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = (gpio_num_t)I2S_TEST_BCK_GPIO;
    std_cfg.gpio_cfg.ws = (gpio_num_t)I2S_TEST_WS_GPIO;
    std_cfg.gpio_cfg.dout = (gpio_num_t)I2S_TEST_DOUT_GPIO;
    std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv = false;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s std mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s enable");
    return ESP_OK;
}

static void tone_task(void *arg)
{
    (void)arg;
    int32_t *stereo = NULL;
    const size_t sample_pairs = I2S_TEST_FRAMES;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        tone_cmd_t command = s_command;
        s_command = TONE_CMD_NONE;
        if (command == TONE_CMD_STOP) {
            s_stop_requested = false;
            s_playing = false;
            ESP_LOGI(TAG, "tone stopped");
            continue;
        }

        if (!stereo) {
            stereo = (int32_t *)heap_caps_malloc(sample_pairs * 2 * sizeof(int32_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!stereo) {
                stereo = (int32_t *)malloc(sample_pairs * 2 * sizeof(int32_t));
            }
        }
        if (!stereo) {
            ESP_LOGE(TAG, "no memory for tone buffer");
            continue;
        }

        const uint32_t freq = s_frequency;
        const uint32_t duration = s_duration_ms;
        const uint32_t total_frames = (I2S_TEST_RATE * duration) / 1000;
        const double step = 2.0 * M_PI * (double)freq / (double)I2S_TEST_RATE;
        const int32_t peak = (int32_t)(INT32_MAX * I2S_TEST_AMPLITUDE);
        double phase = 0.0;
        uint32_t written = 0;

        s_stop_requested = false;
        s_playing = true;
        ESP_LOGI(TAG, "tone %" PRIu32 " Hz, %" PRIu32 " ms", freq, duration);

        while (written < total_frames && !s_stop_requested) {
            size_t frames = total_frames - written;
            if (frames > sample_pairs) {
                frames = sample_pairs;
            }

            for (size_t i = 0; i < frames; i++) {
                double env = 1.0;
                const uint32_t abs_frame = written + (uint32_t)i;
                const uint32_t fade_frames = I2S_TEST_RATE / 50; /* 20 ms */
                if (abs_frame < fade_frames) {
                    env = (double)abs_frame / (double)fade_frames;
                } else if (abs_frame + fade_frames > total_frames) {
                    env = (double)(total_frames - abs_frame) / (double)fade_frames;
                }
                if (s_stop_requested) {
                    env = 0.0;
                }
                const int32_t sample = (int32_t)(sin(phase) * peak * env);
                stereo[i * 2 + 0] = sample;
                stereo[i * 2 + 1] = sample;
                phase += step;
                if (phase >= 2.0 * M_PI) {
                    phase -= 2.0 * M_PI;
                }
            }

            size_t bytes_written = 0;
            const size_t bytes = frames * 2 * sizeof(int32_t);
            esp_err_t err = i2s_channel_write(s_tx, stereo, bytes, &bytes_written,
                                              pdMS_TO_TICKS(200));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "i2s write: %s", esp_err_to_name(err));
                break;
            }
            written += frames;
        }

        if (s_stop_requested) {
            ESP_LOGI(TAG, "tone stopped");
        } else {
            ESP_LOGI(TAG, "tone done");
        }
        /* Let the last DMA block drain into silence rather than a held DC. */
        vTaskDelay(pdMS_TO_TICKS(30));
        s_playing = false;
    }
}

esp_err_t audio_test_init(void)
{
    if (s_tx) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(create_i2s_tx(), TAG, "i2s init");
    BaseType_t ok = xTaskCreatePinnedToCore(tone_task, "audio_test", 6144, NULL, 5, &s_task, 0);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t audio_test_play_tone(uint32_t frequency_hz, uint32_t duration_ms)
{
    if (!s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Abort any tone currently in flight and queue a new PLAY command. */
    s_frequency = frequency_hz ? frequency_hz : 1000;
    s_duration_ms = duration_ms ? duration_ms : 1500;
    s_command = TONE_CMD_PLAY;
    s_stop_requested = true;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t audio_test_stop(void)
{
    if (!s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    s_command = TONE_CMD_STOP;
    s_stop_requested = true;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

bool audio_test_is_playing(void)
{
    return s_playing;
}

i2s_chan_handle_t audio_test_i2s_handle(void)
{
    return s_tx;
}
