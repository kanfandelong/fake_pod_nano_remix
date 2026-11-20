/*
 * SPDX-License-Identifier: MIT
 */

#include "pod_audio.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cs43131.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "espaudiocore.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "pod_audio";

#define POD_I2S_PORT       I2S_NUM_0
#define POD_I2S_BCLK_GPIO  GPIO_NUM_40
#define POD_I2S_WS_GPIO    GPIO_NUM_38
#define POD_I2S_DOUT_GPIO  GPIO_NUM_39

static i2s_chan_handle_t s_tx;
static volatile pod_audio_state_t s_state;
static volatile bool s_eos_pending;
static pod_audio_metadata_t s_probe_meta;
static char s_path[512];
static char s_error[96];
static SemaphoreHandle_t s_lock;

static void probe_meta_cb(void *user, const espaudiocore_meta_t *meta)
{
    (void)user;
    if (!meta || !meta->type || !meta->data) return;

    if (!meta->is_binary) {
        if (strcasecmp(meta->type, "title") == 0) {
            snprintf(s_probe_meta.title, sizeof(s_probe_meta.title), "%s",
                     meta->data);
        } else if (strcasecmp(meta->type, "artist") == 0) {
            snprintf(s_probe_meta.artist, sizeof(s_probe_meta.artist), "%s",
                     meta->data);
        } else if (strcasecmp(meta->type, "album") == 0) {
            snprintf(s_probe_meta.album, sizeof(s_probe_meta.album), "%s",
                     meta->data);
        } else if ((strcasecmp(meta->type, "lyrics") == 0 ||
                    strcasecmp(meta->type, "unsyncedlyrics") == 0 ||
                    strcasecmp(meta->type, "unsynced lyrics") == 0 ||
                    strcasecmp(meta->type, "USLT") == 0 ||
                    strcasecmp(meta->type, "SYLT") == 0) &&
                   meta->len > 0 && !s_probe_meta.lyrics) {
            char *lyrics = heap_caps_malloc(meta->len + 1, MALLOC_CAP_SPIRAM);
            if (lyrics) {
                memcpy(lyrics, meta->data, meta->len);
                lyrics[meta->len] = '\0';
                s_probe_meta.lyrics = lyrics;
                s_probe_meta.lyrics_len = meta->len;
            }
        }
        return;
    }

    bool is_cover = strcasecmp(meta->type, "APIC") == 0 ||
                    strcasecmp(meta->type, "PICTURE") == 0;
    if (!is_cover || meta->len == 0) return;
    if (meta->mime && !strstr(meta->mime, "jpeg") &&
        !strstr(meta->mime, "jpg")) {
        return; /* the UI cover decoder is TJPGD/JPEG only */
    }

    uint8_t *buf = heap_caps_malloc(meta->len, MALLOC_CAP_SPIRAM);
    if (!buf) return;
    memcpy(buf, meta->data, meta->len);
    if (s_probe_meta.cover) heap_caps_free(s_probe_meta.cover);
    s_probe_meta.cover = buf;
    s_probe_meta.cover_len = meta->len;
    snprintf(s_probe_meta.mime, sizeof(s_probe_meta.mime), "%s",
             meta->mime ? meta->mime : "image/jpeg");
    s_probe_meta.pic_type = meta->pic_type;
}

static void probe_meta_clear_unlocked(void)
{
    if (s_probe_meta.cover) {
        heap_caps_free(s_probe_meta.cover);
    }
    if (s_probe_meta.lyrics) {
        heap_caps_free(s_probe_meta.lyrics);
    }
    memset(&s_probe_meta, 0, sizeof(s_probe_meta));
}

static void audio_event_cb(void *user, espaudiocore_event_t ev, const char *msg)
{
    (void)user;
    switch (ev) {
    case ESPAUDIOCORE_EVT_STARTED:
        s_state = POD_AUDIO_PLAYING;
        break;
    case ESPAUDIOCORE_EVT_PAUSED:
        s_state = POD_AUDIO_PAUSED;
        break;
    case ESPAUDIOCORE_EVT_RESUMED:
        s_state = POD_AUDIO_PLAYING;
        break;
    case ESPAUDIOCORE_EVT_EOS:
        s_eos_pending = true;
        s_state = POD_AUDIO_IDLE;
        break;
    case ESPAUDIOCORE_EVT_STOPPED:
        s_state = POD_AUDIO_IDLE;
        break;
    case ESPAUDIOCORE_EVT_ERROR:
        s_state = POD_AUDIO_ERROR;
        snprintf(s_error, sizeof(s_error), "%s", msg ? msg : "decode error");
        ESP_LOGE(TAG, "decoder: %s", s_error);
        break;
    default:
        break;
    }
}

static esp_err_t create_i2s_tx(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(POD_I2S_PORT,
                                                            I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 10;
    chan_cfg.dma_frame_num = 256;
    chan_cfg.auto_clear = true;
    chan_cfg.auto_clear_after_cb = true;
    chan_cfg.auto_clear_before_cb = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL),
                        TAG, "i2s new channel");

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
    std_cfg.gpio_cfg.bclk = (gpio_num_t)POD_I2S_BCLK_GPIO;
    std_cfg.gpio_cfg.ws = (gpio_num_t)POD_I2S_WS_GPIO;
    std_cfg.gpio_cfg.dout = (gpio_num_t)POD_I2S_DOUT_GPIO;
    std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv = false;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg),
                        TAG, "i2s std mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s enable");
    return ESP_OK;
}

esp_err_t pod_audio_init(void)
{
    s_state = POD_AUDIO_IDLE;
    s_path[0] = '\0';
    s_error[0] = '\0';
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_tx) {
        esp_err_t err = create_i2s_tx();
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static void audio_lock(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void audio_unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

esp_err_t pod_audio_play(const char *path)
{
    if (!path || path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    audio_lock();
    esp_err_t ret = ESP_OK;
    s_eos_pending = false;

    if (espaudiocore_is_running()) {
        (void)espaudiocore_stop();
    }

    snprintf(s_path, sizeof(s_path), "%s", path);
    s_error[0] = '\0';

    i2s_chan_handle_t tx = s_tx;
    if (!tx) {
        snprintf(s_error, sizeof(s_error), "I2S not initialized");
        s_state = POD_AUDIO_ERROR;
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }

    const espaudiocore_cfg_t cfg = {
        .task_stack = 0,
        .task_priority = 12,
        .task_core = 0,
        .io_timeout_ms = 0,
        .output_bits = 32,
        .volume_db = 0.0f,
        .on_event = audio_event_cb,
        .on_meta = NULL,
        .on_pcm = NULL,
        .user = NULL,
    };

    int dec_err = 0;
    esp_err_t err = espaudiocore_begin(path, tx, &cfg, &dec_err);
    if (err != ESP_OK) {
        snprintf(s_error, sizeof(s_error), "begin: %s (dec=%d)",
                 esp_err_to_name(err), dec_err);
        s_state = POD_AUDIO_ERROR;
        ESP_LOGE(TAG, "begin %s failed: %s dec=%d", path, esp_err_to_name(err), dec_err);
        ret = err;
        goto done;
    }

    /* The decoder applies the stream rate to I2S internally. Tell the DAC
     * which rate family it is receiving as soon as the format is known. */
    vTaskDelay(pdMS_TO_TICKS(30));
    uint32_t rate = 0;
    uint8_t channels = 0, bits = 0;
    if (espaudiocore_get_format(&rate, &channels, &bits) == ESP_OK && rate) {
        if (cs43131_is_ready()) {
            esp_err_t dac_err = cs43131_set_sample_rate(rate);
            if (dac_err != ESP_OK) {
                ESP_LOGW(TAG, "DAC rate %" PRIu32 " update failed: %s",
                         rate, esp_err_to_name(dac_err));
            }
        }
        ESP_LOGI(TAG, "playing %s (%" PRIu32 " Hz, %u ch, %u bit)",
                 path, rate, channels, bits);
    } else {
        ESP_LOGW(TAG, "format unknown for %s", path);
    }

    s_state = POD_AUDIO_PLAYING;

done:
    audio_unlock();
    return ret;
}

esp_err_t pod_audio_pause(void)
{
    audio_lock();
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_state == POD_AUDIO_PLAYING) {
        err = espaudiocore_pause();
    }
    audio_unlock();
    return err;
}

esp_err_t pod_audio_resume(void)
{
    audio_lock();
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_state == POD_AUDIO_PAUSED) {
        err = espaudiocore_resume();
    }
    audio_unlock();
    return err;
}

esp_err_t pod_audio_stop(void)
{
    audio_lock();
    esp_err_t err = ESP_OK;
    if (espaudiocore_is_running()) {
        err = espaudiocore_stop();
    }
    s_state = POD_AUDIO_IDLE;
    audio_unlock();
    return err;
}

esp_err_t pod_audio_seek_ms(int64_t ms)
{
    audio_lock();
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_state == POD_AUDIO_PLAYING || s_state == POD_AUDIO_PAUSED) {
        err = espaudiocore_seek_ms(ms);
    }
    audio_unlock();
    return err;
}

esp_err_t pod_audio_set_volume_db(float db)
{
    /* The product volume is the CS43131's PCM volume (0.5 dB steps).
     * Keep the decoder at unity so software and hardware attenuation are not
     * applied twice. */
    audio_lock();
    esp_err_t ret = ESP_OK;
    if (cs43131_is_ready()) {
        esp_err_t err = cs43131_set_volume_db(db);
        if (err == ESP_OK) {
            (void)espaudiocore_set_volume_db(0.0f);
            goto done;
        }
        ESP_LOGW(TAG, "hardware volume failed: %s; using software volume",
                 esp_err_to_name(err));
    }
    ret = espaudiocore_set_volume_db(db);

done:
    audio_unlock();
    return ret;
}

pod_audio_state_t pod_audio_state(void)
{
    return s_state;
}

bool pod_audio_is_running(void)
{
    return s_state == POD_AUDIO_PLAYING || s_state == POD_AUDIO_PAUSED;
}

int64_t pod_audio_position_ms(void)
{
    return espaudiocore_get_position_ms();
}

int64_t pod_audio_duration_ms(void)
{
    return espaudiocore_get_duration_ms();
}

esp_err_t pod_audio_get_format(uint32_t *rate, uint8_t *channels, uint8_t *bits)
{
    return espaudiocore_get_format(rate, channels, bits);
}

const char *pod_audio_current_path(void)
{
    return s_path;
}

const char *pod_audio_last_error(void)
{
    return s_error;
}

esp_err_t pod_audio_probe(const char *path)
{
    if (!path || path[0] != '/') return ESP_ERR_INVALID_ARG;

    audio_lock();
    probe_meta_clear_unlocked();

    i2s_chan_handle_t tx = s_tx;
    if (!tx) {
        audio_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    const espaudiocore_cfg_t cfg = {
        .output_bits = 32,
        .volume_db = 0.0f,
        .on_meta = probe_meta_cb,
        .user = NULL,
    };
    int dec_err = 0;
    esp_err_t err = espaudiocore_probe(path, tx, &cfg, &dec_err);
    s_probe_meta.valid = err == ESP_OK;
    audio_unlock();

    ESP_LOGI(TAG, "probe %s: %s title=\"%s\" cover=%u lyrics=%u",
             path, esp_err_to_name(err), s_probe_meta.title,
             (unsigned)s_probe_meta.cover_len,
             (unsigned)s_probe_meta.lyrics_len);
    return err;
}

const pod_audio_metadata_t *pod_audio_probe_meta(void)
{
    return &s_probe_meta;
}

void pod_audio_probe_clear(void)
{
    audio_lock();
    probe_meta_clear_unlocked();
    audio_unlock();
}

bool pod_audio_take_eos(void)
{
    bool pending = s_eos_pending;
    s_eos_pending = false;
    return pending;
}

void pod_audio_clear_eos(void)
{
    s_eos_pending = false;
}
