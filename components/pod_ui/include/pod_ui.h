/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pod_ui_start(lv_display_t *display, esp_lcd_panel_handle_t panel);

/* Debug/console helpers: switch tabs and open the queue overlay without touch. */
void pod_ui_show_tab(int tab);
void pod_ui_show_queue(void);
void pod_ui_show_files_root(void);
void pod_ui_scroll_list(int32_t y);
void pod_ui_toggle_nav(void);
void pod_ui_next_track(void);
void pod_ui_play_index(int index);
void pod_ui_play_path_contains(const char *needle);
void pod_ui_toggle_cover_only(void);
void pod_ui_screen_toggle(void);
void pod_ui_save_data(void);
void pod_ui_set_auto_off(int seconds);
void pod_ui_show_storage(void);
void pod_ui_show_touch_cal(void);
void pod_ui_set_repeat(int mode); /* 0=off, 1=all, 2=one */
void pod_ui_show_top_list(void);
void pod_ui_toggle_favorite(void);
void pod_ui_show_favorites(void);
void pod_ui_toggle_lyrics(void);
void pod_ui_set_autosave(int minutes);

/* Debug: current raw cover (RGB888, 200x200) or NULL. */
const uint8_t *pod_ui_cover_data(size_t *len, uint16_t *w, uint16_t *h);

#ifdef __cplusplus
}
#endif
