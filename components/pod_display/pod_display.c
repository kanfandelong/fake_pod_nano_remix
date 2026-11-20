/*
 * SPDX-License-Identifier: MIT
 */

#include "pod_display.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "pod_display";

#define DISP_W 460
#define DISP_H 460
/* The flush callback is synchronous, so a second full-screen PSRAM buffer
 * bought no overlap.  An 80-line partial buffer keeps the same DMA chunking
 * while freeing ~1 MB of PSRAM and improving cache locality. */
#define DRAW_BUF_LINES 460
/* 16 lines = 22 KB instead of 33 KB of internal DMA staging RAM.  The extra
 * SPI transactions are cheap compared with losing 11 KB of the LVGL heap. */
#define DMA_CHUNK_LINES 10

#define DRAW_BUF_BYTES (DISP_W * DRAW_BUF_LINES * 3)
#define DMA_BUF_BYTES  (DISP_W * DMA_CHUNK_LINES * 3)

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_touch_handle_t s_touch;

static lv_display_t *s_display;
static lv_indev_t *s_indev;
static SemaphoreHandle_t s_flush_done;
static uint8_t *s_draw_buf_a;
static uint8_t *s_draw_buf_b;
static uint8_t *s_dma_buf;

#define TOUCH_CAL_NS "podtouch"
#define TOUCH_CAL_KEY "calibration"
#define TOUCH_CAL_MAGIC 0x5443414Cu /* TCAL */
#define TOUCH_CAL_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    float x_scale;
    float x_offset;
    float y_scale;
    float y_offset;
} touch_cal_nvs_t;

static pod_touch_calibration_t s_touch_cal = {
    .valid = false,
    .x_scale = 1.0f,
    .x_offset = 0.0f,
    .y_scale = 1.0f,
    .y_offset = 0.0f,
};
static volatile uint16_t s_last_raw_x;
static volatile uint16_t s_last_raw_y;
static volatile bool s_last_raw_valid;

static bool touch_cal_valid(const pod_touch_calibration_t *cal)
{
    return cal && isfinite(cal->x_scale) && isfinite(cal->y_scale) &&
           cal->x_scale > 0.2f && cal->x_scale < 5.0f &&
           cal->y_scale > 0.2f && cal->y_scale < 5.0f &&
           fabsf(cal->x_offset) < 4096.0f &&
           fabsf(cal->y_offset) < 4096.0f;
}

static void touch_cal_load(void)
{
    nvs_handle_t h;
    if (nvs_open(TOUCH_CAL_NS, NVS_READONLY, &h) != ESP_OK) return;

    touch_cal_nvs_t b;
    size_t len = sizeof(b);
    esp_err_t err = nvs_get_blob(h, TOUCH_CAL_KEY, &b, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(b) ||
        b.magic != TOUCH_CAL_MAGIC || b.version != TOUCH_CAL_VERSION) {
        return;
    }

    pod_touch_calibration_t cal = {
        .valid = true,
        .x_scale = b.x_scale,
        .x_offset = b.x_offset,
        .y_scale = b.y_scale,
        .y_offset = b.y_offset,
    };
    if (touch_cal_valid(&cal)) {
        s_touch_cal = cal;
        ESP_LOGI(TAG, "touch calibration loaded: x=%.4f*raw%+.2f "
                 "y=%.4f*raw%+.2f", cal.x_scale, cal.x_offset,
                 cal.y_scale, cal.y_offset);
    }
}

void pod_display_touch_calibration_get(pod_touch_calibration_t *out)
{
    if (out) *out = s_touch_cal;
}

esp_err_t pod_display_touch_calibration_set(const pod_touch_calibration_t *cal)
{
    if (!cal || !touch_cal_valid(cal)) return ESP_ERR_INVALID_ARG;

    s_touch_cal = *cal;
    s_touch_cal.valid = true;

    touch_cal_nvs_t b = {
        .magic = TOUCH_CAL_MAGIC,
        .version = TOUCH_CAL_VERSION,
        .x_scale = cal->x_scale,
        .x_offset = cal->x_offset,
        .y_scale = cal->y_scale,
        .y_offset = cal->y_offset,
    };
    nvs_handle_t h;
    esp_err_t err = nvs_open(TOUCH_CAL_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, TOUCH_CAL_KEY, &b, sizeof(b));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "touch calibration saved: x=%.4f*raw%+.2f "
                 "y=%.4f*raw%+.2f", cal->x_scale, cal->x_offset,
                 cal->y_scale, cal->y_offset);
    }
    return err;
}

esp_err_t pod_display_touch_calibration_reset(void)
{
    s_touch_cal = (pod_touch_calibration_t){
        .valid = false,
        .x_scale = 1.0f,
        .x_offset = 0.0f,
        .y_scale = 1.0f,
        .y_offset = 0.0f,
    };

    nvs_handle_t h;
    esp_err_t err = nvs_open(TOUCH_CAL_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, TOUCH_CAL_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "touch calibration reset");
    return err;
}

bool pod_display_get_last_raw_touch(uint16_t *x, uint16_t *y)
{
    if (!s_last_raw_valid) return false;
    if (x) *x = s_last_raw_x;
    if (y) *y = s_last_raw_y;
    return true;
}

static uint32_t tick_get_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    (void)io;
    (void)edata;
    (void)user_ctx;
    BaseType_t hp_task_woken = pdFALSE;
    if (s_flush_done) {
        xSemaphoreGiveFromISR(s_flush_done, &hp_task_woken);
    }
    return hp_task_woken == pdTRUE;
}

static void rounder_cb(lv_event_t *event)
{
    lv_area_t *area = lv_event_get_param(event);
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int x1 = area->x1;
    const int y1 = area->y1;
    const int x2 = area->x2 + 1;
    const int y2 = area->y2 + 1;
    const int width = x2 - x1;
    const int height = y2 - y1;
    const int stride = width * 3;

    /* LVGL's flush pointer addresses the top-left of `area` inside the
     * full-width draw buffer.  Rows are separated by the buffer stride, which
     * is wider than the area for narrow invalidations.  Pack rows into the DMA
     * buffer instead of assuming area-width rows. */
    lv_draw_buf_t *draw_buf = lv_display_get_buf_active(disp);
    uint32_t buffer_stride = draw_buf ? draw_buf->header.stride : DISP_W * 3;
    if (buffer_stride < (uint32_t)stride) buffer_stride = (uint32_t)stride;

    for (int row = 0; row < height; row += DMA_CHUNK_LINES) {
        int lines = height - row;
        if (lines > DMA_CHUNK_LINES) {
            lines = DMA_CHUNK_LINES;
        }

        for (int line = 0; line < lines; line++) {
            memcpy(s_dma_buf + (size_t)line * stride,
                   px_map + (size_t)(row + line) * buffer_stride,
                   (size_t)stride);
        }

        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x1, y1 + row,
                                                  x2, y1 + row + lines,
                                                  s_dma_buf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "panel draw chunk failed: %s", esp_err_to_name(err));
            break;
        }

        if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "flush chunk timeout");
            break;
        }
    }

    lv_display_flush_ready(disp);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    if (!s_touch) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    esp_lcd_touch_read_data(s_touch);
    esp_lcd_touch_point_data_t point = {0};
    uint8_t point_num = 0;
    esp_err_t err = esp_lcd_touch_get_data(s_touch, &point, &point_num, 1);
    if (err == ESP_OK && point_num > 0) {
        s_last_raw_x = point.x;
        s_last_raw_y = point.y;
        s_last_raw_valid = true;

        int32_t x = point.x;
        int32_t y = point.y;
        if (s_touch_cal.valid) {
            x = (int32_t)lroundf(x * s_touch_cal.x_scale + s_touch_cal.x_offset);
            y = (int32_t)lroundf(y * s_touch_cal.y_scale + s_touch_cal.y_offset);
        }
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= DISP_W) x = DISP_W - 1;
        if (y >= DISP_H) y = DISP_H - 1;

        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        s_last_raw_valid = false;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void lvgl_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms < 5) delay_ms = 5;
        if (delay_ms > 20) delay_ms = 20;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

esp_err_t pod_display_init(esp_lcd_panel_handle_t panel,
                           esp_lcd_panel_io_handle_t panel_io,
                           esp_lcd_touch_handle_t touch,
                           lv_display_t **display_out)
{
    if (!panel || !panel_io || !display_out) {
        return ESP_ERR_INVALID_ARG;
    }

    s_panel = panel;
    s_panel_io = panel_io;
    s_touch = touch;
    touch_cal_load();

    s_draw_buf_a = heap_caps_aligned_alloc(64, DRAW_BUF_BYTES,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_draw_buf_b = heap_caps_aligned_alloc(64, DRAW_BUF_BYTES,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_dma_buf = heap_caps_aligned_alloc(64, DMA_BUF_BYTES,
                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_flush_done = xSemaphoreCreateBinary();

    if (!s_draw_buf_a || !s_draw_buf_b || !s_dma_buf || !s_flush_done) {
        ESP_LOGE(TAG, "display buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(
                            s_panel_io, &cbs, NULL),
                        TAG, "panel io callbacks");

    lv_init();
    lv_tick_set_cb(tick_get_cb);

    s_display = lv_display_create(DISP_W, DISP_H);
    if (!s_display) {
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB888);
    lv_display_set_buffers(s_display, s_draw_buf_a, s_draw_buf_b,
                           DRAW_BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_display, flush_cb);
    lv_display_add_event_cb(s_display, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_set_default(s_display);

    if (s_touch) {
        s_indev = lv_indev_create();
        if (!s_indev) {
            return ESP_ERR_NO_MEM;
        }
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, touch_read_cb);
        lv_indev_set_display(s_indev, s_display);
    }

    *display_out = s_display;
    ESP_LOGI(TAG, "display ready: %dx%d RGB888, draw=%d lines, DMA=%d lines",
             DISP_W, DISP_H, DRAW_BUF_LINES, DMA_CHUNK_LINES);
    return ESP_OK;
}

esp_err_t pod_display_start(void)
{
    if (!s_display) {
        return ESP_ERR_INVALID_STATE;
    }
    /* TJPGD needs a 4 KB work buffer and image decode runs in this task.
     * External fonts no longer load multi-megabyte tables here, so 14 KB is
     * enough headroom without spending another 4 KB of internal RAM. */
    BaseType_t ok = xTaskCreatePinnedToCore(lvgl_task, "lvgl", 14336, NULL,
                                            5, NULL, 1);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

