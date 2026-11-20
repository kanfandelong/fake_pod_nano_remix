/*
 * SPDX-License-Identifier: MIT
 *
 * LVGL 9 display/touch port for the CO5300 QSPI panel.
 *
 * LVGL renders into an 80-line PSRAM partial buffer. The flush callback packs
 * each area into strips that fit a single persistent internal DMA buffer, so
 * the SPI driver never has to allocate a private bounce buffer per transfer.
 */

#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pod_display_init(esp_lcd_panel_handle_t panel,
                           esp_lcd_panel_io_handle_t panel_io,
                           esp_lcd_touch_handle_t touch,
                           lv_display_t **display_out);

esp_err_t pod_display_start(void);

typedef struct {
    bool valid;
    float x_scale;
    float x_offset;
    float y_scale;
    float y_offset;
} pod_touch_calibration_t;

/* Raw -> screen mapping: screen = raw * scale + offset. */
void pod_display_touch_calibration_get(pod_touch_calibration_t *out);
esp_err_t pod_display_touch_calibration_set(const pod_touch_calibration_t *cal);
esp_err_t pod_display_touch_calibration_reset(void);

/* Last raw CST820 coordinate read by the LVGL input callback. */
bool pod_display_get_last_raw_touch(uint16_t *x, uint16_t *y);

#ifdef __cplusplus
}
#endif
