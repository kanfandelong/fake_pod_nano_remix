/*
 * SPDX-License-Identifier: MIT
 *
 * POD NANO LVGL UI. 460x460, folder-first library, real SD/LittleFS browsing
 * and real playback through pod_audio.
 */

#include "pod_ui.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "libs/tjpgd/tjpgd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pod_audio.h"
#include "pod_display.h"
#include "pod_font.h"
#include "heart_icons.h"
#include "pod_metadata.h"
#include "pod_storage.h"

static const char *TAG = "pod_ui";

/* -------------------------------------------------------------------------- */
/* Design tokens                                                              */
/* -------------------------------------------------------------------------- */

#define UI_W 460
#define UI_H 460
#define TOP_H 38
#define NAV_H 64
#define PAGE_H (UI_H - TOP_H - NAV_H)
#define PAGE_H_FULL (UI_H - TOP_H)
#define BOOT_GPIO GPIO_NUM_0
/* Second hardware key: short press toggles the pure-cover page. */
#define BTN_GPIO GPIO_NUM_48
#define MAX_QUEUE 512
#define MAX_ROWS 512
/* Recycled scroll list: seven row widgets cover a ~5-row viewport plus
 * overscan, no matter how many folder entries are in the directory. */
#define LIST_POOL 7
#define LIST_ROW_H 66
#define QUEUE_ROWS_PER_PAGE 5

#define C_BG        0x050506
#define C_SURFACE   0x141416
#define C_SURFACE2  0x1D1D21
#define C_BORDER    0x2A2A2E
#define C_TEXT      0xF4F1EA
#define C_MUTED     0x8A8882
#define C_ACCENT    0xFF7A67
#define C_ACCENT2   0xA78BFA
#define C_GREEN     0xB8D8B0
#define C_NO_BG     0xFFFFFFFFu
#define COVER_SIZE  200

static const lv_font_t *F_TITLE = &lv_font_montserrat_28;
static const lv_font_t *F_NOW = &lv_font_montserrat_24;
static const lv_font_t *F_SECTION = &lv_font_montserrat_20;
static const lv_font_t *F_BODY = &lv_font_montserrat_16;
static const lv_font_t *F_SMALL = &lv_font_montserrat_14;
static const lv_font_t *F_MICRO = &lv_font_montserrat_12;

/* The external font partition supplies full-CJK 16 px and 24 px faces.  They
 * include ASCII too, so body/title text uses one consistent family; Montserrat
 * remains the fallback for LV_SYMBOL_* glyphs.  Smaller/larger Latin text stays
 * on Montserrat and falls back to the nearest external CJK face, which keeps
 * the Latin UI crisp without allocating a full table per size. */
static lv_font_t font_title_cn;
static lv_font_t font_section_cn;
static lv_font_t font_now_cn;
static lv_font_t font_body_cn;
static lv_font_t font_small_cn;
static const lv_font_t *cjk_fallback;
static const lv_font_t *F_TITLE_CN = &font_title_cn;
static const lv_font_t *F_SECTION_CN = &font_section_cn;
static const lv_font_t *F_NOW_CN = &font_now_cn;
static const lv_font_t *F_BODY_CN = &font_body_cn;
static const lv_font_t *F_SMALL_CN = &font_small_cn;

static void init_cn_fonts(void)
{
    lv_font_t *cjk16 = pod_font_load_font(16);
    lv_font_t *cjk24 = pod_font_load_font(24);

    if (cjk16) cjk16->fallback = &lv_font_montserrat_16;
    /* The 24 px display face has fewer CJK/punctuation glyphs than the 16 px
     * body face.  Fall back to the full 16 px set before Montserrat so a rare
     * character never renders as a blank box in a large title. */
    if (cjk24) {
        cjk24->fallback = cjk16 ? cjk16
                                : (const lv_font_t *)&lv_font_montserrat_24;
    }

    cjk_fallback = cjk16 ? cjk16 : &lv_font_source_han_sans_sc_16_cjk;

    font_title_cn = lv_font_montserrat_28;
    font_title_cn.fallback = cjk24 ? cjk24 : cjk_fallback;

    font_section_cn = lv_font_montserrat_20;
    font_section_cn.fallback = cjk_fallback;

    font_now_cn = cjk24 ? *cjk24 : lv_font_montserrat_24;
    if (!cjk24) font_now_cn.fallback = cjk_fallback;

    font_body_cn = cjk16 ? *cjk16 : lv_font_montserrat_16;
    if (!cjk16) font_body_cn.fallback = cjk_fallback;

    font_small_cn = lv_font_montserrat_14;
    font_small_cn.fallback = cjk_fallback;
}

typedef struct {
    lv_obj_t *row;
    lv_obj_t *icon;
    lv_obj_t *name;
    lv_obj_t *meta;
    lv_obj_t *chev;
    size_t index;
    char name_buf[160];
    char meta_buf[48];
} list_row_t;

typedef struct {
    lv_obj_t *list;
    lv_obj_t *content;
    list_row_t rows[LIST_POOL];
    size_t count;
    size_t first;
} scroll_list_t;

typedef enum {
    REPEAT_OFF = 0,
    REPEAT_ALL,
    REPEAT_ONE,
} repeat_mode_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *top;
    lv_obj_t *page;
    lv_obj_t *top_title;
    lv_obj_t *top_sd;
    lv_obj_t *top_battery;
    lv_obj_t *nav;
    lv_obj_t *nav_btn[5];
    lv_obj_t *nav_icon[5];
    lv_obj_t *nav_text[5];
    lv_obj_t *nav_line[5];
    lv_timer_t *timer;

    bool nav_visible;
    bool boot_prev;
    bool btn_prev;
    int32_t page_h;
    int tab;
    int settings_page;
    esp_lcd_panel_handle_t panel;
    lv_indev_t *indev;

    char lib_cwd[512];
    char files_cwd[512];
    bool lib_root;
    bool library_add_mode;
    bool files_root;
    bool lib_favorites;
    size_t lib_fav_page;
    int listening_page;

    pod_dir_entry_t *entries;
    size_t entry_count;

    pod_dir_entry_t *queue;
    size_t queue_count;
    size_t queue_index;

    char now_path[512];
    char now_title[128];
    char now_artist[128];
    char now_album[128];
    int volume_db;
    int brightness;
    int auto_off_sec;
    int autosave_min;
    bool gapless;
    bool screen_off;
    bool cover_only;
    bool touch_enabled;
    bool song_stats_enabled;
    bool play_dirty;
    int64_t last_activity_us;
    int64_t last_play_change_us;
    int64_t last_autosave_us;
    char cover_path[260];
    uint8_t *cover_data;
    uint16_t cover_w;
    uint16_t cover_h;
    size_t cover_size;
    uint8_t *cover_large_data;
    uint16_t cover_large_w;
    uint16_t cover_large_h;
    size_t cover_large_size;
    uint32_t cover_bg1;
    uint32_t cover_bg2;
    lv_image_dsc_t cover_dsc;
    bool has_meta;
    bool has_cover;

    lv_obj_t *np_title;
    lv_obj_t *np_artist;
    lv_obj_t *np_artist_box;
    lv_obj_t *np_album;
    lv_obj_t *np_album_box;
    lv_obj_t *np_cover;
    lv_obj_t *np_cover_initial;
    lv_obj_t *np_cover_img;
    lv_obj_t *np_slider;
    lv_obj_t *np_slider_track;
    lv_obj_t *np_cur;
    lv_obj_t *np_dur;
    lv_obj_t *np_controls;
    lv_obj_t *np_play_icon;
    lv_obj_t *np_shuffle;
    lv_obj_t *np_repeat;
    lv_obj_t *np_repeat_one;
    lv_obj_t *np_format;
    lv_obj_t *np_fav_img;
    lv_obj_t *np_fav_btn;
    lv_obj_t *np_lyrics_btn;
    lv_obj_t *np_lyrics_label;
    lv_obj_t *np_lyrics_panel;
    lv_obj_t *np_lyrics_content;
    lv_obj_t *np_lyrics_lines[9];
    lv_obj_t *np_lyrics_text;
    char *lyrics;
    size_t lyrics_len;
    bool has_lyrics;
    bool lyrics_page;
    char np_format_cache[64];
    lv_obj_t *listen_bar;
    lv_obj_t *listen_cur;
    lv_obj_t *listen_dur;
    lv_obj_t *listen_state;
    bool seeking;
    bool shuffle;
    repeat_mode_t repeat;

    lv_obj_t *queue_overlay;
    size_t queue_page;

    uint32_t play_count;
    uint64_t played_seconds;
    uint32_t tick_quarter;

    /* Cached values so the 4 Hz timer only touches labels/sliders when the
     * content actually changes. */
    int last_pos_s;
    int last_dur_s;
    int last_slider;
    pod_audio_state_t last_state;
    bool error_shown;
    char last_error[64];
} ui_t;

static ui_t ui;
static scroll_list_t s_list;
static uint16_t s_eligible[MAX_ROWS];
static size_t s_eligible_count;

/* Console/debug requests are latched and executed from the LVGL timer task;
 * LVGL must never be touched concurrently from the REPL task. */
static volatile int s_pending_tab = -1;
static volatile bool s_pending_queue;
static volatile bool s_pending_files_root;
static volatile bool s_pending_nav_toggle;
static volatile bool s_pending_cover_toggle;
static volatile bool s_pending_storage;
static volatile bool s_pending_touch_cal;
static volatile bool s_pending_top_list;
static volatile bool s_pending_fav_toggle;
static volatile bool s_pending_favorites;
static volatile bool s_pending_lyrics_toggle;
static volatile int s_pending_repeat = -1;
static volatile bool s_pending_next;
static volatile int s_pending_play_index = -1;
static char s_play_needle[96];
static volatile bool s_pending_play_needle;
static volatile int32_t s_pending_scroll_y;
static volatile bool s_pending_scroll;

/* Metadata/cover extraction and audio start run on a low-priority worker, not
 * in the LVGL task.  The worker has a one-slot mailbox: a newer track request
 * overwrites a pending one, and a stale request is skipped before it can stop
 * the current decoder. */
typedef struct {
    char path[512];
    uint32_t seq;
} play_req_t;

static QueueHandle_t s_play_queue;
static SemaphoreHandle_t s_meta_lock;
static pod_metadata_t s_meta_result;
static uint32_t s_meta_seq;
static uint32_t s_play_seq;
static uint8_t *s_meta_cover;
static uint16_t s_meta_cover_w;
static uint16_t s_meta_cover_h;
static size_t s_meta_cover_size;
static uint8_t *s_meta_cover_large;
static uint16_t s_meta_cover_lw;
static uint16_t s_meta_cover_lh;
static size_t s_meta_cover_lsize;
static uint32_t s_meta_cover_bg1;
static uint32_t s_meta_cover_bg2;
static volatile bool s_meta_ready;
static char *s_lyrics_display;

/* LVGL's software draw tasks are asynchronous.  A cover buffer must stay
 * alive for at least a few refreshes after it is detached from the widget,
 * otherwise a queued draw can read freed PSRAM and paint noise. */
#define COVER_RETIRE_SLOTS 4
static uint8_t *s_cover_retire[COVER_RETIRE_SLOTS];
static int64_t s_cover_retire_us[COVER_RETIRE_SLOTS];
static uint8_t s_cover_retire_next;

static void cover_retire(uint8_t *data)
{
    if (!data) return;
    uint8_t slot = s_cover_retire_next++ % COVER_RETIRE_SLOTS;
    if (s_cover_retire[slot]) heap_caps_free(s_cover_retire[slot]);
    s_cover_retire[slot] = data;
    s_cover_retire_us[slot] = esp_timer_get_time();
}

static void cover_retire_poll(void)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < COVER_RETIRE_SLOTS; i++) {
        if (s_cover_retire[i] &&
            now - s_cover_retire_us[i] > 1000000) {
            heap_caps_free(s_cover_retire[i]);
            s_cover_retire[i] = NULL;
        }
    }
}

void pod_ui_rebuild(void);
static void queue_overlay_close(void);

/* -------------------------------------------------------------------------- */
/* Small style helpers                                                        */
/* -------------------------------------------------------------------------- */

static lv_color_t col(uint32_t hex)
{
    return lv_color_hex(hex);
}

static void no_scroll(lv_obj_t *obj)
{
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

/* Shared styles are the single biggest object-heap win in this UI.  Creating a
 * page used to add 6-10 local style properties to every row and label; now
 * every object points at one of a handful of immutable styles instead. */
typedef struct {
    uint32_t bg;
    int16_t radius;
    uint8_t border;
    uint8_t used;
    lv_style_t style;
} panel_style_cache_t;

typedef struct {
    const lv_font_t *font;
    uint32_t color;
    uint8_t used;
    lv_style_t style;
} text_style_cache_t;

#define PANEL_STYLE_CACHE_MAX 16
#define TEXT_STYLE_CACHE_MAX  24

static panel_style_cache_t s_panel_styles[PANEL_STYLE_CACHE_MAX];
static text_style_cache_t s_text_styles[TEXT_STYLE_CACHE_MAX];
static uint8_t s_panel_style_count;
static uint8_t s_text_style_count;

static lv_style_t *panel_style_for(uint32_t bg, int radius, bool border)
{
    for (uint8_t i = 0; i < s_panel_style_count; i++) {
        panel_style_cache_t *entry = &s_panel_styles[i];
        if (entry->bg == bg && entry->radius == radius &&
            entry->border == (border ? 1 : 0)) {
            return &entry->style;
        }
    }
    if (s_panel_style_count >= PANEL_STYLE_CACHE_MAX) return NULL;

    panel_style_cache_t *entry = &s_panel_styles[s_panel_style_count++];
    entry->bg = bg;
    entry->radius = radius;
    entry->border = border ? 1 : 0;
    entry->used = 1;
    lv_style_init(&entry->style);
    lv_style_set_bg_color(&entry->style, col(bg == C_NO_BG ? C_BG : bg));
    lv_style_set_bg_opa(&entry->style, bg == C_NO_BG ? LV_OPA_TRANSP : LV_OPA_COVER);
    lv_style_set_radius(&entry->style, radius);
    lv_style_set_border_width(&entry->style, border ? 1 : 0);
    lv_style_set_border_color(&entry->style, col(C_BORDER));
    lv_style_set_pad_all(&entry->style, 0);
    lv_style_set_outline_width(&entry->style, 0);
    lv_style_set_shadow_width(&entry->style, 0);
    return &entry->style;
}

static lv_style_t *text_style_for(const lv_font_t *font, uint32_t color)
{
    for (uint8_t i = 0; i < s_text_style_count; i++) {
        text_style_cache_t *entry = &s_text_styles[i];
        if (entry->font == font && entry->color == color) {
            return &entry->style;
        }
    }
    if (s_text_style_count >= TEXT_STYLE_CACHE_MAX) return NULL;

    text_style_cache_t *entry = &s_text_styles[s_text_style_count++];
    entry->font = font;
    entry->color = color;
    entry->used = 1;
    lv_style_init(&entry->style);
    lv_style_set_text_font(&entry->style, font);
    lv_style_set_text_color(&entry->style, col(color));
    lv_style_set_text_letter_space(&entry->style, 0);
    lv_style_set_text_opa(&entry->style, LV_OPA_COVER);
    return &entry->style;
}

static void apply_panel_style(lv_obj_t *obj, uint32_t bg, int radius, bool border)
{
    /* Do NOT call lv_obj_remove_style_all() here: LVGL stores width/height and
     * alignment as style properties, and most callers size/align the object
     * before applying this style.  Removing all styles wiped those values and
     * collapsed the whole layout to the top-left corner. */
    lv_style_t *style = panel_style_for(bg, radius, border);
    if (style) {
        lv_obj_add_style(obj, style, 0);
    } else {
        lv_obj_set_style_bg_color(obj, col(bg == C_NO_BG ? C_BG : bg), 0);
        lv_obj_set_style_bg_opa(obj, bg == C_NO_BG ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        lv_obj_set_style_radius(obj, radius, 0);
        lv_obj_set_style_border_width(obj, border ? 1 : 0, 0);
        lv_obj_set_style_border_color(obj, col(C_BORDER), 0);
        lv_obj_set_style_pad_all(obj, 0, 0);
    }
    no_scroll(obj);
}

static void button_style(lv_obj_t *obj, uint32_t bg, int radius)
{
    apply_panel_style(obj, bg, radius, false);
}

static void panel_style(lv_obj_t *obj, uint32_t bg, int radius)
{
    apply_panel_style(obj, bg, radius, false);
}

static void card_style(lv_obj_t *obj, uint32_t bg, int radius)
{
    apply_panel_style(obj, bg, radius, true);
}

/* Present a slider as a plain bar: no knob, but the full 32px object height
 * remains draggable (extended hit area). */
static void slider_as_bar(lv_obj_t *slider, int height, int track)
{
    if (!slider) return;
    int pad = (height - track) / 2;
    if (pad < 0) pad = 0;
    lv_obj_set_style_pad_top(slider, pad, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(slider, pad, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(slider, pad, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 0, LV_PART_KNOB);
    lv_obj_set_style_width(slider, 1, LV_PART_KNOB);
    lv_obj_set_style_height(slider, 1, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 16);
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *text,
                          const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_style_t *style = text_style_for(font, color);
    if (style) {
        lv_obj_add_style(label, style, 0);
    } else {
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, col(color), 0);
        lv_obj_set_style_text_letter_space(label, 0, 0);
    }
    lv_label_set_text(label, text ? text : "");
    return label;
}

static lv_obj_t *mk_label_static(lv_obj_t *parent, const char *text,
                                 const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = mk_label(parent, "", font, color);
    lv_label_set_text_static(label, text ? text : "");
    return label;
}

static void set_label_font(lv_obj_t *label, const lv_font_t *font, uint32_t color)
{
    if (!label) return;
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, col(color), 0);
}

static void format_time(int64_t ms, char *buf, size_t len)
{
    if (ms < 0) {
        snprintf(buf, len, "--:--");
        return;
    }
    int64_t total = ms / 1000;
    int64_t h = total / 3600;
    int64_t m = (total / 60) % 60;
    int64_t s = total % 60;
    if (h > 0) {
        snprintf(buf, len, "%" PRId64 ":%02" PRId64 ":%02" PRId64, h, m, s);
    } else {
        snprintf(buf, len, "%02" PRId64 ":%02" PRId64, m, s);
    }
}

static void format_size(uint32_t bytes, char *buf, size_t len)
{
    if (bytes >= 1024 * 1024) {
        snprintf(buf, len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        snprintf(buf, len, "%.0f KB", (double)bytes / 1024.0);
    } else {
        snprintf(buf, len, "%" PRIu32 " B", bytes);
    }
}

static void format_capacity(uint64_t bytes, char *buf, size_t len)
{
    if (bytes >= 1024ULL * 1024 * 1024) {
        snprintf(buf, len, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024) {
        snprintf(buf, len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(buf, len, "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(buf, len, "%llu B", (unsigned long long)bytes);
    }
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void title_from_path(const char *path, char *buf, size_t len)
{
    const char *name = base_name(path);
    snprintf(buf, len, "%s", name);
    char *dot = strrchr(buf, '.');
    if (dot && dot != buf) {
        *dot = '\0';
    }
}

static void parent_name(const char *path, char *buf, size_t len)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) {
        snprintf(buf, len, "MUSIC");
        return;
    }
    *slash = '\0';
    const char *name = strrchr(tmp, '/');
    snprintf(buf, len, "%s", name ? name + 1 : tmp);
}

static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open("podcfg", NVS_READONLY, &h) != ESP_OK) return;

    uint8_t b;
    int8_t v;
    uint16_t a;
    uint16_t autosave;
    uint8_t g;
    uint8_t song_stats;
    if (nvs_get_u8(h, "bright", &b) == ESP_OK && b >= 10 && b <= 100) {
        ui.brightness = b;
    }
    if (nvs_get_i8(h, "volume", &v) == ESP_OK && v >= -60 && v <= 0) {
        ui.volume_db = v;
    }
    if (nvs_get_u16(h, "auto_off", &a) == ESP_OK) {
        ui.auto_off_sec = a;
    }
    if (nvs_get_u16(h, "autosave_min", &autosave) == ESP_OK &&
        (autosave == 0 || autosave == 5 || autosave == 10 ||
         autosave == 15 || autosave == 30 || autosave == 45 ||
         autosave == 60)) {
        ui.autosave_min = autosave;
    }
    if (nvs_get_u8(h, "gapless", &g) == ESP_OK) {
        ui.gapless = g != 0;
    }
    if (nvs_get_u8(h, "songstats", &song_stats) == ESP_OK) {
        ui.song_stats_enabled = song_stats != 0;
    }
    nvs_close(h);
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("podcfg", NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_u8(h, "bright", (uint8_t)ui.brightness);
    (void)nvs_set_i8(h, "volume", (int8_t)ui.volume_db);
    (void)nvs_set_u16(h, "auto_off", (uint16_t)ui.auto_off_sec);
    (void)nvs_set_u16(h, "autosave_min", (uint16_t)ui.autosave_min);
    (void)nvs_set_u8(h, "gapless", ui.gapless ? 1 : 0);
    (void)nvs_set_u8(h, "songstats", ui.song_stats_enabled ? 1 : 0);
    (void)nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "settings saved: bright=%d vol=%d auto_off=%d autosave=%d "
             "gapless=%d songstats=%d",
             ui.brightness, ui.volume_db, ui.auto_off_sec, ui.autosave_min,
             ui.gapless, ui.song_stats_enabled);
}

#define PLAYBACK_FILE POD_STORAGE_USER_DATA "/playback.bin"
#define PLAYBACK_MAGIC 0x504C4159u
#define PLAYBACK_VERSION 2u

/* v1 is kept as a prefix so existing playback.bin files still load. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    char path[512];
    char title[128];
    char artist[128];
    char album[128];
} playback_blob_v1_t;

typedef struct {
    playback_blob_v1_t base;
    uint64_t played_seconds;
} playback_blob_t;

static void playback_load(void)
{
    FILE *fp = fopen(PLAYBACK_FILE, "rb");
    if (!fp) return;

    playback_blob_t b;
    memset(&b, 0, sizeof(b));
    size_t n = fread(&b, 1, sizeof(b), fp);
    fclose(fp);
    size_t v1_size = sizeof(playback_blob_v1_t);
    if ((n != sizeof(b) && n != v1_size) ||
        b.base.magic != PLAYBACK_MAGIC ||
        (b.base.version != 1 && b.base.version != PLAYBACK_VERSION)) {
        return;
    }
    if (b.base.version == 1) {
        b.played_seconds = 0;
    }

    b.base.path[sizeof(b.base.path) - 1] = '\0';
    b.base.title[sizeof(b.base.title) - 1] = '\0';
    b.base.artist[sizeof(b.base.artist) - 1] = '\0';
    b.base.album[sizeof(b.base.album) - 1] = '\0';
    ui.play_count = b.base.count;
    ui.played_seconds = b.played_seconds;
    snprintf(ui.now_path, sizeof(ui.now_path), "%s", b.base.path);
    snprintf(ui.now_title, sizeof(ui.now_title), "%s", b.base.title);
    snprintf(ui.now_artist, sizeof(ui.now_artist), "%s", b.base.artist);
    snprintf(ui.now_album, sizeof(ui.now_album), "%s", b.base.album);

    if (ui.now_path[0]) {
        if (!ui.now_title[0]) {
            title_from_path(ui.now_path, ui.now_title, sizeof(ui.now_title));
        }
        if (!ui.now_artist[0]) {
            parent_name(ui.now_path, ui.now_artist, sizeof(ui.now_artist));
        }
        ui.has_meta = ui.now_title[0] || ui.now_artist[0];
    }
    ui.play_dirty = false;
    ESP_LOGI(TAG, "playback loaded from littlefs: count=%" PRIu32
             " listened=%" PRIu64 "s path=%s",
             ui.play_count, ui.played_seconds, ui.now_path);
}

static SemaphoreHandle_t s_playback_lock;

static void playback_save_fields(const char *path, const char *title,
                                 const char *artist, const char *album,
                                 uint32_t count)
{
    if (s_playback_lock &&
        xSemaphoreTake(s_playback_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }

    playback_blob_t b;
    memset(&b, 0, sizeof(b));
    b.base.magic = PLAYBACK_MAGIC;
    b.base.version = PLAYBACK_VERSION;
    b.base.count = count;
    b.played_seconds = ui.played_seconds;
    snprintf(b.base.path, sizeof(b.base.path), "%s", path ? path : "");
    if (title && title[0]) {
        snprintf(b.base.title, sizeof(b.base.title), "%s", title);
    } else if (path && path[0]) {
        title_from_path(path, b.base.title, sizeof(b.base.title));
    }
    if (artist && artist[0]) {
        snprintf(b.base.artist, sizeof(b.base.artist), "%s", artist);
    } else if (path && path[0]) {
        parent_name(path, b.base.artist, sizeof(b.base.artist));
    }
    snprintf(b.base.album, sizeof(b.base.album), "%s", album ? album : "");

    FILE *fp = fopen(PLAYBACK_FILE, "wb");
    if (!fp) {
        ESP_LOGW(TAG, "playback save failed: %s", PLAYBACK_FILE);
        if (s_playback_lock) xSemaphoreGive(s_playback_lock);
        return;
    }
    size_t n = fwrite(&b, 1, sizeof(b), fp);
    fclose(fp);
    if (n != sizeof(b)) {
        ESP_LOGW(TAG, "playback save short write");
        if (s_playback_lock) xSemaphoreGive(s_playback_lock);
        return;
    }
    ui.play_dirty = false;
    ESP_LOGI(TAG, "playback saved to littlefs: count=%" PRIu32
             " listened=%" PRIu64 "s path=%s",
             count, ui.played_seconds, b.base.path);
    if (s_playback_lock) xSemaphoreGive(s_playback_lock);
}

static void playback_save(void)
{
    playback_save_fields(ui.now_path, ui.now_title, ui.now_artist,
                         ui.now_album, ui.play_count);
}

static bool autosave_due(void)
{
    if (ui.autosave_min <= 0) return true;
    int64_t now = esp_timer_get_time();
    return now - ui.last_autosave_us >=
           (int64_t)ui.autosave_min * 60 * 1000000;
}

static void autosave_mark(void)
{
    ui.last_autosave_us = esp_timer_get_time();
}

#define SONG_STATS_FILE POD_STORAGE_USER_DATA "/song_stats.bin"
#define SONG_STATS_TMP  POD_STORAGE_USER_DATA "/song_stats.tmp"
#define SONG_STATS_MAGIC 0x53535431u /* SST1 */
#define SONG_STATS_VERSION 1u
#define SONG_STAT_HASH_SIZE 32
#define SONG_STAT_RESERVED 24

typedef struct {
    uint8_t hash[SONG_STAT_HASH_SIZE];
    uint32_t play_count;
    uint32_t last_played; /* seconds since boot; reserved extension slot */
    uint8_t reserved[SONG_STAT_RESERVED];
} song_stat_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint16_t record_size;
    uint16_t reserved0;
    uint32_t record_count;
    uint8_t reserved[16];
} song_stats_header_t;

#define SONG_MAP_FILE POD_STORAGE_USER_DATA "/song_paths.bin"
#define SONG_MAP_TMP  POD_STORAGE_USER_DATA "/song_paths.tmp"
#define SONG_MAP_MAGIC 0x534D4150u /* SMAP */
#define SONG_MAP_VERSION 1u
#define SONG_MAP_PATH_SIZE 512
#define SONG_MAP_MAX 2048

typedef struct {
    uint8_t hash[SONG_STAT_HASH_SIZE];
    char path[SONG_MAP_PATH_SIZE];
} song_map_entry_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t reserved;
} song_map_header_t;

static song_stat_t *s_song_stats;
static size_t s_song_stats_count;
static size_t s_song_stats_cap;
static SemaphoreHandle_t s_song_stats_lock;
static volatile bool s_song_stats_dirty;
static bool s_song_stats_loaded;

static song_map_entry_t *s_song_map;
static size_t s_song_map_count;
static size_t s_song_map_cap;
static size_t s_song_map_evictions;
static bool s_song_map_full_warned;

#define SONG_FAVORITE_BYTE 0
#define SONG_FAVORITE_BIT  0x01
static char (*s_fav_paths)[SONG_MAP_PATH_SIZE];
static size_t s_fav_count;
static size_t s_fav_cap;

static size_t song_stats_unique_count(void)
{
    if (!ui.song_stats_enabled || !s_song_stats_lock) return 0;
    if (xSemaphoreTake(s_song_stats_lock, portMAX_DELAY) != pdTRUE) return 0;
    size_t n = s_song_stats_count;
    xSemaphoreGive(s_song_stats_lock);
    return n;
}

static bool song_path_hash(const char *path, uint8_t out[SONG_STAT_HASH_SIZE])
{
    size_t out_len = 0;
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGW(TAG, "psa_crypto_init: %ld", (long)st);
        return false;
    }
    st = psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)path,
                          strlen(path), out, SONG_STAT_HASH_SIZE, &out_len);
    return st == PSA_SUCCESS && out_len == SONG_STAT_HASH_SIZE;
}

static bool song_map_reserve(size_t need)
{
    if (need <= s_song_map_cap) return true;
    if (need > SONG_MAP_MAX) need = SONG_MAP_MAX;
    size_t cap = s_song_map_cap ? s_song_map_cap : 128;
    while (cap < need) cap *= 2;
    if (cap > SONG_MAP_MAX) cap = SONG_MAP_MAX;
    if (cap < need) return false;

    song_map_entry_t *p = heap_caps_realloc(s_song_map, cap * sizeof(*p),
                                            MALLOC_CAP_SPIRAM);
    if (!p) {
        ESP_LOGW(TAG, "song path map PSRAM grow failed (%u records)",
                 (unsigned)cap);
        return false;
    }
    s_song_map = p;
    s_song_map_cap = cap;
    return true;
}

static ssize_t song_map_find_locked(const uint8_t hash[SONG_STAT_HASH_SIZE]);
static void listening_rank_invalidate(void);

static void song_map_load(void)
{
    s_song_map_count = 0;
    s_song_map_cap = 0;
    s_song_map = NULL;
    s_song_map_evictions = 0;
    s_song_map_full_warned = false;
    (void)song_map_reserve(64);

    FILE *fp = fopen(SONG_MAP_FILE, "rb");
    if (!fp) return;

    song_map_header_t h;
    size_t n = fread(&h, 1, sizeof(h), fp);
    if (n != sizeof(h) || h.magic != SONG_MAP_MAGIC ||
        h.version != SONG_MAP_VERSION) {
        fclose(fp);
        return;
    }
    /* The file is append-only between compactions, so it may contain older
     * duplicate hashes.  Last occurrence wins. */
    for (uint32_t i = 0; i < h.count; i++) {
        song_map_entry_t e;
        if (fread(&e, 1, sizeof(e), fp) != sizeof(e)) break;
        e.path[SONG_MAP_PATH_SIZE - 1] = '\0';
        if (e.path[0] != '/') continue;
        ssize_t dup = song_map_find_locked(e.hash);
        if (dup >= 0) {
            snprintf(s_song_map[dup].path, sizeof(s_song_map[dup].path),
                     "%s", e.path);
            continue;
        }
        if (s_song_map_count >= SONG_MAP_MAX) continue;
        if (!song_map_reserve(s_song_map_count + 1)) break;
        s_song_map[s_song_map_count++] = e;
    }
    fclose(fp);
    ESP_LOGI(TAG, "song path map loaded: %u records",
             (unsigned)s_song_map_count);
}

static ssize_t song_map_find_locked(const uint8_t hash[SONG_STAT_HASH_SIZE])
{
    for (size_t i = 0; i < s_song_map_count; i++) {
        if (memcmp(s_song_map[i].hash, hash, SONG_STAT_HASH_SIZE) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static const char *song_map_path_locked(const uint8_t hash[SONG_STAT_HASH_SIZE])
{
    ssize_t i = song_map_find_locked(hash);
    return i >= 0 ? s_song_map[i].path : NULL;
}

/* Append one mapping.  This happens only the first time a song is seen, so
 * ordinary playback does not rewrite the path table. */
static void song_map_append_locked(const song_map_entry_t *e)
{
    FILE *fp = fopen(SONG_MAP_FILE, "r+b");
    bool created = false;
    if (!fp) {
        fp = fopen(SONG_MAP_FILE, "wb");
        created = true;
    }
    if (!fp) return;

    song_map_header_t h = {
        .magic = SONG_MAP_MAGIC,
        .version = SONG_MAP_VERSION,
    };
    if (created) {
        fwrite(&h, 1, sizeof(h), fp);
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return;
    }
    if (fwrite(e, 1, sizeof(*e), fp) != sizeof(*e)) {
        fclose(fp);
        return;
    }
    long end = ftell(fp);
    if (end < (long)sizeof(h)) {
        fclose(fp);
        return;
    }
    h.count = (uint32_t)((end - (long)sizeof(h)) / (long)sizeof(*e));
    if (fseek(fp, 0, SEEK_SET) != 0 ||
        fwrite(&h, 1, sizeof(h), fp) != sizeof(h)) {
        fclose(fp);
        return;
    }
    fclose(fp);
}

static uint32_t song_stat_count_locked(const uint8_t hash[SONG_STAT_HASH_SIZE])
{
    for (size_t i = 0; i < s_song_stats_count; i++) {
        if (memcmp(s_song_stats[i].hash, hash, SONG_STAT_HASH_SIZE) == 0) {
            return s_song_stats[i].play_count;
        }
    }
    return 0;
}

static bool song_stat_favorite_locked(const uint8_t hash[SONG_STAT_HASH_SIZE])
{
    for (size_t i = 0; i < s_song_stats_count; i++) {
        if (memcmp(s_song_stats[i].hash, hash, SONG_STAT_HASH_SIZE) == 0) {
            return (s_song_stats[i].reserved[SONG_FAVORITE_BYTE] &
                    SONG_FAVORITE_BIT) != 0;
        }
    }
    return false;
}

static void song_map_compact_locked(void)
{
    (void)remove(SONG_MAP_TMP);
    FILE *fp = fopen(SONG_MAP_TMP, "wb");
    if (!fp) return;

    song_map_header_t h = {
        .magic = SONG_MAP_MAGIC,
        .version = SONG_MAP_VERSION,
        .count = (uint32_t)s_song_map_count,
    };
    bool ok = fwrite(&h, 1, sizeof(h), fp) == sizeof(h);
    if (ok && s_song_map_count) {
        ok = fwrite(s_song_map, sizeof(song_map_entry_t), s_song_map_count,
                    fp) == s_song_map_count;
    }
    if (fclose(fp) != 0) ok = false;
    if (!ok) {
        (void)remove(SONG_MAP_TMP);
        return;
    }
    if (rename(SONG_MAP_TMP, SONG_MAP_FILE) != 0) {
        (void)remove(SONG_MAP_FILE);
        if (rename(SONG_MAP_TMP, SONG_MAP_FILE) != 0) {
            (void)remove(SONG_MAP_TMP);
        }
    }
}

static void song_map_note_locked(const uint8_t hash[SONG_STAT_HASH_SIZE],
                                 const char *path, bool force)
{
    if (!path || path[0] != '/' || !s_song_map) return;
    if (song_map_find_locked(hash) >= 0) return;

    if (s_song_map_count < SONG_MAP_MAX) {
        if (!song_map_reserve(s_song_map_count + 1)) return;
        song_map_entry_t *e = &s_song_map[s_song_map_count];
        memcpy(e->hash, hash, SONG_STAT_HASH_SIZE);
        snprintf(e->path, sizeof(e->path), "%s", path);
        s_song_map_count++;
        song_map_append_locked(e);
        return;
    }

    /* The cache is full: keep paths for the most-played songs.  Replace the
     * lowest-count entry only if the new song has a higher count. */
    uint32_t new_count = song_stat_count_locked(hash);
    size_t victim = 0;
    uint32_t victim_count = UINT32_MAX;
    for (size_t i = 0; i < s_song_map_count; i++) {
        if (song_stat_favorite_locked(s_song_map[i].hash)) continue;
        uint32_t c = song_stat_count_locked(s_song_map[i].hash);
        if (c < victim_count) {
            victim_count = c;
            victim = i;
        }
    }
    if (victim_count == UINT32_MAX) return; /* every mapped song is liked */
    if (!force && new_count <= victim_count) {
        if (!s_song_map_full_warned) {
            s_song_map_full_warned = true;
            ESP_LOGW(TAG, "song path map full; keeping top %u play-count paths",
                     (unsigned)SONG_MAP_MAX);
        }
        return;
    }

    song_map_entry_t *e = &s_song_map[victim];
    memcpy(e->hash, hash, SONG_STAT_HASH_SIZE);
    snprintf(e->path, sizeof(e->path), "%s", path);
    song_map_append_locked(e);
    s_song_map_evictions++;
    if (s_song_map_evictions >= 64) {
        s_song_map_evictions = 0;
        song_map_compact_locked();
    }
}

static bool song_stats_has_hash_locked(const uint8_t hash[SONG_STAT_HASH_SIZE])
{
    for (size_t i = 0; i < s_song_stats_count; i++) {
        if (memcmp(s_song_stats[i].hash, hash, SONG_STAT_HASH_SIZE) == 0) {
            return true;
        }
    }
    return false;
}

static void song_map_backfill_locked(const char *path)
{
    if (!path || path[0] != '/') return;
    uint8_t hash[SONG_STAT_HASH_SIZE];
    if (!song_path_hash(path, hash)) return;
    if (!song_stats_has_hash_locked(hash)) return;
    song_map_note_locked(hash, path, false);
}

static bool song_stats_reserve(size_t need)
{
    if (need <= s_song_stats_cap) return true;
    size_t cap = s_song_stats_cap ? s_song_stats_cap : 256;
    while (cap < need) cap *= 2;
    if (cap > 16384) cap = 16384;
    if (cap < need) return false;

    song_stat_t *p = heap_caps_realloc(s_song_stats, cap * sizeof(*p),
                                       MALLOC_CAP_SPIRAM);
    if (!p) {
        ESP_LOGW(TAG, "song stats PSRAM grow failed (%u records)",
                 (unsigned)cap);
        return false;
    }
    s_song_stats = p;
    s_song_stats_cap = cap;
    return true;
}

static void song_stats_load(void)
{
    s_song_stats_count = 0;
    s_song_stats_cap = 0;
    s_song_stats = NULL;
    s_song_stats_dirty = false;

    FILE *fp = fopen(SONG_STATS_FILE, "rb");
    if (!fp) {
        (void)song_stats_reserve(256);
        s_song_stats_loaded = true;
        return;
    }

    song_stats_header_t h;
    size_t n = fread(&h, 1, sizeof(h), fp);
    if (n != sizeof(h) || h.magic != SONG_STATS_MAGIC ||
        h.version != SONG_STATS_VERSION ||
        h.header_size != sizeof(h) || h.record_size != sizeof(song_stat_t) ||
        h.record_count > 16384) {
        fclose(fp);
        (void)song_stats_reserve(256);
        ESP_LOGW(TAG, "song stats header invalid");
        s_song_stats_loaded = true;
        return;
    }

    s_song_stats_count = h.record_count;
    if (!song_stats_reserve(s_song_stats_count + 64)) {
        s_song_stats_count = 0;
        fclose(fp);
        s_song_stats_loaded = true;
        return;
    }
    if (s_song_stats_count) {
        size_t got = fread(s_song_stats, sizeof(song_stat_t),
                           s_song_stats_count, fp);
        s_song_stats_count = got;
    }
    fclose(fp);
    s_song_stats_loaded = true;
    ESP_LOGI(TAG, "song stats loaded from littlefs: %u records",
             (unsigned)s_song_stats_count);
}

static ssize_t song_stats_find(const uint8_t hash[SONG_STAT_HASH_SIZE])
{
    for (size_t i = 0; i < s_song_stats_count; i++) {
        if (memcmp(s_song_stats[i].hash, hash, SONG_STAT_HASH_SIZE) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static void song_stats_note_play(const char *path)
{
    if (!ui.song_stats_enabled || !path || !s_song_stats_lock) return;
    uint8_t hash[SONG_STAT_HASH_SIZE];
    if (!song_path_hash(path, hash)) return;

    if (xSemaphoreTake(s_song_stats_lock, portMAX_DELAY) != pdTRUE) return;
    ssize_t idx = song_stats_find(hash);
    if (idx < 0) {
        if (!song_stats_reserve(s_song_stats_count + 1)) {
            xSemaphoreGive(s_song_stats_lock);
            return;
        }
        idx = (ssize_t)s_song_stats_count++;
        memset(&s_song_stats[idx], 0, sizeof(song_stat_t));
        memcpy(s_song_stats[idx].hash, hash, SONG_STAT_HASH_SIZE);
    }
    if (s_song_stats[idx].play_count != UINT32_MAX) {
        s_song_stats[idx].play_count++;
    }
    s_song_stats[idx].last_played =
        (uint32_t)(esp_timer_get_time() / 1000000);
    uint32_t count = s_song_stats[idx].play_count;
    song_map_note_locked(hash, path, false);
    listening_rank_invalidate();
    s_song_stats_dirty = true;
    xSemaphoreGive(s_song_stats_lock);

    ESP_LOGI(TAG, "song count: path=%s count=%" PRIu32, path, count);
}

static void song_stats_save(void)
{
    if (!s_song_stats_lock || !s_song_stats_loaded) return;
    if (xSemaphoreTake(s_song_stats_lock, portMAX_DELAY) != pdTRUE) return;

    song_stats_header_t h = {
        .magic = SONG_STATS_MAGIC,
        .version = SONG_STATS_VERSION,
        .header_size = sizeof(song_stats_header_t),
        .record_size = sizeof(song_stat_t),
        .record_count = (uint32_t)s_song_stats_count,
    };

    (void)remove(SONG_STATS_TMP);
    FILE *fp = fopen(SONG_STATS_TMP, "wb");
    if (!fp) {
        ESP_LOGW(TAG, "song stats save open failed");
        xSemaphoreGive(s_song_stats_lock);
        return;
    }
    bool ok = fwrite(&h, 1, sizeof(h), fp) == sizeof(h);
    if (ok && s_song_stats_count) {
        ok = fwrite(s_song_stats, sizeof(song_stat_t), s_song_stats_count, fp) ==
             s_song_stats_count;
    }
    if (fclose(fp) != 0) ok = false;

    if (ok) {
        if (rename(SONG_STATS_TMP, SONG_STATS_FILE) != 0) {
            /* LittleFS normally replaces an existing target; some VFS layers
             * need the old file removed first. */
            (void)remove(SONG_STATS_FILE);
            if (rename(SONG_STATS_TMP, SONG_STATS_FILE) != 0) ok = false;
        }
    }
    if (!ok) {
        (void)remove(SONG_STATS_TMP);
        ESP_LOGW(TAG, "song stats save failed");
    } else {
        s_song_stats_dirty = false;
        ESP_LOGI(TAG, "song stats saved to littlefs: %u records",
                 (unsigned)s_song_stats_count);
    }
    xSemaphoreGive(s_song_stats_lock);
}

#define LISTEN_TOP_MAX 5

typedef struct {
    uint32_t count;
    uint32_t last_played;
    char path[SONG_MAP_PATH_SIZE];
} listen_top_t;

static listen_top_t s_listen_top[LISTEN_TOP_MAX];
static size_t s_listen_top_count;

static uint16_t *s_rank_idx;
static size_t s_rank_count;
static size_t s_rank_page;
static bool s_rank_valid;

static int listen_rank_cmp(const void *a, const void *b)
{
    uint16_t ia = *(const uint16_t *)a;
    uint16_t ib = *(const uint16_t *)b;
    const song_stat_t *sa = &s_song_stats[ia];
    const song_stat_t *sb = &s_song_stats[ib];
    if (sa->play_count != sb->play_count) {
        return sa->play_count > sb->play_count ? -1 : 1;
    }
    if (sa->last_played != sb->last_played) {
        return sa->last_played > sb->last_played ? -1 : 1;
    }
    return 0;
}

static void listening_rank_invalidate(void)
{
    s_rank_valid = false;
}

static bool listening_rank_build(void)
{
    if (s_rank_valid && s_rank_idx) return true;
    if (s_rank_idx) heap_caps_free(s_rank_idx);
    s_rank_idx = NULL;
    s_rank_count = 0;
    s_rank_valid = false;
    if (!ui.song_stats_enabled || !s_song_stats_lock ||
        !s_song_stats_count) {
        return false;
    }

    uint16_t *idx = heap_caps_malloc(s_song_stats_count * sizeof(uint16_t),
                                     MALLOC_CAP_SPIRAM);
    if (!idx) return false;
    if (xSemaphoreTake(s_song_stats_lock, portMAX_DELAY) != pdTRUE) {
        heap_caps_free(idx);
        return false;
    }
    size_t n = s_song_stats_count;
    for (size_t i = 0; i < n; i++) idx[i] = (uint16_t)i;
    qsort(idx, n, sizeof(uint16_t), listen_rank_cmp);
    xSemaphoreGive(s_song_stats_lock);

    s_rank_idx = idx;
    s_rank_count = n;
    s_rank_valid = true;
    return true;
}

typedef struct {
    uint8_t hash[SONG_STAT_HASH_SIZE];
    uint32_t count;
    uint32_t last_played;
} listen_rank_t;

static size_t listen_top_build(void)
{
    s_listen_top_count = 0;
    if (!ui.song_stats_enabled || !s_song_stats_lock) return 0;
    if (xSemaphoreTake(s_song_stats_lock, portMAX_DELAY) != pdTRUE) return 0;

    listen_rank_t top[LISTEN_TOP_MAX] = {0};
    size_t n = 0;
    for (size_t i = 0; i < s_song_stats_count; i++) {
        const song_stat_t *s = &s_song_stats[i];
        size_t pos = 0;
        while (pos < n &&
               (top[pos].count > s->play_count ||
                (top[pos].count == s->play_count &&
                 top[pos].last_played >= s->last_played))) {
            pos++;
        }
        if (pos >= LISTEN_TOP_MAX) continue;
        if (n < LISTEN_TOP_MAX) n++;
        for (size_t j = n - 1; j > pos; j--) {
            top[j] = top[j - 1];
        }
        memcpy(top[pos].hash, s->hash, SONG_STAT_HASH_SIZE);
        top[pos].count = s->play_count;
        top[pos].last_played = s->last_played;
    }

    for (size_t i = 0; i < n; i++) {
        const char *path = song_map_path_locked(top[i].hash);
        s_listen_top[i].count = top[i].count;
        s_listen_top[i].last_played = top[i].last_played;
        if (path) {
            snprintf(s_listen_top[i].path, sizeof(s_listen_top[i].path),
                     "%s", path);
        } else {
            s_listen_top[i].path[0] = '\0';
        }
    }
    xSemaphoreGive(s_song_stats_lock);
    s_listen_top_count = n;
    return n;
}

static bool song_favorite_get_path(const char *path)
{
    if (!path || path[0] != '/' || !s_song_stats_lock) return false;
    uint8_t hash[SONG_STAT_HASH_SIZE];
    if (!song_path_hash(path, hash)) return false;
    bool favorite = false;
    if (xSemaphoreTake(s_song_stats_lock, portMAX_DELAY) == pdTRUE) {
        ssize_t idx = song_stats_find(hash);
        if (idx >= 0) {
            favorite = (s_song_stats[idx].reserved[SONG_FAVORITE_BYTE] &
                        SONG_FAVORITE_BIT) != 0;
        }
        xSemaphoreGive(s_song_stats_lock);
    }
    return favorite;
}

static bool song_favorite_toggle_path(const char *path)
{
    if (!path || path[0] != '/' || !s_song_stats_lock) return false;
    uint8_t hash[SONG_STAT_HASH_SIZE];
    if (!song_path_hash(path, hash)) return false;

    bool favorite = false;
    if (xSemaphoreTake(s_song_stats_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    ssize_t idx = song_stats_find(hash);
    if (idx < 0) {
        if (!song_stats_reserve(s_song_stats_count + 1)) {
            xSemaphoreGive(s_song_stats_lock);
            return false;
        }
        idx = (ssize_t)s_song_stats_count++;
        memset(&s_song_stats[idx], 0, sizeof(song_stat_t));
        memcpy(s_song_stats[idx].hash, hash, SONG_STAT_HASH_SIZE);
    }
    s_song_stats[idx].reserved[SONG_FAVORITE_BYTE] ^= SONG_FAVORITE_BIT;
    favorite = (s_song_stats[idx].reserved[SONG_FAVORITE_BYTE] &
                SONG_FAVORITE_BIT) != 0;
    song_map_note_locked(hash, path, true);
    s_song_stats_dirty = true;
    listening_rank_invalidate();
    xSemaphoreGive(s_song_stats_lock);

    /* This is an explicit user action, so persist immediately rather than
     * waiting for the next playback autosave. */
    song_stats_save();
    ESP_LOGI(TAG, "favorite %s -> %d", path, (int)favorite);
    return favorite;
}

static void song_favorites_collect(void)
{
    s_fav_count = 0;
    if (!s_song_stats_lock) return;
    if (xSemaphoreTake(s_song_stats_lock, portMAX_DELAY) != pdTRUE) return;

    for (size_t i = 0; i < s_song_stats_count; i++) {
        if ((s_song_stats[i].reserved[SONG_FAVORITE_BYTE] &
             SONG_FAVORITE_BIT) == 0) {
            continue;
        }
        const char *path = song_map_path_locked(s_song_stats[i].hash);
        if (!path || path[0] != '/') continue;

        if (s_fav_count >= s_fav_cap) {
            size_t cap = s_fav_cap ? s_fav_cap * 2 : 16;
            char (*p)[SONG_MAP_PATH_SIZE] =
                heap_caps_realloc(s_fav_paths, cap * SONG_MAP_PATH_SIZE,
                                  MALLOC_CAP_SPIRAM);
            if (!p) break;
            s_fav_paths = p;
            s_fav_cap = cap;
        }
        snprintf(s_fav_paths[s_fav_count], SONG_MAP_PATH_SIZE, "%s", path);
        s_fav_count++;
    }
    xSemaphoreGive(s_song_stats_lock);

    if (s_fav_count > 1 && s_fav_paths) {
        qsort(s_fav_paths, s_fav_count, SONG_MAP_PATH_SIZE,
              (int (*)(const void *, const void *))strcmp);
    }
    ESP_LOGI(TAG, "favorites collected: %u", (unsigned)s_fav_count);
}

static void save_all_littlefs_data(void)
{
    playback_save();
    song_stats_save();
    autosave_mark();
}

static void cover_palette(const char *path, uint32_t *a, uint32_t *b, char *initial)
{
    static const uint32_t pairs[][2] = {
        {0xFF7A67, 0x5B3A8E},
        {0x7DD3C7, 0x1F5F6B},
        {0xF2C14E, 0x7A3E2A},
        {0xA78BFA, 0x33265E},
        {0x8ECAE6, 0x1D3557},
    };
    uint32_t hash = 2166136261u;
    for (const char *p = path ? path : "POD"; *p; p++) {
        hash = (hash ^ (uint8_t)*p) * 16777619u;
    }
    const uint32_t *pair = pairs[hash % (sizeof(pairs) / sizeof(pairs[0]))];
    *a = pair[0];
    *b = pair[1];
    const char *name = base_name(path ? path : "POD");
    unsigned char first = (name && name[0]) ? (unsigned char)name[0] : 'P';
    initial[0] = (first >= 0x20 && first < 0x7F) ? (char)toupper(first) : 'P';
    initial[1] = '\0';
}

/* -------------------------------------------------------------------------- */
/* Queue / playback                                                           */
/* -------------------------------------------------------------------------- */

#define QUEUE_FILE POD_STORAGE_USER_DATA "/queue.bin"
#define QUEUE_MAGIC 0x51554555u /* QUEU */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t reserved;
} queue_header_t;

static uint32_t s_queue_saved_sig;
static bool s_queue_saved_valid;

static uint32_t queue_signature(void)
{
    uint32_t h = 2166136261u;
    h = (h ^ (uint32_t)ui.queue_count) * 16777619u;
    for (size_t i = 0; i < ui.queue_count; i++) {
        const char *p = ui.queue[i].path;
        while (*p) {
            h = (h ^ (uint8_t)*p++) * 16777619u;
        }
        h = (h ^ 0xFFu) * 16777619u;
    }
    return h;
}

static void queue_save(void)
{
    if (ui.queue_count == 0) return;

    uint32_t sig = queue_signature();
    if (s_queue_saved_valid && sig == s_queue_saved_sig) {
        return; /* same folder/playlist content; no flash write */
    }

    FILE *fp = fopen(QUEUE_FILE, "wb");
    if (!fp) {
        ESP_LOGW(TAG, "queue save open failed");
        return;
    }
    queue_header_t h = {
        .magic = QUEUE_MAGIC,
        .version = 1,
        .count = (uint32_t)ui.queue_count,
    };
    bool ok = fwrite(&h, 1, sizeof(h), fp) == sizeof(h);
    for (size_t i = 0; ok && i < ui.queue_count; i++) {
        char path[512] = {0};
        snprintf(path, sizeof(path), "%s", ui.queue[i].path);
        ok = fwrite(path, 1, sizeof(path), fp) == sizeof(path);
    }
    fclose(fp);
    if (!ok) {
        ESP_LOGW(TAG, "queue save failed");
        return;
    }
    s_queue_saved_sig = sig;
    s_queue_saved_valid = true;
    ESP_LOGI(TAG, "queue saved to littlefs: %u tracks",
             (unsigned)ui.queue_count);
}

static void queue_load(void)
{
    if (!ui.queue) return;
    FILE *fp = fopen(QUEUE_FILE, "rb");
    if (!fp) return;

    queue_header_t h;
    if (fread(&h, 1, sizeof(h), fp) != sizeof(h) ||
        h.magic != QUEUE_MAGIC || h.version != 1 ||
        h.count > MAX_QUEUE) {
        fclose(fp);
        return;
    }

    ui.queue_count = 0;
    ui.queue_index = 0;
    for (uint32_t i = 0; i < h.count; i++) {
        char path[512];
        if (fread(path, 1, sizeof(path), fp) != sizeof(path)) break;
        path[sizeof(path) - 1] = '\0';
        if (path[0] != '/') continue;

        pod_dir_entry_t *e = &ui.queue[ui.queue_count];
        memset(e, 0, sizeof(*e));
        const char *name = strrchr(path, '/');
        snprintf(e->name, sizeof(e->name), "%.*s",
                 (int)(sizeof(e->name) - 1), name ? name + 1 : path);
        snprintf(e->path, sizeof(e->path), "%s", path);
        e->is_audio = true;
        ui.queue_count++;
    }
    fclose(fp);

    if (ui.now_path[0]) {
        for (size_t i = 0; i < ui.queue_count; i++) {
            if (strcmp(ui.queue[i].path, ui.now_path) == 0) {
                ui.queue_index = i;
                break;
            }
        }
    }
    s_queue_saved_sig = queue_signature();
    s_queue_saved_valid = ui.queue_count > 0;
    ESP_LOGI(TAG, "queue restored from littlefs: %u tracks, index=%u",
             (unsigned)ui.queue_count, (unsigned)ui.queue_index);
}

static void queue_from_entries(void)
{
    ui.queue_count = 0;
    if (!ui.queue) return;
    for (size_t i = 0; i < ui.entry_count && ui.queue_count < MAX_QUEUE; i++) {
        if (ui.entries[i].is_audio) {
            ui.queue[ui.queue_count++] = ui.entries[i];
        }
    }
    queue_save();
}

#define MAX_LIB_SOURCES 16
#define LIB_SOURCES_FILE POD_STORAGE_USER_DATA "/library_sources.bin"
#define LIB_SOURCES_MAGIC 0x4C494252u /* LIBR */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t reserved;
} lib_sources_header_t;

static char (*s_lib_sources)[512];
static size_t s_lib_count;

static void library_sources_save(void)
{
    if (!s_lib_sources) return;
    FILE *fp = fopen(LIB_SOURCES_FILE, "wb");
    if (!fp) {
        ESP_LOGW(TAG, "library sources save open failed");
        return;
    }
    lib_sources_header_t h = {
        .magic = LIB_SOURCES_MAGIC,
        .version = 1,
        .count = (uint32_t)s_lib_count,
    };
    bool ok = fwrite(&h, 1, sizeof(h), fp) == sizeof(h);
    for (size_t i = 0; ok && i < s_lib_count; i++) {
        ok = fwrite(s_lib_sources[i], 1, 512, fp) == 512;
    }
    fclose(fp);
    if (ok) {
        ESP_LOGI(TAG, "library sources saved: %u", (unsigned)s_lib_count);
    }
}

static bool library_sources_add_path(const char *path, bool persist)
{
    if (!path || path[0] != '/' || !s_lib_sources) return false;
    if (s_lib_count >= MAX_LIB_SOURCES) return false;
    for (size_t i = 0; i < s_lib_count; i++) {
        if (strcmp(s_lib_sources[i], path) == 0) return false;
    }

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGW(TAG, "library source is not a directory: %s", path);
        return false;
    }

    snprintf(s_lib_sources[s_lib_count], 512, "%s", path);
    s_lib_count++;
    if (persist) library_sources_save();
    ESP_LOGI(TAG, "library source added: %s", path);
    return true;
}

static void library_sources_init(void)
{
    if (!s_lib_sources) {
        s_lib_sources = heap_caps_malloc(MAX_LIB_SOURCES * 512,
                                         MALLOC_CAP_SPIRAM);
    }
    if (!s_lib_sources) return;
    s_lib_count = 0;

    FILE *fp = fopen(LIB_SOURCES_FILE, "rb");
    if (fp) {
        lib_sources_header_t h;
        if (fread(&h, 1, sizeof(h), fp) == sizeof(h) &&
            h.magic == LIB_SOURCES_MAGIC && h.version == 1 &&
            h.count <= MAX_LIB_SOURCES) {
            for (uint32_t i = 0; i < h.count; i++) {
                char path[512];
                if (fread(path, 1, sizeof(path), fp) != sizeof(path)) break;
                path[sizeof(path) - 1] = '\0';
                if (path[0] == '/') {
                    snprintf(s_lib_sources[s_lib_count], 512, "%s", path);
                    s_lib_count++;
                }
            }
        }
        fclose(fp);
    }

    if (s_lib_count == 0) {
        snprintf(s_lib_sources[0], 512, "%s", pod_storage_music_root());
        s_lib_count = 1;
        library_sources_save();
    }
    ESP_LOGI(TAG, "library sources loaded: %u", (unsigned)s_lib_count);
}

static void update_now_static(void);
static void repeat_apply_visual(void);
static void apply_cover_background(void);
static void now_lyrics_visual(void);
static void build_lyrics_panel(void);
static void lyrics_sync_update(void);
static void lyrics_anim_stop(void);
static void lyrics_parsed_clear(void);
static void lyrics_parse(void);
static void library_add_current_cb(lv_event_t *event);
static void library_add_cancel_cb(lv_event_t *event);

typedef struct {
    FILE *fp;
    uint8_t *out;
    uint16_t w;
    uint16_t h;
} jpeg_io_t;

static size_t jpeg_in_cb(JDEC *jd, uint8_t *buff, size_t ndata)
{
    jpeg_io_t *io = (jpeg_io_t *)jd->device;
    if (!io || !io->fp) return 0;
    if (buff) return fread(buff, 1, ndata, io->fp);
    if (fseek(io->fp, (long)ndata, SEEK_CUR) != 0) return 0;
    return ndata;
}

static int jpeg_out_cb(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_io_t *io = (jpeg_io_t *)jd->device;
    if (!io || !io->out) return 0;

    uint16_t w = (uint16_t)(rect->right - rect->left + 1);
    uint16_t h = (uint16_t)(rect->bottom - rect->top + 1);
    if ((uint32_t)rect->left + w > io->w ||
        (uint32_t)rect->top + h > io->h) {
        return 0;
    }

    const uint8_t *src = (const uint8_t *)bitmap;
    for (uint16_t y = 0; y < h; y++) {
        uint8_t *dst = io->out + ((size_t)(rect->top + y) * io->w +
                                   rect->left) * 3;
        /* TJPGD already emits B,G,R, matching LVGL's RGB888 memory order. */
        memcpy(dst, src + (size_t)y * w * 3, (size_t)w * 3);
    }
    return 1;
}

static void fit_dim(uint16_t sw, uint16_t sh, uint16_t max_w, uint16_t max_h,
                    uint16_t *dw, uint16_t *dh)
{
    if (!sw || !sh) {
        *dw = *dh = 0;
        return;
    }
    uint32_t w = sw, h = sh;
    if (w > max_w) {
        h = (uint32_t)h * max_w / w;
        w = max_w;
    }
    if (h > max_h) {
        w = (uint32_t)w * max_h / h;
        h = max_h;
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    *dw = (uint16_t)w;
    *dh = (uint16_t)h;
}

static void downscale_bgr(const uint8_t *src, uint16_t sw, uint16_t sh,
                          uint8_t *dst, uint16_t dw, uint16_t dh)
{
    for (uint16_t y = 0; y < dh; y++) {
        uint32_t sy0 = (uint32_t)y * sh / dh;
        uint32_t sy1 = (uint32_t)(y + 1) * sh / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (uint16_t x = 0; x < dw; x++) {
            uint32_t sx0 = (uint32_t)x * sw / dw;
            uint32_t sx1 = (uint32_t)(x + 1) * sw / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint32_t b = 0, g = 0, r = 0;
            uint32_t n = 0;
            for (uint32_t sy = sy0; sy < sy1; sy++) {
                const uint8_t *row = src + ((size_t)sy * sw + sx0) * 3;
                for (uint32_t sx = sx0; sx < sx1; sx++) {
                    b += row[0];
                    g += row[1];
                    r += row[2];
                    row += 3;
                    n++;
                }
            }
            uint8_t *out = dst + ((size_t)y * dw + x) * 3;
            out[0] = (uint8_t)(b / n);
            out[1] = (uint8_t)(g / n);
            out[2] = (uint8_t)(r / n);
        }
    }
}

static void cover_theme_colors(const uint8_t *data, uint16_t w, uint16_t h,
                               uint32_t *out1, uint32_t *out2)
{
    uint64_t sr = 0, sg = 0, sb = 0, sw = 0;
    for (uint16_t y = 0; y < h; y += 4) {
        const uint8_t *row = data + (size_t)y * w * 3;
        for (uint16_t x = 0; x < w; x += 4) {
            const uint8_t *p = row + (size_t)x * 3; /* B,G,R */
            int b = p[0], g = p[1], r = p[2];
            int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
            int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
            uint32_t weight = (uint32_t)(mx - mn) + 8; /* prefer coloured pixels */
            sr += (uint64_t)r * weight;
            sg += (uint64_t)g * weight;
            sb += (uint64_t)b * weight;
            sw += weight;
        }
    }

    if (!sw) {
        *out1 = C_BG;
        *out2 = 0x151318;
        return;
    }

    int r = (int)(sr / sw);
    int g = (int)(sg / sw);
    int b = (int)(sb / sw);

    /* Keep the artwork readable: tint the existing near-black theme instead
     * of flooding the page with a bright album colour. */
    int r1 = (r * 38 + 0x0B * 62) / 100;
    int g1 = (g * 38 + 0x0B * 62) / 100;
    int b1 = (b * 38 + 0x0C * 62) / 100;
    int r2 = r1 * 55 / 100;
    int g2 = g1 * 55 / 100;
    int b2 = b1 * 55 / 100;

    *out1 = ((uint32_t)r1 << 16) | ((uint32_t)g1 << 8) | (uint32_t)b1;
    *out2 = ((uint32_t)r2 << 16) | ((uint32_t)g2 << 8) | (uint32_t)b2;
}

/* Decode the cover on the metadata worker, then box-filter it to exactly
 * COVER_SIZE x COVER_SIZE.  The UI receives a plain RGB888 image, so it never
 * runs a transformed layer or touches the SD card while audio is playing. */
static uint8_t *decode_cover_jpeg(const char *path, uint16_t *w_out,
                                  uint16_t *h_out, size_t *size_out,
                                  uint8_t **large_out, uint16_t *lw_out,
                                  uint16_t *lh_out, size_t *lsize_out)
{
    if (large_out) *large_out = NULL;
    if (lw_out) *lw_out = 0;
    if (lh_out) *lh_out = 0;
    if (lsize_out) *lsize_out = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    void *work = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!work) work = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!work) {
        fclose(fp);
        return NULL;
    }

    jpeg_io_t io = {.fp = fp};
    JDEC jd;
    JRESULT rc = jd_prepare(&jd, jpeg_in_cb, work, 4096, &io);
    if (rc != JDR_OK || jd.width == 0 || jd.height == 0) {
        heap_caps_free(work);
        fclose(fp);
        return NULL;
    }

    uint16_t src_w = jd.width;
    uint16_t src_h = jd.height;
    /* Decode full-size: TJPGD's 1/2 scaling path only adjusts the output
     * rectangle, not the MCU bitmap, so it produces a tiled/sheared image. */
    uint16_t dec_w = src_w;
    uint16_t dec_h = src_h;
    uint8_t tjpgd_scale = 0;

    size_t dec_size = (size_t)dec_w * dec_h * 3;
    uint8_t *decoded = heap_caps_malloc(dec_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!decoded) {
        heap_caps_free(work);
        fclose(fp);
        return NULL;
    }

    io.out = decoded;
    io.w = dec_w;
    io.h = dec_h;
    rc = jd_decomp(&jd, jpeg_out_cb, tjpgd_scale);

    heap_caps_free(work);
    fclose(fp);

    if (rc != JDR_OK) {
        heap_caps_free(decoded);
        return NULL;
    }

    uint16_t normal_w, normal_h;
    fit_dim(src_w, src_h, COVER_SIZE, COVER_SIZE, &normal_w, &normal_h);
    size_t out_size = (size_t)normal_w * normal_h * 3;
    uint8_t *out = heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        heap_caps_free(decoded);
        return NULL;
    }
    downscale_bgr(decoded, dec_w, dec_h, out, normal_w, normal_h);

    /* Pure-cover source: fit inside 460x460, preserving aspect ratio.  Never
     * upscale smaller artwork (that was the source of the blur). */
    uint16_t large_w, large_h;
    fit_dim(src_w, src_h, UI_W, UI_H, &large_w, &large_h);
    uint8_t *large = decoded;
    size_t large_size = dec_size;
    if (large_w != src_w || large_h != src_h) {
        size_t fitted_size = (size_t)large_w * large_h * 3;
        uint8_t *fitted = heap_caps_malloc(fitted_size,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (fitted) {
            downscale_bgr(decoded, src_w, src_h, fitted, large_w, large_h);
            heap_caps_free(decoded);
            large = fitted;
            large_size = fitted_size;
        } else {
            large_w = src_w;
            large_h = src_h;
        }
    }

    *w_out = normal_w;
    *h_out = normal_h;
    *size_out = out_size;
    if (large_out) *large_out = large;
    else heap_caps_free(large);
    if (lw_out) *lw_out = large_w;
    if (lh_out) *lh_out = large_h;
    if (lsize_out) *lsize_out = large_size;

    ESP_LOGI(TAG, "cover decoded: %ux%u -> %ux%u normal + %ux%u large, %u bytes",
             src_w, src_h, normal_w, normal_h, large_w, large_h,
             (unsigned)out_size);
    return out;
}

static void apply_ready_metadata(void)
{
    if (!s_meta_ready || !s_meta_lock) return;
    if (xSemaphoreTake(s_meta_lock, 0) != pdTRUE) return;

    pod_metadata_t meta = s_meta_result;
    uint32_t seq = s_meta_seq;
    char *lyrics = s_meta_result.lyrics;
    s_meta_result.lyrics = NULL;
    s_meta_result.lyrics_len = 0;
    uint8_t *cover = s_meta_cover;
    uint16_t cover_w = s_meta_cover_w;
    uint16_t cover_h = s_meta_cover_h;
    size_t cover_size = s_meta_cover_size;
    uint8_t *cover_large = s_meta_cover_large;
    uint16_t cover_lw = s_meta_cover_lw;
    uint16_t cover_lh = s_meta_cover_lh;
    size_t cover_lsize = s_meta_cover_lsize;
    uint32_t cover_bg1 = s_meta_cover_bg1;
    uint32_t cover_bg2 = s_meta_cover_bg2;
    s_meta_cover = NULL;
    s_meta_cover_w = s_meta_cover_h = 0;
    s_meta_cover_size = 0;
    s_meta_cover_large = NULL;
    s_meta_cover_lw = s_meta_cover_lh = 0;
    s_meta_cover_lsize = 0;
    s_meta_cover_bg1 = C_BG;
    s_meta_cover_bg2 = 0x151318;
    s_meta_ready = false;
    xSemaphoreGive(s_meta_lock);

    if (seq != s_play_seq) { /* a newer track was requested */
        if (cover) heap_caps_free(cover);
        if (cover_large) heap_caps_free(cover_large);
        if (lyrics) heap_caps_free(lyrics);
        return;
    }

    if (meta.title[0]) {
        snprintf(ui.now_title, sizeof(ui.now_title), "%s", meta.title);
        ui.has_meta = true;
    }
    if (meta.artist[0]) {
        snprintf(ui.now_artist, sizeof(ui.now_artist), "%s", meta.artist);
        ui.has_meta = true;
    }
    if (meta.album[0]) {
        snprintf(ui.now_album, sizeof(ui.now_album), "%s", meta.album);
        ui.has_meta = true;
    }

    if (ui.np_lyrics_text) lv_label_set_text(ui.np_lyrics_text, "");
    if (ui.lyrics) heap_caps_free(ui.lyrics);
    if (s_lyrics_display) {
        heap_caps_free(s_lyrics_display);
        s_lyrics_display = NULL;
    }
    ui.lyrics = lyrics;
    ui.lyrics_len = lyrics ? meta.lyrics_len : 0;
    ui.has_lyrics = lyrics != NULL && ui.lyrics_len > 0;
    lyrics_parsed_clear();
    if (ui.has_lyrics) lyrics_parse();
    if (!ui.has_lyrics) ui.lyrics_page = false;

    if (ui.np_cover_img) lv_image_set_src(ui.np_cover_img, NULL);
    cover_retire(ui.cover_data);
    cover_retire(ui.cover_large_data);
    ui.cover_data = cover;
    ui.cover_w = cover_w;
    ui.cover_h = cover_h;
    ui.cover_size = cover_size;
    ui.cover_large_data = cover_large;
    ui.cover_large_w = cover_lw;
    ui.cover_large_h = cover_lh;
    ui.cover_large_size = cover_lsize;
    if (cover) {
        ui.cover_bg1 = cover_bg1;
        ui.cover_bg2 = cover_bg2;
    }
    ui.has_cover = cover != NULL;
    if (meta.cover_path[0]) {
        snprintf(ui.cover_path, sizeof(ui.cover_path), "S:%s", meta.cover_path);
    }

    ESP_LOGI(TAG, "metadata: title=%s artist=%s album=%s cover=%s raw=%" PRIu32
             "x%" PRIu32 " bg=%06" PRIx32 "/%06" PRIx32,
             ui.now_title, ui.now_artist, ui.now_album,
             ui.cover_path, cover_w, cover_h, cover_bg1, cover_bg2);
    apply_cover_background();
    if (ui.tab == 0 && ui.lyrics_page) pod_ui_rebuild();
    else if (ui.cover_only || ui.tab == 3) pod_ui_rebuild();
    else update_now_static();
}

static uint8_t *decode_probe_cover(uint8_t *data, size_t len,
                                   uint16_t *w_out, uint16_t *h_out,
                                   size_t *size_out, uint8_t **large_out,
                                   uint16_t *lw_out, uint16_t *lh_out,
                                   size_t *lsize_out)
{
    if (!data || len == 0) return NULL;

    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s/probe_cover.jpg", POD_STORAGE_CACHE_ROOT);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return NULL;
    size_t written = fwrite(data, 1, len, fp);
    fclose(fp);
    if (written != len) {
        (void)remove(tmp);
        return NULL;
    }

    uint8_t *cover = decode_cover_jpeg(tmp, w_out, h_out, size_out,
                                       large_out, lw_out, lh_out, lsize_out);
    (void)remove(tmp);
    return cover;
}

static void play_worker_task(void *arg)
{
    (void)arg;
    play_req_t req;
    for (;;) {
        if (xQueueReceive(s_play_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* Stop the old stream before reading metadata/cover from the same SD
         * card.  Starting the new decoder after those reads means UI-driven
         * I/O can never starve the audio decode task. */
        (void)pod_audio_stop();

        pod_metadata_t meta = {0};
        esp_err_t probe_err = pod_audio_probe(req.path);
        bool ok = probe_err == ESP_OK;
        const pod_audio_metadata_t *pm = pod_audio_probe_meta();
        if (ok && pm) {
            snprintf(meta.title, sizeof(meta.title), "%s", pm->title);
            snprintf(meta.artist, sizeof(meta.artist), "%s", pm->artist);
            snprintf(meta.album, sizeof(meta.album), "%s", pm->album);
            if (pm->lyrics && pm->lyrics_len) {
                meta.lyrics = heap_caps_malloc(pm->lyrics_len + 1,
                                               MALLOC_CAP_SPIRAM);
                if (meta.lyrics) {
                    memcpy(meta.lyrics, pm->lyrics, pm->lyrics_len);
                    meta.lyrics[pm->lyrics_len] = '\0';
                    meta.lyrics_len = pm->lyrics_len;
                }
            }
        }

        uint8_t *cover = NULL;
        uint16_t cover_w = 0, cover_h = 0;
        size_t cover_size = 0;
        uint8_t *cover_large = NULL;
        uint16_t cover_lw = 0, cover_lh = 0;
        size_t cover_lsize = 0;
        uint32_t cover_bg1 = C_BG, cover_bg2 = 0x151318;
        if (ok && pm && pm->cover && pm->cover_len) {
            cover = decode_probe_cover(pm->cover, pm->cover_len,
                                       &cover_w, &cover_h, &cover_size,
                                       &cover_large, &cover_lw, &cover_lh,
                                       &cover_lsize);
            if (cover) {
                cover_theme_colors(cover, cover_w, cover_h,
                                   &cover_bg1, &cover_bg2);
            }
        }
        pod_audio_probe_clear();

        /* If the user picked another track while metadata was being read,
         * do not stop the current decoder for this stale request. */
        play_req_t pending;
        if (xQueuePeek(s_play_queue, &pending, 0) == pdTRUE &&
            pending.seq != req.seq) {
            if (cover) heap_caps_free(cover);
            if (cover_large) heap_caps_free(cover_large);
            if (meta.lyrics) heap_caps_free(meta.lyrics);
            continue;
        }

        if (ok && s_meta_lock) {
            xSemaphoreTake(s_meta_lock, portMAX_DELAY);
            if (s_meta_cover) heap_caps_free(s_meta_cover);
            if (s_meta_cover_large) heap_caps_free(s_meta_cover_large);
            if (s_meta_result.lyrics) heap_caps_free(s_meta_result.lyrics);
            s_meta_result = meta;
            s_meta_seq = req.seq;
            s_meta_cover = cover;
            s_meta_cover_w = cover_w;
            s_meta_cover_h = cover_h;
            s_meta_cover_size = cover_size;
            s_meta_cover_large = cover_large;
            s_meta_cover_lw = cover_lw;
            s_meta_cover_lh = cover_lh;
            s_meta_cover_lsize = cover_lsize;
            s_meta_cover_bg1 = cover_bg1;
            s_meta_cover_bg2 = cover_bg2;
            s_meta_ready = true;
            xSemaphoreGive(s_meta_lock);
        } else {
            if (cover) heap_caps_free(cover);
            if (cover_large) heap_caps_free(cover_large);
            if (meta.lyrics) heap_caps_free(meta.lyrics);
        }

        /* Both data files are persisted only at safe points: the old stream
         * has already been stopped and the new one has not started yet. */
        if (ui.song_stats_enabled) {
            song_stats_note_play(req.path);
        }
        if (autosave_due()) {
            playback_save_fields(req.path, meta.title, meta.artist, meta.album,
                                 ui.play_count);
            if (ui.song_stats_enabled) song_stats_save();
            autosave_mark();
        }

        esp_err_t err = pod_audio_play(req.path);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "play %s: %s", req.path, esp_err_to_name(err));
        }
    }
}

static void stop_for_track_change(void)
{
    pod_audio_state_t old_state = pod_audio_state();
    if (old_state == POD_AUDIO_PLAYING || old_state == POD_AUDIO_PAUSED) {
        (void)pod_audio_stop();
    }
}

static void start_play_path(const char *path)
{
    if (!path || !path[0]) return;
    pod_audio_clear_eos();
    /* Stop the old decoder before touching metadata/cover.  Pausing it would
     * keep its SD read path and decode task alive while probe/cover reads the
     * same card, which causes audible decode stalls. */
    stop_for_track_change();

    snprintf(ui.now_path, sizeof(ui.now_path), "%s", path);
    ui.play_count++;
    ui.play_dirty = true;
    ui.last_play_change_us = esp_timer_get_time();

    if (ui.np_cover_img) lv_image_set_src(ui.np_cover_img, NULL);
    cover_retire(ui.cover_data);
    cover_retire(ui.cover_large_data);
    ui.cover_data = NULL;
    ui.cover_w = ui.cover_h = 0;
    ui.cover_size = 0;
    ui.cover_large_data = NULL;
    ui.cover_large_w = ui.cover_large_h = 0;
    ui.cover_large_size = 0;
    ui.has_meta = false;
    ui.has_cover = false;
    ui.has_lyrics = false;
    ui.lyrics_page = false;
    lyrics_anim_stop();
    if (ui.np_lyrics_text) lv_label_set_text(ui.np_lyrics_text, "");
    if (ui.lyrics) {
        heap_caps_free(ui.lyrics);
        ui.lyrics = NULL;
        ui.lyrics_len = 0;
    }
    if (s_lyrics_display) {
        heap_caps_free(s_lyrics_display);
        s_lyrics_display = NULL;
    }
    lyrics_parsed_clear();
    ui.cover_path[0] = '\0';
    title_from_path(ui.now_path, ui.now_title, sizeof(ui.now_title));
    parent_name(ui.now_path, ui.now_artist, sizeof(ui.now_artist));
    ui.now_album[0] = '\0';
    update_now_static();

    if (s_meta_lock) {
        xSemaphoreTake(s_meta_lock, portMAX_DELAY);
        if (s_meta_cover) heap_caps_free(s_meta_cover);
        if (s_meta_cover_large) heap_caps_free(s_meta_cover_large);
        s_meta_cover = NULL;
        s_meta_cover_w = s_meta_cover_h = 0;
        s_meta_cover_size = 0;
        s_meta_cover_large = NULL;
        s_meta_cover_lw = s_meta_cover_lh = 0;
        s_meta_cover_lsize = 0;
        s_meta_cover_bg1 = C_BG;
        s_meta_cover_bg2 = 0x151318;
        if (s_meta_result.lyrics) {
            heap_caps_free(s_meta_result.lyrics);
            s_meta_result.lyrics = NULL;
            s_meta_result.lyrics_len = 0;
        }
        s_meta_ready = false;
        xSemaphoreGive(s_meta_lock);
    }

    if (!s_play_queue) {
        ESP_LOGE(TAG, "play queue unavailable");
        return;
    }

    play_req_t req;
    snprintf(req.path, sizeof(req.path), "%s", path);
    req.seq = ++s_play_seq;
    if (xQueueOverwrite(s_play_queue, &req) != pdTRUE) {
        ESP_LOGW(TAG, "play request dropped");
    }
}

static void play_queue_index(size_t index)
{
    if (index >= ui.queue_count) return;
    ui.queue_index = index;
    start_play_path(ui.queue[index].path);
}

static void goto_now(void);

static void play_file_from_current(const pod_dir_entry_t *entry)
{
    stop_for_track_change();
    queue_from_entries();
    size_t index = 0;
    for (size_t i = 0; i < ui.queue_count; i++) {
        if (strcmp(ui.queue[i].path, entry->path) == 0) {
            index = i;
            break;
        }
    }
    play_queue_index(index);
    goto_now();
}

/* -------------------------------------------------------------------------- */
/* Chrome: top bar + bottom nav                                               */
/* -------------------------------------------------------------------------- */

static void set_tab_visual(int tab)
{
    static const uint32_t active = C_ACCENT;
    static const uint32_t idle = C_MUTED;
    for (int i = 0; i < 5; i++) {
        bool on = (i == tab);
        set_label_font(ui.nav_icon[i], &lv_font_montserrat_24, on ? active : idle);
        set_label_font(ui.nav_text[i], &lv_font_montserrat_12, on ? active : idle);
        if (ui.nav_line[i]) {
            lv_obj_set_style_bg_opa(ui.nav_line[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_color(ui.nav_line[i], col(active), 0);
        }
    }
}

static void update_touch_state(void)
{
    if (!ui.indev) return;
    /* Touch is meaningful only when the screen is on, and in pure-cover
     * mode only while the overlay menu is visible. */
    bool enable = !ui.screen_off && (!ui.cover_only || ui.nav_visible);
    if (enable == ui.touch_enabled) return;
    ui.touch_enabled = enable;
    lv_indev_enable(ui.indev, enable);
    if (!enable) lv_indev_reset(ui.indev, NULL);
    ESP_LOGI(TAG, "touch input %s", enable ? "enabled" : "suspended");
}

static void nav_apply_visibility(bool visible)
{
    if (!ui.nav) return;
    ui.nav_visible = visible;
    if (visible) {
        lv_obj_move_foreground(ui.nav);
        lv_obj_remove_flag(ui.nav, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui.nav, LV_OBJ_FLAG_HIDDEN);
    }
    /* The bottom bar is an overlay menu: showing it must not resize/reflow
     * the page.  This keeps the playback layout stable and preserves scroll
     * positions on Library/Files/Settings. */
    update_touch_state();
    ESP_LOGI(TAG, "bottom nav %s (overlay)", visible ? "shown" : "hidden");
}

static void nav_toggle(void)
{
    if (ui.queue_overlay) queue_overlay_close();
    nav_apply_visibility(!ui.nav_visible);
}

static void nav_clicked_cb(lv_event_t *event)
{
    int tab = (int)(intptr_t)lv_event_get_user_data(event);
    if (tab < 0 || tab > 4) return;
    nav_apply_visibility(false);
    bool was_cover_only = ui.cover_only;
    ui.cover_only = false;
    if (tab == ui.tab) {
        if (tab == 4 && ui.settings_page != 0) {
            ui.settings_page = 0;
            pod_ui_rebuild();
        } else if (was_cover_only) {
            pod_ui_rebuild();
        }
        return;
    }
    ui.tab = tab;
    if (tab == 1) ui.lib_root = true;
    if (tab == 4) ui.settings_page = 0;
    set_tab_visual(tab);
    pod_ui_rebuild();
}

static void screen_power(bool on)
{
    if (on) {
        if (!ui.screen_off) return;
        ui.screen_off = false;
        if (ui.panel) {
            (void)esp_lcd_panel_co5300_set_brightness(ui.panel, ui.brightness);
        }
        ui.last_activity_us = esp_timer_get_time();
        update_touch_state();
        ESP_LOGI(TAG, "screen on");
    } else {
        if (ui.screen_off) return;
        ui.screen_off = true;
        if (ui.panel) {
            (void)esp_lcd_panel_co5300_set_brightness(ui.panel, 0);
        }
        update_touch_state();
        ESP_LOGI(TAG, "screen off");
    }
}

static void cover_only_toggle(void)
{
    if (!ui.page || ui.screen_off) return;
    ui.cover_only = !ui.cover_only;
    if (ui.cover_only) {
        ui.tab = 0;
        set_tab_visual(0);
        nav_apply_visibility(false);
    }
    pod_ui_rebuild();
    ESP_LOGI(TAG, "cover-only %s", ui.cover_only ? "on" : "off");
}

static void indev_activity_cb(lv_event_t *event)
{
    (void)event;
    /* Touch is disabled while the screen is off or while the pure-cover page
     * has no menu open; this callback only feeds the idle timer. */
    ui.last_activity_us = esp_timer_get_time();
}

static void build_chrome(void)
{
    lv_obj_t *top = lv_obj_create(ui.screen);
    ui.top = top;
    lv_obj_set_size(top, UI_W, TOP_H);
    lv_obj_set_pos(top, 0, 0);
    panel_style(top, C_BG, 0);
    lv_obj_set_style_border_color(top, col(C_BORDER), 0);
    lv_obj_set_style_border_width(top, 1, 0);
    lv_obj_set_style_border_side(top, LV_BORDER_SIDE_BOTTOM, 0);

    lv_obj_t *brand = mk_label_static(top, "POD", F_SMALL, C_ACCENT);
    lv_obj_set_pos(brand, 16, 10);

    ui.top_title = mk_label_static(top, "NOW PLAYING", F_MICRO, C_MUTED);
    lv_obj_set_width(ui.top_title, 240);
    lv_obj_set_style_text_align(ui.top_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ui.top_title, LV_ALIGN_TOP_MID, 0, 13);
    lv_label_set_long_mode(ui.top_title, LV_LABEL_LONG_CLIP);

    ui.top_sd = mk_label_static(top, LV_SYMBOL_SD_CARD, F_SMALL, C_MUTED);
    lv_obj_align(ui.top_sd, LV_ALIGN_RIGHT_MID, -44, 0);

    ui.top_battery = mk_label_static(top, LV_SYMBOL_BATTERY_FULL, F_SMALL, C_MUTED);
    lv_obj_align(ui.top_battery, LV_ALIGN_RIGHT_MID, -16, 0);

    lv_obj_t *nav = lv_obj_create(ui.screen);
    ui.nav = nav;
    lv_obj_set_size(nav, UI_W, NAV_H);
    lv_obj_set_pos(nav, 0, UI_H - NAV_H);
    panel_style(nav, C_BG, 0);
    lv_obj_set_style_border_color(nav, col(C_BORDER), 0);
    lv_obj_set_style_border_width(nav, 1, 0);
    lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_TOP, 0);

    static const char *icons[5] = {
        LV_SYMBOL_PLAY, LV_SYMBOL_DIRECTORY, LV_SYMBOL_FILE,
        LV_SYMBOL_BARS, LV_SYMBOL_SETTINGS,
    };
    static const char *labels[5] = {"NOW", "LIBRARY", "FILES", "LISTEN", "SETTINGS"};

    for (int i = 0; i < 5; i++) {
        lv_obj_t *btn = lv_button_create(nav);
        lv_obj_set_size(btn, 92, NAV_H);
        lv_obj_set_pos(btn, i * 92, 0);
        button_style(btn, C_NO_BG, 0);
        lv_obj_set_style_pad_row(btn, 1, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(btn, nav_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        ui.nav_btn[i] = btn;
        ui.nav_icon[i] = mk_label_static(btn, icons[i], &lv_font_montserrat_24, C_MUTED);
        ui.nav_text[i] = mk_label_static(btn, labels[i], &lv_font_montserrat_12, C_MUTED);

        ui.nav_line[i] = lv_obj_create(btn);
        lv_obj_set_size(ui.nav_line[i], 22, 3);
        lv_obj_align(ui.nav_line[i], LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_add_flag(ui.nav_line[i], LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_style_radius(ui.nav_line[i], 2, 0);
        lv_obj_set_style_bg_color(ui.nav_line[i], col(C_ACCENT), 0);
        lv_obj_set_style_bg_opa(ui.nav_line[i], LV_OPA_TRANSP, 0);
        no_scroll(ui.nav_line[i]);
    }

    /* The bar is summoned with the BOOT button; the page uses the full
     * 422 px height until then. */
    lv_obj_add_flag(ui.nav, LV_OBJ_FLAG_HIDDEN);
}

/* -------------------------------------------------------------------------- */
/* Common page helpers                                                        */
/* -------------------------------------------------------------------------- */

static void page_begin(const char *title)
{
    lv_label_set_text_static(ui.top_title, title);
    s_list.list = NULL;
    s_list.content = NULL;
    s_list.first = (size_t)-1;
    s_eligible_count = 0;
    lv_obj_clean(ui.page);
    if (ui.entries) {
        pod_storage_free_entries(ui.entries);
        ui.entries = NULL;
    }
    ui.entry_count = 0;
    ui.np_title = NULL;
    ui.np_artist = NULL;
    ui.np_artist_box = NULL;
    ui.np_album = NULL;
    ui.np_album_box = NULL;
    ui.np_cover = NULL;
    ui.np_cover_initial = NULL;
    ui.np_cover_img = NULL;
    ui.np_slider = NULL;
    ui.np_slider_track = NULL;
    ui.np_cur = NULL;
    ui.np_dur = NULL;
    ui.np_controls = NULL;
    ui.np_play_icon = NULL;
    ui.np_shuffle = NULL;
    ui.np_repeat = NULL;
    ui.np_repeat_one = NULL;
    ui.np_format = NULL;
    ui.np_fav_img = NULL;
    ui.np_fav_btn = NULL;
    ui.np_lyrics_btn = NULL;
    ui.np_lyrics_label = NULL;
    ui.np_lyrics_panel = NULL;
    ui.np_lyrics_content = NULL;
    memset(ui.np_lyrics_lines, 0, sizeof(ui.np_lyrics_lines));
    ui.np_lyrics_text = NULL;
    ui.np_format_cache[0] = '\0';
    ui.seeking = false;
    ui.last_pos_s = -1;
    ui.last_dur_s = -1;
    ui.last_slider = -1;
    ui.last_state = (pod_audio_state_t)-1;
    ui.error_shown = false;
    ui.last_error[0] = '\0';
    if (ui.top) {
        lv_obj_remove_flag(ui.top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(ui.top, col(C_BG), 0);
    }
    if (ui.nav) {
        lv_obj_set_style_bg_color(ui.nav, col(C_BG), 0);
    }
    lv_obj_set_pos(ui.page, 0, TOP_H);
    lv_obj_set_style_bg_opa(ui.page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_dir(ui.page, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_pad_top(ui.page, 8, 0);
    lv_obj_set_height(ui.page, ui.page_h);
    lv_obj_add_flag(ui.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_scroll_to_y(ui.page, 0, LV_ANIM_OFF);
}

static void empty_close_cb(lv_event_t *event)
{
    lv_obj_t *box = (lv_obj_t *)lv_event_get_user_data(event);
    if (box) lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
}

static void empty_state(const char *title, const char *sub)
{
    lv_obj_t *box = lv_obj_create(ui.page);
    lv_obj_set_size(box, 400, 150);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 52);
    card_style(box, C_SURFACE, 22);

    lv_obj_t *icon = mk_label(box, LV_SYMBOL_DIRECTORY, &lv_font_montserrat_28, C_ACCENT);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *t = mk_label(box, title, F_BODY_CN, C_TEXT);
    lv_obj_set_width(t, 340);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_t *s = mk_label(box, sub, F_MICRO, C_MUTED);
    lv_obj_set_width(s, 340);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 94);

    lv_obj_t *close = lv_button_create(box);
    lv_obj_set_size(close, 36, 36);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -8, 8);
    button_style(close, C_NO_BG, LV_RADIUS_CIRCLE);
    lv_obj_add_event_cb(close, empty_close_cb, LV_EVENT_CLICKED, box);
    lv_obj_t *x = mk_label(close, LV_SYMBOL_CLOSE, F_SMALL, C_MUTED);
    lv_obj_center(x);
}

static void path_label(const char *path)
{
    lv_obj_t *p = mk_label(ui.page, path, F_SMALL_CN, C_MUTED);
    lv_obj_set_width(p, 428);
    lv_label_set_long_mode(p, LV_LABEL_LONG_DOT);
    lv_obj_align(p, LV_ALIGN_TOP_MID, 0, 0);
}

static void browser_parent_cb(lv_event_t *event);
static void list_row_clicked_cb(lv_event_t *event);
static void list_bind_row(list_row_t *row, size_t index);
static void list_rebuild_window(size_t first);
static void list_scroll_cb(lv_event_t *event);

static void list_bind_row(list_row_t *row, size_t index)
{
    if (!row || !row->row) return;
    if (index >= s_eligible_count) {
        lv_obj_add_flag(row->row, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    pod_dir_entry_t *e = &ui.entries[s_eligible[index]];
    row->index = index;
    lv_obj_remove_flag(row->row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(row->row, 0, (int32_t)(index * LIST_ROW_H));

    lv_label_set_text_static(row->icon,
                             e->is_dir ? LV_SYMBOL_DIRECTORY :
                             e->is_audio ? LV_SYMBOL_AUDIO : LV_SYMBOL_FILE);
    lv_obj_set_style_text_color(row->icon,
                                col(e->is_dir ? C_ACCENT2 :
                                    e->is_audio ? C_ACCENT : C_MUTED), 0);

    if (e->is_dir) {
        strncpy(row->name_buf, e->name, sizeof(row->name_buf) - 1);
        row->name_buf[sizeof(row->name_buf) - 1] = '\0';
    } else {
        title_from_path(e->name, row->name_buf, sizeof(row->name_buf));
    }
    lv_label_set_text_static(row->name, row->name_buf);

    if (e->is_dir) {
        snprintf(row->meta_buf, sizeof(row->meta_buf), "Folder");
    } else {
        char size[24];
        format_size(e->size, size, sizeof(size));
        const char *dot = strrchr(e->name, '.');
        snprintf(row->meta_buf, sizeof(row->meta_buf), "%s  %s",
                 dot ? dot + 1 : "AUDIO", size);
    }
    lv_label_set_text_static(row->meta, row->meta_buf);
    lv_label_set_text_static(row->chev,
                             e->is_dir ? LV_SYMBOL_RIGHT :
                             e->is_audio ? LV_SYMBOL_PLAY : "");
}

static void list_rebuild_window(size_t first)
{
    if (!s_list.list) return;

    size_t count = s_list.count;
    size_t max_first = count > LIST_POOL ? count - LIST_POOL : 0;
    if (first > max_first) first = max_first;
    s_list.first = first;

    for (size_t i = 0; i < LIST_POOL; i++) {
        list_bind_row(&s_list.rows[i], first + i);
    }
}

static void list_scroll_cb(lv_event_t *event)
{
    if (!s_list.list || lv_event_get_target(event) != s_list.list) return;
    int32_t y = lv_obj_get_scroll_top(s_list.list);
    if (y < 0) y = 0;
    size_t first = (size_t)y / LIST_ROW_H;
    if (first != s_list.first) {
        list_rebuild_window(first);
    }
}

static void list_row_clicked_cb(lv_event_t *event)
{
    list_row_t *row = (list_row_t *)lv_event_get_user_data(event);
    if (!row || row->index >= s_eligible_count) return;
    pod_dir_entry_t *e = &ui.entries[s_eligible[row->index]];

    if (e->is_dir) {
        if (ui.tab == 1) {
            snprintf(ui.lib_cwd, sizeof(ui.lib_cwd), "%s", e->path);
        } else {
            snprintf(ui.files_cwd, sizeof(ui.files_cwd), "%s", e->path);
            ui.files_root = false;
        }
        pod_ui_rebuild();
        return;
    }

    if (e->is_audio) {
        play_file_from_current(e);
    }
}

static void build_rows(const char *path, bool show_back, bool show_all)
{
    esp_err_t err = pod_storage_list_dir(path, &ui.entries, &ui.entry_count, MAX_ROWS);
    if (err != ESP_OK) {
        empty_state("Cannot open folder", "Check the SD card or filesystem.");
        return;
    }

    path_label(path);
    int top = 26;
    if (show_back) {
        lv_obj_t *back = lv_button_create(ui.page);
        lv_obj_set_size(back, 428, 52);
        lv_obj_set_pos(back, 16, 24);
        card_style(back, C_SURFACE, 14);
        lv_obj_add_event_cb(back, browser_parent_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *bi = mk_label(back, LV_SYMBOL_LEFT, &lv_font_montserrat_20, C_ACCENT);
        lv_obj_align(bi, LV_ALIGN_LEFT_MID, 14, 0);
        lv_obj_t *bt = mk_label(back, "Back", F_BODY, C_TEXT);
        lv_obj_align(bt, LV_ALIGN_LEFT_MID, 46, 0);
        top = 82;
    }

    if (ui.library_add_mode && ui.tab == 2) {
        lv_obj_t *add = lv_button_create(ui.page);
        lv_obj_set_size(add, 300, 52);
        lv_obj_set_pos(add, 16, 82);
        button_style(add, C_ACCENT, 14);
        lv_obj_add_event_cb(add, library_add_current_cb,
                            LV_EVENT_CLICKED, NULL);
        lv_obj_t *al = mk_label(add, "ADD THIS FOLDER", F_SMALL, 0x0B0B0C);
        lv_obj_center(al);

        lv_obj_t *cancel = lv_button_create(ui.page);
        lv_obj_set_size(cancel, 112, 52);
        lv_obj_set_pos(cancel, 324, 82);
        button_style(cancel, C_SURFACE, 14);
        lv_obj_add_event_cb(cancel, library_add_cancel_cb,
                            LV_EVENT_CLICKED, NULL);
        lv_obj_t *cl = mk_label(cancel, "CANCEL", F_MICRO, C_TEXT);
        lv_obj_center(cl);
        top = 142;
    }

    s_eligible_count = 0;
    for (size_t i = 0; i < ui.entry_count && s_eligible_count < MAX_ROWS; i++) {
        if (ui.entries[i].is_dir || show_all || ui.entries[i].is_audio) {
            s_eligible[s_eligible_count++] = (uint16_t)i;
        }
    }
    ESP_LOGI(TAG, "list %s: %zu entries, %zu eligible (all=%d)", path,
             ui.entry_count, s_eligible_count, show_all);
    if (s_eligible_count == 0) {
        if (show_all) {
            empty_state("Folder is empty", "No files or subfolders.");
        } else {
            empty_state("No music here", "Put supported audio files in this folder.");
        }
        return;
    }

    lv_obj_t *list = lv_obj_create(ui.page);
    lv_obj_set_size(list, 428, ui.page_h - 8 - 16 - top);
    lv_obj_set_pos(list, 16, top);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *content = lv_obj_create(list);
    lv_obj_set_size(content, 428, s_eligible_count * LIST_ROW_H);
    lv_obj_set_pos(content, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    no_scroll(content);

    s_list.list = list;
    s_list.content = content;
    s_list.count = s_eligible_count;
    s_list.first = (size_t)-1;

    for (size_t i = 0; i < LIST_POOL; i++) {
        list_row_t *row = &s_list.rows[i];
        row->row = lv_obj_create(content);
        lv_obj_set_size(row->row, 428, 60);
        lv_obj_set_pos(row->row, 0, 0);
        card_style(row->row, C_SURFACE, 14);
        lv_obj_add_flag(row->row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row->row, list_row_clicked_cb,
                            LV_EVENT_CLICKED, row);

        row->icon = mk_label(row->row, LV_SYMBOL_DIRECTORY,
                             &lv_font_montserrat_20, C_ACCENT2);
        lv_obj_set_pos(row->icon, 14, 13);

        row->name = mk_label(row->row, "", F_BODY_CN, C_TEXT);
        lv_obj_set_width(row->name, 310);
        lv_label_set_long_mode(row->name, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(row->name, 52, 9);

        row->meta = mk_label(row->row, "", F_MICRO, C_MUTED);
        lv_obj_set_pos(row->meta, 52, 35);

        row->chev = mk_label(row->row, LV_SYMBOL_RIGHT, F_BODY, C_MUTED);
        lv_obj_align(row->chev, LV_ALIGN_RIGHT_MID, -14, 0);
    }

    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    lv_obj_add_event_cb(list, list_scroll_cb, LV_EVENT_SCROLL, NULL);
    list_rebuild_window(0);
}

static void browser_parent_cb(lv_event_t *event)
{
    (void)event;
    char *path = (ui.tab == 1) ? ui.lib_cwd : ui.files_cwd;
    if (ui.tab == 1 && strcmp(path, pod_storage_music_root()) == 0) {
        ui.lib_root = true;
        pod_ui_rebuild();
        return;
    }
    if (ui.tab == 2 && ui.files_root) {
        return;
    }
    if (ui.tab == 2 && ui.library_add_mode &&
        strcmp(path, "/sdcard") == 0) {
        library_add_cancel_cb(NULL);
        return;
    }
    if (ui.tab == 2 &&
        (strcmp(path, "/sdcard") == 0 || strcmp(path, "/littlefs") == 0)) {
        ui.files_root = true;
        pod_ui_rebuild();
        return;
    }
    char *slash = strrchr(path, '/');
    if (!slash) return;
    if (slash == path) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    pod_ui_rebuild();
}

/* -------------------------------------------------------------------------- */
/* Library / Files                                                            */
/* -------------------------------------------------------------------------- */

static void library_source_cb(lv_event_t *event)
{
    const char *path = (const char *)lv_event_get_user_data(event);
    snprintf(ui.lib_cwd, sizeof(ui.lib_cwd), "%s", path);
    ui.lib_root = false;
    pod_ui_rebuild();
}

static void library_add_cb(lv_event_t *event)
{
    (void)event;
    ui.library_add_mode = true;
    ui.tab = 2;
    ui.files_root = false;
    snprintf(ui.files_cwd, sizeof(ui.files_cwd), "%s", "/sdcard");
    set_tab_visual(2);
    pod_ui_rebuild();
}

static void library_add_current_cb(lv_event_t *event)
{
    (void)event;
    if (ui.files_cwd[0]) {
        (void)library_sources_add_path(ui.files_cwd, true);
    }
    ui.library_add_mode = false;
    ui.tab = 1;
    ui.lib_root = true;
    ui.lib_cwd[0] = '\0';
    set_tab_visual(1);
    pod_ui_rebuild();
}

static void library_add_cancel_cb(lv_event_t *event)
{
    (void)event;
    ui.library_add_mode = false;
    ui.tab = 1;
    ui.lib_root = true;
    ui.lib_cwd[0] = '\0';
    set_tab_visual(1);
    pod_ui_rebuild();
}

static void favorites_back_cb(lv_event_t *event)
{
    (void)event;
    ui.lib_favorites = false;
    ui.lib_root = true;
    pod_ui_rebuild();
}

static void favorites_page_cb(lv_event_t *event)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(event);
    if (delta < 0 && ui.lib_fav_page > 0) {
        ui.lib_fav_page--;
    } else if (delta > 0) {
        ui.lib_fav_page++;
    }
    pod_ui_rebuild();
}

static void queue_from_favorites(void)
{
    if (!ui.queue) return;
    ui.queue_count = 0;
    for (size_t i = 0; i < s_fav_count && ui.queue_count < MAX_QUEUE; i++) {
        pod_dir_entry_t *e = &ui.queue[ui.queue_count];
        memset(e, 0, sizeof(*e));
        const char *name = strrchr(s_fav_paths[i], '/');
        snprintf(e->name, sizeof(e->name), "%.*s",
                 (int)(sizeof(e->name) - 1), name ? name + 1 : s_fav_paths[i]);
        snprintf(e->path, sizeof(e->path), "%s", s_fav_paths[i]);
        e->is_audio = true;
        ui.queue_count++;
    }
    queue_save();
}

static void favorite_track_cb(lv_event_t *event)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (index >= s_fav_count) return;
    stop_for_track_change();
    queue_from_favorites();
    play_queue_index(index);
    goto_now();
}

static void favorites_open_cb(lv_event_t *event)
{
    (void)event;
    ui.lib_favorites = true;
    ui.lib_root = false;
    ui.lib_fav_page = 0;
    pod_ui_rebuild();
}

static void build_library_favorites(void)
{
    page_begin("FAVORITES");
    song_favorites_collect();

    lv_obj_t *back = lv_button_create(ui.page);
    lv_obj_set_size(back, 96, 38);
    lv_obj_set_pos(back, 16, 0);
    button_style(back, C_SURFACE, 12);
    lv_obj_add_event_cb(back, favorites_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = mk_label(back, LV_SYMBOL_LEFT "  BACK", F_SMALL, C_TEXT);
    lv_obj_center(bl);

    lv_obj_t *hdr = mk_label(ui.page, "LIKED TRACKS", F_MICRO, C_MUTED);
    lv_obj_set_pos(hdr, 16, 48);
    char count_text[32];
    snprintf(count_text, sizeof(count_text), "%zu tracks", s_fav_count);
    lv_obj_t *ct = mk_label(ui.page, count_text, F_MICRO, C_MUTED);
    lv_obj_align(ct, LV_ALIGN_TOP_RIGHT, -16, 48);

    if (s_fav_count == 0) {
        empty_state("No liked songs yet", "Tap the heart on Now Playing to add one.");
        return;
    }

    const size_t per_page = 5;
    size_t pages = (s_fav_count + per_page - 1) / per_page;
    if (ui.lib_fav_page >= pages) ui.lib_fav_page = pages - 1;
    size_t start = ui.lib_fav_page * per_page;
    size_t n = s_fav_count - start;
    if (n > per_page) n = per_page;

    int row_y = 78;
    for (size_t i = 0; i < n; i++) {
        size_t idx = start + i;
        lv_obj_t *row = lv_button_create(ui.page);
        lv_obj_set_size(row, 428, 54);
        lv_obj_set_pos(row, 16, row_y);
        card_style(row, C_SURFACE, 14);
        lv_obj_add_event_cb(row, favorite_track_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)idx);

        char rank[8];
        snprintf(rank, sizeof(rank), "%u", (unsigned)(idx + 1));
        lv_obj_t *rl = mk_label(row, rank, F_MICRO, C_ACCENT);
        lv_obj_set_pos(rl, 14, 17);

        char name[160];
        title_from_path(s_fav_paths[idx], name, sizeof(name));
        lv_obj_t *t = mk_label(row, name, F_SMALL_CN, C_TEXT);
        lv_obj_set_width(t, 330);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(t, 42, 15);
        lv_obj_t *chev = mk_label(row, LV_SYMBOL_PLAY, F_BODY, C_ACCENT);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -14, 0);

        row_y += 58;
    }

    if (pages > 1) {
        lv_obj_t *prev = lv_button_create(ui.page);
        lv_obj_set_size(prev, 96, 34);
        lv_obj_set_pos(prev, 16, row_y + 12);
        button_style(prev, C_SURFACE, 10);
        lv_obj_add_event_cb(prev, favorites_page_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)-1);
        lv_obj_t *pl = mk_label(prev, LV_SYMBOL_LEFT "  PREV", F_MICRO, C_TEXT);
        lv_obj_center(pl);

        char pagebuf[32];
        snprintf(pagebuf, sizeof(pagebuf), "%u / %u",
                 (unsigned)(ui.lib_fav_page + 1), (unsigned)pages);
        lv_obj_t *pc = mk_label(ui.page, pagebuf, F_MICRO, C_MUTED);
        lv_obj_align(pc, LV_ALIGN_TOP_MID, 0, row_y + 20);

        lv_obj_t *next = lv_button_create(ui.page);
        lv_obj_set_size(next, 96, 34);
        lv_obj_align(next, LV_ALIGN_TOP_RIGHT, -16, row_y + 12);
        button_style(next, C_SURFACE, 10);
        lv_obj_add_event_cb(next, favorites_page_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)1);
        lv_obj_t *nl = mk_label(next, "NEXT  " LV_SYMBOL_RIGHT, F_MICRO, C_TEXT);
        lv_obj_center(nl);
    }
}

static void build_library_sources(void)
{
    page_begin("LIBRARY");

    lv_obj_t *sub = mk_label(ui.page, "Folder-first library", F_SMALL_CN, C_MUTED);
    lv_obj_set_width(sub, 428);
    lv_obj_set_pos(sub, 16, 0);

    lv_obj_t *fav = lv_button_create(ui.page);
    lv_obj_set_size(fav, 428, 96);
    lv_obj_set_pos(fav, 16, 30);
    card_style(fav, C_SURFACE, 20);
    lv_obj_add_event_cb(fav, favorites_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *fi = lv_image_create(fav);
    lv_obj_set_size(fi, 32, 32);
    lv_obj_set_pos(fi, 20, 32);
    lv_image_set_src(fi, &heart_filled_dsc);
    lv_obj_set_style_image_recolor(fi, col(C_ACCENT), 0);
    lv_obj_set_style_image_recolor_opa(fi, LV_OPA_COVER, 0);
    lv_obj_t *fn = mk_label(fav, "Favorites", F_NOW_CN, C_TEXT);
    lv_obj_set_pos(fn, 78, 18);
    lv_obj_t *fm = mk_label(fav, "Liked tracks across all folders", F_SMALL_CN, C_MUTED);
    lv_obj_set_pos(fm, 78, 54);
    lv_obj_t *fc = mk_label(fav, LV_SYMBOL_RIGHT, F_BODY, C_MUTED);
    lv_obj_align(fc, LV_ALIGN_RIGHT_MID, -18, 0);

    for (size_t i = 0; i < s_lib_count; i++) {
        const char *path = s_lib_sources[i];
        lv_obj_t *row = lv_button_create(ui.page);
        lv_obj_set_size(row, 428, 96);
        lv_obj_set_pos(row, 16, 138 + (int32_t)i * 108);
        card_style(row, C_SURFACE, 20);
        lv_obj_add_event_cb(row, library_source_cb, LV_EVENT_CLICKED,
                            (void *)path);

        lv_obj_t *icon = mk_label(row, LV_SYMBOL_DIRECTORY,
                                  &lv_font_montserrat_28, C_ACCENT);
        lv_obj_set_pos(icon, 20, 30);
        lv_obj_t *name = mk_label(row, base_name(path), F_NOW_CN, C_TEXT);
        lv_obj_set_pos(name, 78, 18);
        lv_obj_t *meta = mk_label(row, path, F_SMALL_CN, C_MUTED);
        lv_obj_set_pos(meta, 78, 54);
        lv_obj_t *chev = mk_label(row, LV_SYMBOL_RIGHT, F_BODY, C_MUTED);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -18, 0);
    }

    int32_t add_y = 138 + (int32_t)s_lib_count * 108;
    lv_obj_t *add = lv_button_create(ui.page);
    lv_obj_set_size(add, 428, 56);
    lv_obj_set_pos(add, 16, add_y);
    button_style(add, C_SURFACE, 16);
    lv_obj_add_event_cb(add, library_add_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *add_label = mk_label(add, LV_SYMBOL_PLUS "  ADD MUSIC FOLDER",
                                   F_SMALL, C_ACCENT);
    lv_obj_center(add_label);

    lv_obj_t *hint = mk_label(ui.page,
                              "Each folder is one playlist. Nested folders stay separate.",
                              F_SMALL_CN, C_MUTED);
    lv_obj_set_width(hint, 420);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(hint, 16, add_y + 76);
}

static void build_library(void)
{
    if (!pod_storage_sd_ready()) {
        page_begin("LIBRARY");
        empty_state("microSD not mounted", "Insert a card and reboot.");
        return;
    }
    if (ui.lib_favorites) {
        build_library_favorites();
        return;
    }
    if (ui.lib_root || !ui.lib_cwd[0]) {
        build_library_sources();
        return;
    }

    page_begin("LIBRARY");
    build_rows(ui.lib_cwd, true, false);
}

static void files_root_cb(lv_event_t *event)
{
    const char *path = (const char *)lv_event_get_user_data(event);
    snprintf(ui.files_cwd, sizeof(ui.files_cwd), "%s", path);
    ui.files_root = false;
    pod_ui_rebuild();
}

static void build_files_roots(void)
{
    page_begin("FILES");

    const struct {
        const char *label;
        const char *path;
        const char *meta;
        const char *icon;
        bool ready;
    } roots[] = {
        {"microSD", "/sdcard", pod_storage_sd_ready() ? "Removable music library" : "Not mounted",
         LV_SYMBOL_SD_CARD, pod_storage_sd_ready()},
        {"Internal", "/littlefs", pod_storage_littlefs_ready() ? "User data / settings" : "Not mounted",
         LV_SYMBOL_DRIVE, pod_storage_littlefs_ready()},
    };

    for (int i = 0; i < 2; i++) {
        lv_obj_t *row = lv_button_create(ui.page);
        lv_obj_set_size(row, 400, 92);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 12 + i * 108);
        card_style(row, C_SURFACE, 20);
        lv_obj_add_event_cb(row, files_root_cb, LV_EVENT_CLICKED, (void *)roots[i].path);

        lv_obj_t *icon = mk_label(row, roots[i].icon, &lv_font_montserrat_28,
                                  roots[i].ready ? C_ACCENT : C_MUTED);
        lv_obj_set_pos(icon, 20, 26);
        lv_obj_t *t = mk_label(row, roots[i].label, F_NOW_CN, C_TEXT);
        lv_obj_set_pos(t, 78, 18);
        lv_obj_t *m = mk_label(row, roots[i].meta, F_MICRO,
                               roots[i].ready ? C_MUTED : C_ACCENT);
        lv_obj_set_pos(m, 78, 52);
        lv_obj_t *chev = mk_label(row, LV_SYMBOL_RIGHT, F_BODY, C_MUTED);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -18, 0);
    }
}

static void build_files(void)
{
    if (ui.files_root || !ui.files_cwd[0]) {
        build_files_roots();
        return;
    }

    page_begin("FILES");
    build_rows(ui.files_cwd, true, true);
}

/* -------------------------------------------------------------------------- */
/* Now playing                                                                */
/* -------------------------------------------------------------------------- */

static void now_layout(void);

static void apply_cover_gradient(lv_obj_t *obj)
{
    if (!obj) return;
    uint32_t c1 = ui.cover_bg1;
    uint32_t c2 = ui.cover_bg2;
    lv_obj_set_style_bg_color(obj, col(c1), 0);
    lv_obj_set_style_bg_grad_color(obj, col(c2), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}

static void apply_cover_background(void)
{
    /* Keep the last known cover tint while the next track is being probed.
     * It is replaced only when a new cover has actually produced colors. */
    uint32_t c1 = ui.cover_bg1;

    apply_cover_gradient(ui.screen);
    apply_cover_gradient(ui.page);
    if (ui.top) lv_obj_set_style_bg_color(ui.top, col(c1), 0);
    if (ui.nav) lv_obj_set_style_bg_color(ui.nav, col(c1), 0);
    apply_cover_gradient(ui.queue_overlay);
}

typedef struct {
    uint32_t time_ms;
    char *text;
} lyric_line_t;

#define MAX_LYRIC_LINES 512

static lyric_line_t *s_lyric_lines;
static size_t s_lyric_count;
static size_t s_lyric_cap;
static bool s_lyric_timed;
static size_t s_lyric_current = (size_t)-1;
static bool s_lyric_animating;
static size_t s_lyric_pending;

static void lyrics_parsed_clear(void)
{
    if (s_lyric_lines) {
        for (size_t i = 0; i < s_lyric_count; i++) {
            if (s_lyric_lines[i].text) heap_caps_free(s_lyric_lines[i].text);
        }
        heap_caps_free(s_lyric_lines);
    }
    s_lyric_lines = NULL;
    s_lyric_count = 0;
    s_lyric_cap = 0;
    s_lyric_timed = false;
    s_lyric_current = (size_t)-1;
}

static bool lyric_time_parse(const char *tag, size_t len, uint32_t *out_ms)
{
    char buf[48];
    if (!tag || len == 0 || len >= sizeof(buf)) return false;
    memcpy(buf, tag, len);
    buf[len] = '\0';

    int h = 0, m = 0;
    double s = 0.0;
    int n;
    if (strchr(buf, ':') != strrchr(buf, ':')) {
        n = sscanf(buf, "%d:%d:%lf", &h, &m, &s);
        if (n != 3) return false;
    } else {
        n = sscanf(buf, "%d:%lf", &m, &s);
        if (n != 2) return false;
    }
    double total = (h * 3600.0) + (m * 60.0) + s;
    if (total < 0) total = 0;
    *out_ms = (uint32_t)(total * 1000.0);
    return true;
}

static void lyric_line_add(uint32_t time_ms, const char *text, size_t len)
{
    if (s_lyric_count >= MAX_LYRIC_LINES) return;
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) len--;
    if (len == 0) return;

    if (s_lyric_count >= s_lyric_cap) {
        size_t cap = s_lyric_cap ? s_lyric_cap * 2 : 32;
        if (cap > MAX_LYRIC_LINES) cap = MAX_LYRIC_LINES;
        lyric_line_t *p = heap_caps_realloc(s_lyric_lines,
                                            cap * sizeof(*p),
                                            MALLOC_CAP_SPIRAM);
        if (!p) return;
        s_lyric_lines = p;
        s_lyric_cap = cap;
    }
    char *copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!copy) return;
    memcpy(copy, text, len);
    copy[len] = '\0';
    s_lyric_lines[s_lyric_count].time_ms = time_ms;
    s_lyric_lines[s_lyric_count].text = copy;
    s_lyric_count++;
}

static int lyric_time_cmp(const void *a, const void *b)
{
    const lyric_line_t *la = (const lyric_line_t *)a;
    const lyric_line_t *lb = (const lyric_line_t *)b;
    if (la->time_ms < lb->time_ms) return -1;
    if (la->time_ms > lb->time_ms) return 1;
    return 0;
}

static void lyrics_parse(void)
{
    lyrics_parsed_clear();
    if (!ui.lyrics || ui.lyrics_len == 0) return;

    const char *p = ui.lyrics;
    const char *end = ui.lyrics + ui.lyrics_len;
    int32_t offset_ms = 0;
    bool any_time = false;

    while (p < end) {
        const char *line_end = p;
        while (line_end < end && *line_end != '\n' && *line_end != '\r') {
            line_end++;
        }
        const char *q = p;
        while (q < line_end && (*q == ' ' || *q == '\t')) q++;

        uint32_t times[8];
        size_t time_count = 0;
        bool line_timed = false;
        while (q < line_end && *q == '[') {
            const char *close = q + 1;
            while (close < line_end && *close != ']') close++;
            if (close >= line_end) break;
            const char *tag = q + 1;
            size_t tag_len = (size_t)(close - tag);
            if (tag_len > 7 && strncasecmp(tag, "offset:", 7) == 0) {
                offset_ms = (int32_t)strtol(tag + 7, NULL, 10);
            } else {
                uint32_t ms;
                if (time_count < sizeof(times) / sizeof(times[0]) &&
                    lyric_time_parse(tag, tag_len, &ms)) {
                    times[time_count++] = ms;
                    line_timed = true;
                }
            }
            q = close + 1;
            while (q < line_end && (*q == ' ' || *q == '\t')) q++;
        }

        if (q < line_end) {
            if (line_timed) {
                any_time = true;
                for (size_t i = 0; i < time_count; i++) {
                    int64_t adjusted = (int64_t)times[i] + offset_ms;
                    if (adjusted < 0) adjusted = 0;
                    lyric_line_add((uint32_t)adjusted, q,
                                   (size_t)(line_end - q));
                }
            } else {
                lyric_line_add(0, q, (size_t)(line_end - q));
            }
        }

        p = line_end;
        while (p < end && (*p == '\n' || *p == '\r')) p++;
    }

    s_lyric_timed = any_time;
    if (any_time) {
        qsort(s_lyric_lines, s_lyric_count, sizeof(*s_lyric_lines),
              lyric_time_cmp);
    }
    s_lyric_current = (size_t)-1;
    ESP_LOGI(TAG, "lyrics parsed: %u lines, timed=%d",
             (unsigned)s_lyric_count, (int)s_lyric_timed);
}

static size_t lyric_index_at(int64_t pos_ms)
{
    if (s_lyric_count == 0) return 0;
    size_t lo = 0, hi = s_lyric_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (s_lyric_lines[mid].time_ms <= (uint32_t)pos_ms) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo > 0 ? lo - 1 : 0;
}

static char *lyrics_display_copy(const char *src, size_t len)
{
    if (!src || len == 0) return NULL;
    char *out = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!out) return NULL;

    size_t o = 0;
    const char *p = src;
    const char *end = src + len;
    while (p < end) {
        const char *line_end = p;
        while (line_end < end && *line_end != '\n' && *line_end != '\r') {
            line_end++;
        }

        const char *text = p;
        while (text < line_end && (*text == ' ' || *text == '\t')) text++;
        /* Strip leading LRC timestamps / metadata tags: [00:12.34] etc. */
        while (text < line_end && *text == '[') {
            const char *close = text;
            while (close < line_end && *close != ']') close++;
            if (close >= line_end) break;
            text = close + 1;
            while (text < line_end && (*text == ' ' || *text == '\t')) text++;
        }
        if (text < line_end) {
            while (text < line_end) out[o++] = *text++;
            out[o++] = '\n';
        }

        p = line_end;
        while (p < end && (*p == '\n' || *p == '\r')) p++;
    }
    out[o] = '\0';
    if (o == 0) {
        memcpy(out, src, len);
        out[len] = '\0';
    }
    return out;
}

static void now_lyrics_visual(void)
{
    if (!ui.np_lyrics_btn) return;
    if (!ui.has_lyrics) {
        lv_obj_add_flag(ui.np_lyrics_btn, LV_OBJ_FLAG_HIDDEN);
        if (ui.lyrics_page) ui.lyrics_page = false;
        return;
    }
    lv_obj_remove_flag(ui.np_lyrics_btn, LV_OBJ_FLAG_HIDDEN);
    if (ui.np_lyrics_label) {
        lv_label_set_text(ui.np_lyrics_label,
                          ui.lyrics_page ? LV_SYMBOL_CLOSE : "词");
    }
}

static void lyrics_toggle_cb(lv_event_t *event)
{
    (void)event;
    if (!ui.has_lyrics) return;
    ui.lyrics_page = !ui.lyrics_page;
    pod_ui_rebuild();
}

#define LYRICS_CONTENT_Y (-44)
#define LYRICS_LINE_STEP 36

static void lyrics_fill_lines(size_t idx)
{
    if (!s_lyric_timed || !s_lyric_count) return;
    for (int slot = 0; slot < 9; slot++) {
        lv_obj_t *label = ui.np_lyrics_lines[slot];
        if (!label) continue;
        int delta = slot - 4;
        ssize_t line = (ssize_t)idx + delta;
        if (line >= 0 && (size_t)line < s_lyric_count) {
            lv_label_set_text(label, s_lyric_lines[line].text);
        } else {
            lv_label_set_text(label, "");
        }
        uint32_t color = (slot == 4) ? C_TEXT : C_MUTED;
        set_label_font(label, F_NOW_CN, color);
    }
}

static void lyrics_anim_ready(lv_anim_t *anim)
{
    (void)anim;
    s_lyric_animating = false;
    s_lyric_current = s_lyric_pending;
    lyrics_fill_lines(s_lyric_current);
    if (ui.np_lyrics_content) {
        lv_obj_set_y(ui.np_lyrics_content, LYRICS_CONTENT_Y);
    }
}

static void lyrics_anim_start(bool forward)
{
    if (!ui.np_lyrics_content) return;
    s_lyric_animating = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ui.np_lyrics_content);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&a, LYRICS_CONTENT_Y,
                       forward ? LYRICS_CONTENT_Y - LYRICS_LINE_STEP
                               : LYRICS_CONTENT_Y + LYRICS_LINE_STEP);
    lv_anim_set_time(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_ready_cb(&a, lyrics_anim_ready);
    lv_anim_start(&a);
}

static void lyrics_anim_stop(void)
{
    if (ui.np_lyrics_content) {
        lv_anim_del(ui.np_lyrics_content, NULL);
    }
    s_lyric_animating = false;
    s_lyric_pending = 0;
}

static void build_lyrics_panel(void)
{
    if (!ui.lyrics || ui.lyrics_len == 0) return;
    if (s_lyric_count == 0) lyrics_parse();
    if (!s_lyrics_display) {
        s_lyrics_display = lyrics_display_copy(ui.lyrics, ui.lyrics_len);
    }
    s_lyric_animating = false;

    ui.np_lyrics_panel = lv_obj_create(ui.page);
    lv_obj_set_size(ui.np_lyrics_panel, 428, 200);
    lv_obj_set_pos(ui.np_lyrics_panel, 16, 0);
    panel_style(ui.np_lyrics_panel, C_NO_BG, 0);
    lv_obj_set_scroll_dir(ui.np_lyrics_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui.np_lyrics_panel,
                              s_lyric_timed ? LV_SCROLLBAR_MODE_OFF
                                            : LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_all(ui.np_lyrics_panel, 0, 0);

    ui.np_lyrics_content = lv_obj_create(ui.np_lyrics_panel);
    lv_obj_set_size(ui.np_lyrics_content, 428,
                    s_lyric_timed ? 9 * LYRICS_LINE_STEP
                                  : LV_SIZE_CONTENT);
    lv_obj_set_pos(ui.np_lyrics_content, 0,
                   s_lyric_timed ? LYRICS_CONTENT_Y : 0);
    lv_obj_set_style_bg_opa(ui.np_lyrics_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.np_lyrics_content, 0, 0);
    lv_obj_set_style_radius(ui.np_lyrics_content, 0, 0);
    lv_obj_set_style_pad_all(ui.np_lyrics_content, 0, 0);
    no_scroll(ui.np_lyrics_content);

    if (s_lyric_timed) {
        for (int i = 0; i < 9; i++) {
            ui.np_lyrics_lines[i] =
                mk_label(ui.np_lyrics_content, "", F_NOW_CN,
                         i == 4 ? C_TEXT : C_MUTED);
            lv_obj_set_width(ui.np_lyrics_lines[i], 400);
            lv_obj_set_height(ui.np_lyrics_lines[i], 30);
            lv_label_set_long_mode(ui.np_lyrics_lines[i],
                                   LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(ui.np_lyrics_lines[i],
                                        LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(ui.np_lyrics_lines[i], LV_ALIGN_TOP_MID, 0,
                         i * LYRICS_LINE_STEP);
        }
        ui.np_lyrics_text = ui.np_lyrics_lines[4];
        no_scroll(ui.np_lyrics_panel);
    } else {
        ui.np_lyrics_lines[4] =
            mk_label(ui.np_lyrics_content,
                     s_lyrics_display ? s_lyrics_display
                                      : "Lyrics unavailable",
                     F_NOW_CN, C_TEXT);
        ui.np_lyrics_text = ui.np_lyrics_lines[4];
        lv_obj_set_width(ui.np_lyrics_text, 400);
        lv_obj_set_height(ui.np_lyrics_text, LV_SIZE_CONTENT);
        lv_label_set_long_mode(ui.np_lyrics_text, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(ui.np_lyrics_text,
                                    LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(ui.np_lyrics_text, LV_ALIGN_TOP_MID, 0, 12);
    }

    lv_obj_add_flag(ui.np_cover, LV_OBJ_FLAG_HIDDEN);
    lyrics_sync_update();
}

static void lyrics_sync_update(void)
{
    if (!ui.np_lyrics_text || !s_lyric_count || !s_lyric_timed) return;
    if (s_lyric_animating) return;

    int64_t pos = pod_audio_position_ms();
    size_t idx = lyric_index_at(pos);
    if (idx == s_lyric_current) return;

    if (s_lyric_current != (size_t)-1 && ui.np_lyrics_content &&
        (idx + 1 == s_lyric_current || idx == s_lyric_current + 1)) {
        s_lyric_pending = idx;
        lyrics_anim_start(idx > s_lyric_current);
        return;
    }

    s_lyric_current = idx;
    lyrics_fill_lines(idx);
}

static void now_format_update(void)
{
    if (!ui.np_format) return;
    char ext[12] = "AUDIO";
    const char *dot = strrchr(ui.now_path, '.');
    if (dot && dot[1]) {
        size_t n = 0;
        for (const char *p = dot + 1; *p && n < sizeof(ext) - 1; p++) {
            ext[n++] = (char)toupper((unsigned char)*p);
        }
        ext[n] = '\0';
    }

    char text[48];
    uint32_t rate = 0;
    uint8_t channels = 0, bits = 0;
    if (ui.now_path[0] &&
        pod_audio_get_format(&rate, &channels, &bits) == ESP_OK && rate) {
        char rate_buf[16];
        uint32_t khz10 = (rate + 50) / 100;
        if (khz10 % 10 == 0) {
            snprintf(rate_buf, sizeof(rate_buf), "%" PRIu32, khz10 / 10);
        } else {
            snprintf(rate_buf, sizeof(rate_buf), "%" PRIu32 ".%" PRIu32,
                     khz10 / 10, khz10 % 10);
        }
        if (bits) {
            snprintf(text, sizeof(text), "%s • %u/%s", ext,
                     (unsigned)bits, rate_buf);
        } else {
            snprintf(text, sizeof(text), "%s • %s", ext, rate_buf);
        }
    } else {
        snprintf(text, sizeof(text), "%s", ext);
    }

    if (strcmp(text, ui.np_format_cache) != 0) {
        snprintf(ui.np_format_cache, sizeof(ui.np_format_cache), "%s", text);
        lv_label_set_text(ui.np_format, text);
    }
}

static void now_favorite_refresh(void)
{
    if (!ui.np_fav_img) return;
    bool fav = ui.now_path[0] && song_favorite_get_path(ui.now_path);
    lv_image_set_src(ui.np_fav_img,
                     fav ? &heart_filled_dsc : &heart_outline_dsc);
    lv_obj_set_style_image_recolor(ui.np_fav_img,
                                   col(fav ? C_ACCENT : C_MUTED), 0);
    lv_obj_set_style_image_recolor_opa(ui.np_fav_img, LV_OPA_COVER, 0);
}

static void now_favorite_cb(lv_event_t *event)
{
    (void)event;
    if (!ui.now_path[0]) return;
    (void)song_favorite_toggle_path(ui.now_path);
    now_favorite_refresh();
}

static void update_now_static(void)
{
    if (!ui.np_title) return;

    /* Re-apply toggle colours after every page rebuild. */
    if (ui.np_shuffle) {
        set_label_font(ui.np_shuffle, F_SMALL, ui.shuffle ? C_ACCENT : C_MUTED);
    }
    repeat_apply_visual();

    if (!ui.now_path[0]) {
        lv_label_set_text_static(ui.np_title, "Nothing playing");
        lv_label_set_text_static(ui.np_artist, "Choose a folder in Library");
        lv_label_set_text_static(ui.np_album, "");
        lv_label_set_text_static(ui.np_cover_initial, "P");
        lv_obj_remove_flag(ui.np_cover_initial, LV_OBJ_FLAG_HIDDEN);
        lv_slider_set_value(ui.np_slider, 0, LV_ANIM_OFF);
        apply_cover_background();
        now_layout();
        now_format_update();
        now_favorite_refresh();
        now_lyrics_visual();
        return;
    }

    lv_label_set_text_static(ui.np_title, ui.now_title);
    lv_label_set_text_static(ui.np_artist, ui.now_artist);
    lv_label_set_text_static(ui.np_album, ui.now_album);

    uint32_t a, b;
    char initial[2];
    cover_palette(ui.now_path, &a, &b, initial);
    lv_label_set_text(ui.np_cover_initial, initial);
    lv_obj_set_style_bg_color(ui.np_cover, col(a), 0);
    lv_obj_set_style_bg_grad_color(ui.np_cover, col(b), 0);

    if (ui.np_cover_img) {
        if (ui.has_cover && ui.cover_data && ui.cover_w && ui.cover_h) {
            memset(&ui.cover_dsc, 0, sizeof(ui.cover_dsc));
            ui.cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            ui.cover_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
            ui.cover_dsc.header.w = ui.cover_w;
            ui.cover_dsc.header.h = ui.cover_h;
            ui.cover_dsc.header.stride = ui.cover_w * 3;
            ui.cover_dsc.data_size = ui.cover_size;
            ui.cover_dsc.data = ui.cover_data;

            lv_image_set_src(ui.np_cover_img, &ui.cover_dsc);
            lv_obj_set_size(ui.np_cover_img, COVER_SIZE, COVER_SIZE);
            lv_obj_center(ui.np_cover_img);
            lv_image_set_inner_align(ui.np_cover_img, LV_IMAGE_ALIGN_CENTER);
            lv_image_set_scale(ui.np_cover_img, LV_SCALE_NONE);
            lv_image_set_pivot(ui.np_cover_img, 0, 0);
            lv_obj_remove_flag(ui.np_cover_img, LV_OBJ_FLAG_HIDDEN);
            /* The fallback initial is a separate label.  It must not sit on
             * top of real artwork (the old code left "C"/"P" floating over
             * the cover). */
            lv_obj_add_flag(ui.np_cover_initial, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_image_set_src(ui.np_cover_img, NULL);
            lv_obj_add_flag(ui.np_cover_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui.np_cover_initial, LV_OBJ_FLAG_HIDDEN);
        }
        /* A queued swdraw task may still hold the previous source; force one
         * more cover redraw after the swap so stale pixels cannot remain. */
        lv_obj_invalidate(ui.np_cover);
    }

    apply_cover_background();
    now_layout();
    now_format_update();
    now_favorite_refresh();
    now_lyrics_visual();
}

static void play_button_cb(lv_event_t *event)
{
    (void)event;
    pod_audio_state_t state = pod_audio_state();
    if (state == POD_AUDIO_PLAYING) {
        (void)pod_audio_pause();
    } else if (state == POD_AUDIO_PAUSED) {
        (void)pod_audio_resume();
    } else if (ui.queue_count > 0) {
        play_queue_index(ui.queue_index < ui.queue_count ? ui.queue_index : 0);
        pod_ui_rebuild();
    } else if (ui.now_path[0]) {
        start_play_path(ui.now_path);
        pod_ui_rebuild();
    } else {
        ui.tab = 1;
        set_tab_visual(1);
        pod_ui_rebuild();
    }
}

static size_t shuffled_index(void)
{
    if (ui.queue_count <= 1) return ui.queue_index;
    size_t idx;
    do {
        idx = (size_t)(esp_random() % ui.queue_count);
    } while (idx == ui.queue_index);
    return idx;
}

static void repeat_apply_visual(void)
{
    if (!ui.np_repeat) return;
    lv_label_set_text(ui.np_repeat,
                      ui.repeat == REPEAT_ONE ? LV_SYMBOL_REFRESH
                                               : LV_SYMBOL_LOOP);
    set_label_font(ui.np_repeat, F_SMALL,
                   ui.repeat == REPEAT_OFF ? C_MUTED : C_ACCENT);
    if (ui.np_repeat_one) {
        if (ui.repeat == REPEAT_ONE) {
            lv_obj_remove_flag(ui.np_repeat_one, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui.np_repeat_one, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void prev_cb(lv_event_t *event)
{
    (void)event;
    if (ui.queue_count == 0) return;

    size_t idx;
    if (ui.shuffle) {
        idx = shuffled_index();
    } else if (ui.queue_index > 0) {
        idx = ui.queue_index - 1;
    } else if (ui.repeat != REPEAT_OFF) {
        idx = ui.queue_count - 1;
    } else {
        return;
    }
    play_queue_index(idx);
    pod_ui_rebuild();
}

static bool next_queue_index(size_t *out, bool automatic)
{
    if (!out || ui.queue_count == 0) return false;

    /* Repeat-one only affects natural end-of-track.  A manual Next/Prev
     * still navigates (and wraps when any repeat mode is active). */
    if (automatic && ui.repeat == REPEAT_ONE) {
        *out = ui.queue_index;
        return true;
    }

    if (ui.shuffle) {
        if (ui.queue_count <= 1 && ui.repeat == REPEAT_OFF) return false;
        *out = shuffled_index();
        return true;
    }
    if (ui.queue_index + 1 < ui.queue_count) {
        *out = ui.queue_index + 1;
        return true;
    }
    if (ui.repeat != REPEAT_OFF) {
        *out = 0;
        return true;
    }
    return false;
}

static void next_cb(lv_event_t *event)
{
    (void)event;
    size_t idx;
    if (!next_queue_index(&idx, false)) return;
    play_queue_index(idx);
    pod_ui_rebuild();
}

static void shuffle_cb(lv_event_t *event)
{
    (void)event;
    ui.shuffle = !ui.shuffle;
    set_label_font(ui.np_shuffle, F_SMALL, ui.shuffle ? C_ACCENT : C_MUTED);
}

static void repeat_cb(lv_event_t *event)
{
    (void)event;
    ui.repeat = (repeat_mode_t)((ui.repeat + 1) % 3);
    repeat_apply_visual();
}

static void seek_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        ui.seeking = true;
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        ui.seeking = false;
        if (code == LV_EVENT_RELEASED && ui.np_slider) {
            int64_t dur = pod_audio_duration_ms();
            int value = lv_slider_get_value(ui.np_slider);
            if (dur > 0) {
                (void)pod_audio_seek_ms((dur * value) / 1000);
            }
        }
    }
}

#define QUEUE_ROW_POOL 9

typedef struct {
    lv_obj_t *row;
    lv_obj_t *name;
    lv_obj_t *idx;
    size_t index;
} queue_row_t;

static queue_row_t s_queue_rows[QUEUE_ROW_POOL];
static size_t s_queue_first = (size_t)-1;

static void queue_rows_update(lv_obj_t *list)
{
    if (!list || ui.queue_count == 0) return;
    int32_t scroll_y = lv_obj_get_scroll_y(list);
    if (scroll_y < 0) scroll_y = 0;
    size_t first = (size_t)scroll_y / 64;
    if (first >= ui.queue_count) first = ui.queue_count - 1;
    if (ui.queue_count <= QUEUE_ROW_POOL) first = 0;
    if (first == s_queue_first) return;
    s_queue_first = first;

    for (size_t j = 0; j < QUEUE_ROW_POOL; j++) {
        queue_row_t *r = &s_queue_rows[j];
        if (!r->row) continue;
        size_t idx = first + j;
        if (idx >= ui.queue_count) {
            lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(r->row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(r->row, 8, (int32_t)idx * 64);
        lv_obj_set_style_bg_color(r->row,
                                  col(idx == ui.queue_index ? C_SURFACE2
                                                             : C_SURFACE), 0);
        r->index = idx;

        char name[128];
        title_from_path(ui.queue[idx].path, name, sizeof(name));
        lv_label_set_text(r->name, name);
        lv_label_set_text(r->idx, idx == ui.queue_index ? LV_SYMBOL_PLAY : "");
    }
}

static void queue_scroll_cb(lv_event_t *event)
{
    queue_rows_update(lv_event_get_target(event));
}

static void queue_overlay_close(void)
{
    if (ui.queue_overlay) {
        lv_obj_del(ui.queue_overlay);
        ui.queue_overlay = NULL;
    }
    memset(s_queue_rows, 0, sizeof(s_queue_rows));
    s_queue_first = (size_t)-1;
}

static void queue_row_cb(lv_event_t *event)
{
    queue_row_t *row = (queue_row_t *)lv_event_get_user_data(event);
    if (!row || row->index >= ui.queue_count) return;
    size_t index = row->index;
    queue_overlay_close();
    play_queue_index(index);
    pod_ui_rebuild();
}

static void queue_close_cb(lv_event_t *event)
{
    (void)event;
    queue_overlay_close();
}

static void queue_overlay_build(void)
{
    queue_overlay_close();

    ui.queue_overlay = lv_obj_create(ui.screen);
    lv_obj_set_size(ui.queue_overlay, UI_W, UI_H);
    lv_obj_set_pos(ui.queue_overlay, 0, 0);
    panel_style(ui.queue_overlay, C_BG, 0);
    apply_cover_gradient(ui.queue_overlay);

    lv_obj_t *title = mk_label(ui.queue_overlay, "UP NEXT", F_SECTION_CN, C_TEXT);
    lv_obj_set_pos(title, 20, 14);
    lv_obj_t *count = mk_label(ui.queue_overlay, "", F_MICRO, C_MUTED);
    lv_obj_set_pos(count, 20, 44);

    char count_text[48];
    snprintf(count_text, sizeof(count_text), "%zu tracks", ui.queue_count);
    lv_label_set_text(count, count_text);

    lv_obj_t *close = lv_button_create(ui.queue_overlay);
    lv_obj_set_size(close, 52, 52);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -14, 8);
    button_style(close, C_SURFACE, LV_RADIUS_CIRCLE);
    lv_obj_add_event_cb(close, queue_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *x = mk_label(close, LV_SYMBOL_CLOSE, F_SECTION, C_MUTED);
    lv_obj_center(x);

    if (ui.queue_count == 0) {
        lv_obj_t *empty = mk_label(ui.queue_overlay, "Queue is empty", F_BODY_CN, C_MUTED);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    lv_obj_t *list = lv_obj_create(ui.queue_overlay);
    lv_obj_set_size(list, 428, 360);
    lv_obj_set_pos(list, 16, 74);
    panel_style(list, C_NO_BG, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_all(list, 0, 0);

    lv_obj_t *content = lv_obj_create(list);
    lv_obj_set_size(content, 428, ui.queue_count * 64);
    lv_obj_set_pos(content, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    no_scroll(content);

    for (size_t i = 0; i < QUEUE_ROW_POOL; i++) {
        queue_row_t *r = &s_queue_rows[i];
        r->row = lv_button_create(content);
        lv_obj_set_size(r->row, 412, 58);
        lv_obj_set_pos(r->row, 8, 0);
        card_style(r->row, C_SURFACE, 14);
        lv_obj_add_event_cb(r->row, queue_row_cb, LV_EVENT_CLICKED, r);
        lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);

        r->name = mk_label(r->row, "", F_SMALL_CN, C_TEXT);
        lv_obj_set_width(r->name, 310);
        lv_label_set_long_mode(r->name, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(r->name, 16, 10);

        r->idx = mk_label(r->row, "", F_BODY, C_ACCENT);
        lv_obj_align(r->idx, LV_ALIGN_RIGHT_MID, -14, 0);
    }

    lv_obj_add_event_cb(list, queue_scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_update_layout(list);
    if (ui.queue_index > 0) {
        int32_t y = (int32_t)ui.queue_index * 64 - 8;
        if (y > 0) lv_obj_scroll_to_y(list, y, LV_ANIM_OFF);
    }
    s_queue_first = (size_t)-1;
    queue_rows_update(list);
}

static void queue_button_cb(lv_event_t *event)
{
    (void)event;
    queue_overlay_build();
}

static void now_layout(void)
{
    if (!ui.np_title || !ui.np_artist || !ui.np_artist_box ||
        !ui.np_album || !ui.np_album_box || !ui.np_slider ||
        !ui.np_cur || !ui.np_dur || !ui.np_controls) {
        return;
    }

    /* One-line, auto-scrolling title keeps the vertical layout fixed no
     * matter how long the title is.  Artist and album get separate 20px
     * lines so a long album is never hidden under the progress bar. */
    const int32_t artist_y = 236;
    lv_obj_align(ui.np_artist_box, LV_ALIGN_TOP_LEFT, 24, artist_y);

    bool has_album = ui.now_album[0] != '\0';
    lv_obj_align(ui.np_album_box, LV_ALIGN_TOP_LEFT, 24, artist_y + 20);
    if (has_album) {
        lv_obj_remove_flag(ui.np_album_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui.np_album_box, LV_OBJ_FLAG_HIDDEN);
    }

    int32_t slider_y = artist_y + 22 + (has_album ? 20 : 0);
    if (ui.np_slider_track) {
        lv_obj_align(ui.np_slider_track, LV_ALIGN_TOP_MID, 0, slider_y + 5);
    }
    lv_obj_align(ui.np_slider, LV_ALIGN_TOP_MID, 0, slider_y);
    lv_obj_align(ui.np_cur, LV_ALIGN_TOP_LEFT, 28, slider_y + 18);
    lv_obj_align(ui.np_dur, LV_ALIGN_TOP_RIGHT, -28, slider_y + 18);
    lv_obj_align(ui.np_controls, LV_ALIGN_TOP_MID, 0, slider_y + 32);

    ESP_LOGI(TAG, "now layout: album=%d slider_y=%" PRId32,
             has_album, slider_y);
}

static void build_now(void)
{
    page_begin("NOW PLAYING");
    /* This page is a fixed layout; leaving it scrollable made accidental
     * vertical drags re-render the whole 460px page.  With the nav hidden the
     * page is 64 px taller, so nudge the fixed composition down to keep it
     * visually centered instead of leaving a dark band at the bottom. */
    no_scroll(ui.page);
    int32_t shift = (ui.page_h - PAGE_H) / 4;
    if (shift > 0) {
        lv_obj_set_style_pad_top(ui.page, 8 + shift, 0);
    }

    ui.np_cover = lv_obj_create(ui.page);
    lv_obj_set_size(ui.np_cover, COVER_SIZE, COVER_SIZE);
    lv_obj_align(ui.np_cover, LV_ALIGN_TOP_MID, 0, 0);
    panel_style(ui.np_cover, C_ACCENT, 28);
    lv_obj_set_style_bg_grad_dir(ui.np_cover, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_grad_color(ui.np_cover, col(0x5B3A8E), 0);
    lv_obj_set_style_clip_corner(ui.np_cover, true, 0);

    lv_obj_t *c1 = lv_obj_create(ui.np_cover);
    lv_obj_set_size(c1, 130, 130);
    lv_obj_set_style_radius(c1, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c1, col(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(c1, LV_OPA_10, 0);
    lv_obj_set_style_border_width(c1, 0, 0);
    lv_obj_set_pos(c1, -32, 20);
    no_scroll(c1);

    lv_obj_t *c2 = lv_obj_create(ui.np_cover);
    lv_obj_set_size(c2, 84, 84);
    lv_obj_set_style_radius(c2, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c2, col(0x000000), 0);
    lv_obj_set_style_bg_opa(c2, LV_OPA_20, 0);
    lv_obj_set_style_border_width(c2, 0, 0);
    lv_obj_set_pos(c2, 112, 112);
    no_scroll(c2);

    ui.np_cover_initial = mk_label(ui.np_cover, "P", F_TITLE_CN, 0xFFFFFF);
    lv_obj_center(ui.np_cover_initial);

    ui.np_cover_img = lv_image_create(ui.np_cover);
    lv_obj_set_size(ui.np_cover_img, COVER_SIZE, COVER_SIZE);
    lv_obj_center(ui.np_cover_img);
    lv_image_set_inner_align(ui.np_cover_img, LV_IMAGE_ALIGN_CENTER);
    lv_image_set_antialias(ui.np_cover_img, true);
    lv_obj_add_flag(ui.np_cover_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.np_cover_img, LV_OBJ_FLAG_IGNORE_LAYOUT);

    ui.np_format = mk_label(ui.np_cover, "", F_MICRO, 0xFFFFFF);
    lv_obj_set_style_bg_color(ui.np_format, col(0x111111), 0);
    lv_obj_set_style_bg_opa(ui.np_format, 180, 0);
    lv_obj_set_style_radius(ui.np_format, 10, 0);
    lv_obj_set_style_pad_hor(ui.np_format, 8, 0);
    lv_obj_set_style_pad_ver(ui.np_format, 4, 0);
    lv_obj_set_pos(ui.np_format, 8, 8);

    if (!ui.has_lyrics) ui.lyrics_page = false;
    if (ui.lyrics_page) build_lyrics_panel();

    ui.np_title = mk_label(ui.page, "Nothing playing", F_NOW_CN, C_TEXT);
    lv_obj_set_width(ui.np_title, 330);
    lv_obj_set_height(ui.np_title, 30);
    lv_label_set_long_mode(ui.np_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(ui.np_title, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(ui.np_title, LV_ALIGN_TOP_LEFT, 24, 204);

    /* Keep the artist/album to one 20px line.  A plain label can still paint
     * a wrapped second line outside its own box, which then sits under the
     * progress bar; the clipping parent makes the 20px bound authoritative. */
    ui.np_artist_box = lv_obj_create(ui.page);
    lv_obj_set_size(ui.np_artist_box, 330, 20);
    lv_obj_align(ui.np_artist_box, LV_ALIGN_TOP_LEFT, 24, 236);
    lv_obj_set_style_bg_opa(ui.np_artist_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.np_artist_box, 0, 0);
    lv_obj_set_style_pad_all(ui.np_artist_box, 0, 0);
    lv_obj_set_style_radius(ui.np_artist_box, 0, 0);
    no_scroll(ui.np_artist_box);

    ui.np_artist = mk_label(ui.np_artist_box, "Choose a folder in Library", F_SMALL_CN, C_MUTED);
    lv_obj_set_width(ui.np_artist, 330);
    lv_obj_set_height(ui.np_artist, 20);
    lv_label_set_long_mode(ui.np_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(ui.np_artist, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(ui.np_artist, LV_ALIGN_TOP_LEFT, 0, 0);

    ui.np_album_box = lv_obj_create(ui.page);
    lv_obj_set_size(ui.np_album_box, 330, 20);
    lv_obj_align(ui.np_album_box, LV_ALIGN_TOP_LEFT, 24, 258);
    lv_obj_set_style_bg_opa(ui.np_album_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.np_album_box, 0, 0);
    lv_obj_set_style_pad_all(ui.np_album_box, 0, 0);
    lv_obj_set_style_radius(ui.np_album_box, 0, 0);
    no_scroll(ui.np_album_box);

    ui.np_album = mk_label(ui.np_album_box, "", F_SMALL_CN, C_MUTED);
    lv_obj_set_width(ui.np_album, 330);
    lv_obj_set_height(ui.np_album, 20);
    lv_label_set_long_mode(ui.np_album, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(ui.np_album, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(ui.np_album, LV_ALIGN_TOP_LEFT, 0, 0);

    ui.np_fav_btn = lv_button_create(ui.page);
    lv_obj_set_size(ui.np_fav_btn, 52, 52);
    lv_obj_align(ui.np_fav_btn, LV_ALIGN_TOP_RIGHT, -24, 204);
    button_style(ui.np_fav_btn, C_SURFACE, LV_RADIUS_CIRCLE);
    lv_obj_add_event_cb(ui.np_fav_btn, now_favorite_cb, LV_EVENT_CLICKED, NULL);
    ui.np_fav_img = lv_image_create(ui.np_fav_btn);
    lv_obj_set_size(ui.np_fav_img, 28, 28);
    lv_obj_center(ui.np_fav_img);
    lv_image_set_src(ui.np_fav_img, &heart_outline_dsc);
    lv_obj_set_style_image_recolor(ui.np_fav_img, col(C_MUTED), 0);
    lv_obj_set_style_image_recolor_opa(ui.np_fav_img, LV_OPA_COVER, 0);

    ui.np_slider_track = lv_obj_create(ui.page);
    lv_obj_set_size(ui.np_slider_track, 404, 6);
    lv_obj_align(ui.np_slider_track, LV_ALIGN_TOP_MID, 0, 263);
    lv_obj_set_style_bg_color(ui.np_slider_track, col(C_SURFACE2), 0);
    lv_obj_set_style_bg_opa(ui.np_slider_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui.np_slider_track, 3, 0);
    lv_obj_set_style_border_width(ui.np_slider_track, 0, 0);
    lv_obj_set_style_pad_all(ui.np_slider_track, 0, 0);
    lv_obj_clear_flag(ui.np_slider_track, LV_OBJ_FLAG_CLICKABLE);
    no_scroll(ui.np_slider_track);

    ui.np_slider = lv_slider_create(ui.page);
    lv_obj_set_size(ui.np_slider, 404, 16);
    lv_obj_align(ui.np_slider, LV_ALIGN_TOP_MID, 0, 258);
    lv_slider_set_range(ui.np_slider, 0, 1000);
    lv_slider_set_value(ui.np_slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(ui.np_slider, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.np_slider, col(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ui.np_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui.np_slider, 3, LV_PART_INDICATOR);
    slider_as_bar(ui.np_slider, 16, 6);
    lv_obj_add_event_cb(ui.np_slider, seek_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ui.np_slider, seek_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(ui.np_slider, seek_cb, LV_EVENT_PRESS_LOST, NULL);

    ui.np_cur = mk_label(ui.page, "00:00", F_MICRO, C_MUTED);
    lv_obj_align(ui.np_cur, LV_ALIGN_TOP_LEFT, 28, 276);
    ui.np_dur = mk_label(ui.page, "--:--", F_MICRO, C_MUTED);
    lv_obj_align(ui.np_dur, LV_ALIGN_TOP_RIGHT, -28, 276);

    /* All transport controls live in one flex row so their vertical centres
     * cannot drift apart, regardless of icon font metrics. */
    lv_obj_t *controls = lv_obj_create(ui.page);
    ui.np_controls = controls;
    lv_obj_set_size(controls, 404, 64);
    lv_obj_align(controls, LV_ALIGN_TOP_MID, 0, 290);
    panel_style(controls, C_NO_BG, 0);
    lv_obj_set_style_pad_column(controls, 0, 0);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    no_scroll(controls);

    lv_obj_t *shuffle = lv_button_create(controls);
    lv_obj_set_size(shuffle, 46, 46);
    button_style(shuffle, C_NO_BG, 0);
    lv_obj_add_event_cb(shuffle, shuffle_cb, LV_EVENT_CLICKED, NULL);
    ui.np_shuffle = mk_label(shuffle, LV_SYMBOL_SHUFFLE, F_SMALL, C_MUTED);
    lv_obj_center(ui.np_shuffle);

    lv_obj_t *prev = lv_button_create(controls);
    lv_obj_set_size(prev, 52, 52);
    button_style(prev, C_NO_BG, 0);
    lv_obj_add_event_cb(prev, prev_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *prev_i = mk_label(prev, LV_SYMBOL_PREV, F_NOW, C_TEXT);
    lv_obj_center(prev_i);

    lv_obj_t *play = lv_button_create(controls);
    lv_obj_set_size(play, 64, 64);
    button_style(play, C_ACCENT, LV_RADIUS_CIRCLE);
    lv_obj_add_event_cb(play, play_button_cb, LV_EVENT_CLICKED, NULL);
    ui.np_play_icon = mk_label(play, LV_SYMBOL_PLAY, F_TITLE, 0x0B0B0C);
    lv_obj_center(ui.np_play_icon);

    lv_obj_t *next = lv_button_create(controls);
    lv_obj_set_size(next, 52, 52);
    button_style(next, C_NO_BG, 0);
    lv_obj_add_event_cb(next, next_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *next_i = mk_label(next, LV_SYMBOL_NEXT, F_NOW, C_TEXT);
    lv_obj_center(next_i);

    lv_obj_t *repeat = lv_button_create(controls);
    lv_obj_set_size(repeat, 46, 46);
    button_style(repeat, C_NO_BG, 0);
    lv_obj_add_event_cb(repeat, repeat_cb, LV_EVENT_CLICKED, NULL);
    ui.np_repeat = mk_label(repeat,
                            ui.repeat == REPEAT_ONE ? LV_SYMBOL_REFRESH
                                                    : LV_SYMBOL_LOOP,
                            F_SMALL,
                            ui.repeat == REPEAT_OFF ? C_MUTED : C_ACCENT);
    lv_obj_center(ui.np_repeat);
    ui.np_repeat_one = mk_label(repeat, "1", F_MICRO, C_ACCENT);
    lv_obj_align(ui.np_repeat_one, LV_ALIGN_BOTTOM_RIGHT, -8, -5);
    if (ui.repeat != REPEAT_ONE) {
        lv_obj_add_flag(ui.np_repeat_one, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *queue = lv_button_create(ui.page);
    lv_obj_set_size(queue, 52, 52);
    lv_obj_align(queue, LV_ALIGN_TOP_LEFT, 12, 4);
    button_style(queue, C_SURFACE, 16);
    lv_obj_add_event_cb(queue, queue_button_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ql = mk_label(queue, LV_SYMBOL_LIST, F_TITLE, C_MUTED);
    lv_obj_center(ql);

    ui.np_lyrics_btn = lv_button_create(ui.page);
    lv_obj_set_size(ui.np_lyrics_btn, 52, 52);
    lv_obj_align(ui.np_lyrics_btn, LV_ALIGN_TOP_RIGHT, -12, 4);
    button_style(ui.np_lyrics_btn, C_SURFACE, 16);
    lv_obj_add_event_cb(ui.np_lyrics_btn, lyrics_toggle_cb,
                        LV_EVENT_CLICKED, NULL);
    ui.np_lyrics_label = mk_label(ui.np_lyrics_btn, "词", F_NOW_CN, C_TEXT);
    lv_obj_center(ui.np_lyrics_label);

    update_now_static();
}

/* -------------------------------------------------------------------------- */
/* Listening                                                                  */
/* -------------------------------------------------------------------------- */

static lv_obj_t *stat_card(int x, int y, int w, int h, const char *caption,
                           const char *value, uint32_t accent)
{
    lv_obj_t *card = lv_obj_create(ui.page);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    card_style(card, C_SURFACE, 16);

    lv_obj_t *cap = mk_label(card, caption, F_MICRO, C_MUTED);
    lv_obj_set_pos(cap, 12, 10);
    lv_obj_t *v = mk_label(card, value, F_BODY, accent);
    lv_obj_set_width(v, w - 24);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(v, 12, 36);
    return card;
}

static void listening_queue_cb(lv_event_t *event)
{
    size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (idx >= ui.queue_count) return;
    play_queue_index(idx);
    pod_ui_rebuild();
}

static void listening_update(void)
{
    if (!ui.listen_bar) return;

    pod_audio_state_t state = pod_audio_state();
    const char *text = "IDLE";
    uint32_t color = C_MUTED;
    if (state == POD_AUDIO_PLAYING) {
        text = "PLAYING";
        color = C_GREEN;
    } else if (state == POD_AUDIO_PAUSED) {
        text = "PAUSED";
        color = C_ACCENT;
    } else if (state == POD_AUDIO_ERROR) {
        text = "ERROR";
        color = C_ACCENT;
    }
    if (ui.listen_state) {
        lv_label_set_text(ui.listen_state, text);
        set_label_font(ui.listen_state, F_MICRO, color);
    }

    int64_t pos = pod_audio_position_ms();
    int64_t dur = pod_audio_duration_ms();
    if (!ui.now_path[0]) {
        pos = 0;
        dur = -1;
    }
    if (ui.listen_bar) {
        int value = (dur > 0) ? (int)((pos * 1000) / dur) : 0;
        if (value < 0) value = 0;
        if (value > 1000) value = 1000;
        lv_bar_set_value(ui.listen_bar, value, LV_ANIM_OFF);
    }
    if (ui.listen_cur) {
        char buf[16];
        format_time(pos, buf, sizeof(buf));
        lv_label_set_text(ui.listen_cur, buf);
    }
    if (ui.listen_dur) {
        char buf[16];
        format_time(dur, buf, sizeof(buf));
        lv_label_set_text(ui.listen_dur, buf);
    }
}

static void listening_back_cb(lv_event_t *event)
{
    (void)event;
    ui.listening_page = 0;
    pod_ui_rebuild();
}

static void listening_page_cb(lv_event_t *event)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(event);
    if (delta < 0 && s_rank_page > 0) {
        s_rank_page--;
    } else if (delta > 0) {
        s_rank_page++;
    }
    pod_ui_rebuild();
}

static void listening_open_all_cb(lv_event_t *event)
{
    (void)event;
    ui.listening_page = 1;
    s_rank_page = 0;
    pod_ui_rebuild();
}

static void build_listening_all(void)
{
    page_begin("MOST PLAYED");
    ui.listen_bar = NULL;
    ui.listen_cur = NULL;
    ui.listen_dur = NULL;
    ui.listen_state = NULL;

    lv_obj_t *back = lv_button_create(ui.page);
    lv_obj_set_size(back, 96, 38);
    lv_obj_set_pos(back, 16, 0);
    button_style(back, C_SURFACE, 12);
    lv_obj_add_event_cb(back, listening_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = mk_label(back, LV_SYMBOL_LEFT "  BACK", F_SMALL, C_TEXT);
    lv_obj_center(bl);

    lv_obj_t *hdr = mk_label(ui.page, "RANK   TRACK", F_MICRO, C_MUTED);
    lv_obj_set_pos(hdr, 16, 50);
    lv_obj_t *ph = mk_label(ui.page, "PLAYS", F_MICRO, C_MUTED);
    lv_obj_align(ph, LV_ALIGN_TOP_RIGHT, -28, 50);

    if (!ui.song_stats_enabled) {
        empty_state("Play counts are disabled", "Enable Song play counts in Settings.");
        return;
    }
    if (!listening_rank_build()) {
        empty_state("No play counts yet", "Play a few songs first.");
        return;
    }

    const size_t per_page = 5;
    size_t pages = (s_rank_count + per_page - 1) / per_page;
    if (pages == 0) pages = 1;
    if (s_rank_page >= pages) s_rank_page = pages - 1;

    typedef struct {
        uint32_t count;
        char path[SONG_MAP_PATH_SIZE];
    } page_rec_t;
    page_rec_t page[5];
    memset(page, 0, sizeof(page));

    size_t start = s_rank_page * per_page;
    size_t n = s_rank_count - start;
    if (n > per_page) n = per_page;
    if (s_song_stats_lock &&
        xSemaphoreTake(s_song_stats_lock, portMAX_DELAY) == pdTRUE) {
        for (size_t i = 0; i < n; i++) {
            const song_stat_t *s = &s_song_stats[s_rank_idx[start + i]];
            page[i].count = s->play_count;
            const char *p = song_map_path_locked(s->hash);
            if (p) snprintf(page[i].path, sizeof(page[i].path), "%s", p);
        }
        xSemaphoreGive(s_song_stats_lock);
    }

    int row_y = 80;
    for (size_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(ui.page);
        lv_obj_set_size(row, 428, 50);
        lv_obj_set_pos(row, 16, row_y);
        card_style(row, C_SURFACE, 12);

        char rank[8];
        snprintf(rank, sizeof(rank), "%u", (unsigned)(start + i + 1));
        lv_obj_t *rl = mk_label(row, rank, F_MICRO, C_ACCENT);
        lv_obj_set_pos(rl, 14, 15);

        char name[160];
        if (page[i].path[0]) {
            title_from_path(page[i].path, name, sizeof(name));
        } else {
            snprintf(name, sizeof(name), "Unknown track");
        }
        lv_obj_t *t = mk_label(row, name, F_SMALL_CN, C_TEXT);
        lv_obj_set_width(t, 280);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(t, 42, 13);

        char plays[32];
        snprintf(plays, sizeof(plays), "%" PRIu32, page[i].count);
        lv_obj_t *pc = mk_label(row, plays, F_MICRO, C_ACCENT);
        lv_obj_align(pc, LV_ALIGN_RIGHT_MID, -14, 0);

        row_y += 54;
    }

    if (pages > 1) {
        row_y += 8;
        lv_obj_t *prev = lv_button_create(ui.page);
        lv_obj_set_size(prev, 96, 34);
        lv_obj_set_pos(prev, 16, row_y);
        button_style(prev, C_SURFACE, 10);
        lv_obj_add_event_cb(prev, listening_page_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)-1);
        lv_obj_t *pl = mk_label(prev, LV_SYMBOL_LEFT "  PREV", F_MICRO, C_TEXT);
        lv_obj_center(pl);

        char pagebuf[32];
        snprintf(pagebuf, sizeof(pagebuf), "%u / %u",
                 (unsigned)(s_rank_page + 1), (unsigned)pages);
        lv_obj_t *pc = mk_label(ui.page, pagebuf, F_MICRO, C_MUTED);
        lv_obj_align(pc, LV_ALIGN_TOP_MID, 0, row_y + 8);

        lv_obj_t *next = lv_button_create(ui.page);
        lv_obj_set_size(next, 96, 34);
        lv_obj_align(next, LV_ALIGN_TOP_RIGHT, -16, row_y);
        button_style(next, C_SURFACE, 10);
        lv_obj_add_event_cb(next, listening_page_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)1);
        lv_obj_t *nl = mk_label(next, "NEXT  " LV_SYMBOL_RIGHT, F_MICRO, C_TEXT);
        lv_obj_center(nl);
    }
}

static void build_listening(void)
{
    if (ui.listening_page == 1) {
        build_listening_all();
        return;
    }
    page_begin("LISTENING");
    ui.listen_bar = NULL;
    ui.listen_cur = NULL;
    ui.listen_dur = NULL;
    ui.listen_state = NULL;

    char plays[32], timebuf[32], unique[32];
    snprintf(plays, sizeof(plays), "%" PRIu32, ui.play_count);
    format_time((int64_t)ui.played_seconds * 1000, timebuf,
                sizeof(timebuf));
    if (ui.song_stats_enabled) {
        snprintf(unique, sizeof(unique), "%zu", song_stats_unique_count());
    } else {
        snprintf(unique, sizeof(unique), "--");
    }

    lv_obj_t *hero = lv_obj_create(ui.page);
    lv_obj_set_size(hero, 428, 170);
    lv_obj_set_pos(hero, 16, 4);
    card_style(hero, C_SURFACE, 20);

    lv_obj_t *cover = lv_obj_create(hero);
    lv_obj_set_size(cover, 132, 132);
    lv_obj_set_pos(cover, 14, 18);
    lv_obj_set_style_radius(cover, 16, 0);
    lv_obj_set_style_bg_color(cover, col(C_SURFACE2), 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    no_scroll(cover);
    if (ui.has_cover && ui.cover_data) {
        lv_obj_t *img = lv_image_create(cover);
        lv_obj_set_size(img, 132, 132);
        lv_obj_center(img);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
        lv_image_set_antialias(img, true);
        if (ui.cover_w && ui.cover_h) {
            uint32_t sx = (132u * 256u) / ui.cover_w;
            uint32_t sy = (132u * 256u) / ui.cover_h;
            lv_image_set_scale(img, (uint16_t)(sx < sy ? sx : sy));
        }
        lv_image_set_src(img, &ui.cover_dsc);
    } else {
        char initial[2] = {0};
        if (ui.now_title[0]) {
            initial[0] = ui.now_title[0];
        } else {
            initial[0] = 'P';
        }
        lv_obj_t *il = mk_label(cover, initial, F_TITLE_CN, C_MUTED);
        lv_obj_center(il);
    }

    lv_obj_t *np = mk_label(hero, "NOW PLAYING", F_MICRO, C_MUTED);
    lv_obj_set_pos(np, 160, 14);
    lv_obj_t *title = mk_label(hero,
                               ui.now_path[0] ? ui.now_title : "Nothing playing",
                               F_BODY_CN, C_TEXT);
    lv_obj_set_width(title, 250);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(title, 160, 34);
    lv_obj_t *artist = mk_label(hero,
                                ui.now_path[0] ? ui.now_artist : "Choose a track",
                                F_SMALL_CN, C_MUTED);
    lv_obj_set_width(artist, 250);
    lv_label_set_long_mode(artist, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(artist, 160, 62);

    ui.listen_state = mk_label(hero, "IDLE", F_MICRO, C_MUTED);
    lv_obj_set_pos(ui.listen_state, 160, 88);

    ui.listen_bar = lv_bar_create(hero);
    lv_obj_set_size(ui.listen_bar, 240, 4);
    lv_obj_set_pos(ui.listen_bar, 160, 126);
    lv_bar_set_range(ui.listen_bar, 0, 1000);
    lv_bar_set_value(ui.listen_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(ui.listen_bar, col(C_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui.listen_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ui.listen_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.listen_bar, col(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui.listen_bar, 2, LV_PART_INDICATOR);

    ui.listen_cur = mk_label(hero, "00:00", F_MICRO, C_MUTED);
    lv_obj_set_pos(ui.listen_cur, 160, 136);
    ui.listen_dur = mk_label(hero, "--:--", F_MICRO, C_MUTED);
    lv_obj_align(ui.listen_dur, LV_ALIGN_TOP_RIGHT, -18, 136);

    stat_card(16, 184, 132, 76, "TOTAL PLAYS", plays, C_ACCENT);
    stat_card(164, 184, 132, 76, "LISTENING", timebuf, C_GREEN);
    stat_card(312, 184, 116, 76, "UNIQUE", unique, C_ACCENT2);

    size_t topn = listen_top_build();
    lv_obj_t *hdr = mk_label(ui.page, "MOST PLAYED", F_MICRO, C_MUTED);
    lv_obj_set_pos(hdr, 16, 276);
    char topcount[32];
    if (ui.song_stats_enabled) {
        snprintf(topcount, sizeof(topcount), "%zu songs", topn);
    } else {
        snprintf(topcount, sizeof(topcount), "counts off");
    }
    lv_obj_t *tc = mk_label(ui.page, topcount, F_MICRO, C_MUTED);
    lv_obj_align(tc, LV_ALIGN_TOP_RIGHT, -112, 276);

    lv_obj_t *view = lv_button_create(ui.page);
    lv_obj_set_size(view, 88, 26);
    lv_obj_align(view, LV_ALIGN_TOP_RIGHT, -16, 270);
    button_style(view, C_SURFACE, 10);
    lv_obj_add_event_cb(view, listening_open_all_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *vl = mk_label(view, "VIEW ALL", F_MICRO, C_ACCENT);
    lv_obj_center(vl);

    int row_y = 302;
    if (!ui.song_stats_enabled) {
        lv_obj_t *off = mk_label(ui.page, "Song play counts are disabled",
                                 F_SMALL_CN, C_MUTED);
        lv_obj_set_pos(off, 16, row_y);
    } else if (topn == 0) {
        lv_obj_t *empty = mk_label(ui.page, "No plays recorded yet",
                                   F_SMALL_CN, C_MUTED);
        lv_obj_set_pos(empty, 16, row_y);
    } else {
        for (size_t i = 0; i < topn; i++) {
            lv_obj_t *row = lv_obj_create(ui.page);
            lv_obj_set_size(row, 428, 54);
            lv_obj_set_pos(row, 16, row_y);
            card_style(row, C_SURFACE, 14);

            char rank[8];
            snprintf(rank, sizeof(rank), "%u", (unsigned)(i + 1));
            lv_obj_t *rl = mk_label(row, rank, F_MICRO, C_ACCENT);
            lv_obj_set_pos(rl, 14, 17);

            char name[160];
            if (s_listen_top[i].path[0]) {
                title_from_path(s_listen_top[i].path, name, sizeof(name));
            } else {
                snprintf(name, sizeof(name), "Unknown track");
            }
            lv_obj_t *t = mk_label(row, name, F_SMALL_CN, C_TEXT);
            lv_obj_set_width(t, 280);
            lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
            lv_obj_set_pos(t, 42, 15);

            char plays[32];
            snprintf(plays, sizeof(plays), "%" PRIu32 " plays",
                     s_listen_top[i].count);
            lv_obj_t *pc = mk_label(row, plays, F_MICRO, C_MUTED);
            lv_obj_align(pc, LV_ALIGN_RIGHT_MID, -14, 0);

            row_y += 58;
        }
    }

    row_y += 14;
    lv_obj_t *nhdr = mk_label(ui.page, "UP NEXT", F_MICRO, C_MUTED);
    lv_obj_set_pos(nhdr, 16, row_y);
    char qtext[32];
    snprintf(qtext, sizeof(qtext), "%zu tracks", ui.queue_count);
    lv_obj_t *qc = mk_label(ui.page, qtext, F_MICRO, C_MUTED);
    lv_obj_align(qc, LV_ALIGN_TOP_RIGHT, -16, row_y);
    row_y += 26;

    if (ui.queue_count == 0) {
        lv_obj_t *empty = mk_label(ui.page, "Queue is empty",
                                   F_SMALL_CN, C_MUTED);
        lv_obj_set_pos(empty, 16, row_y);
    } else {
        size_t shown = 0;
        for (size_t n = 1; n <= 2 && shown < 2; n++) {
            size_t idx = ui.queue_index + n;
            if (idx >= ui.queue_count) {
                if (ui.repeat == REPEAT_OFF) break;
                idx %= ui.queue_count;
            }
            if (ui.queue_count <= 1 && idx == ui.queue_index) break;

            lv_obj_t *row = lv_button_create(ui.page);
            lv_obj_set_size(row, 428, 56);
            lv_obj_set_pos(row, 16, row_y);
            card_style(row, C_SURFACE, 14);
            lv_obj_add_event_cb(row, listening_queue_cb, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)idx);

            char num[8];
            snprintf(num, sizeof(num), "%u", (unsigned)n);
            lv_obj_t *nl = mk_label(row, num, F_MICRO, C_ACCENT);
            lv_obj_set_pos(nl, 14, 18);
            char name[128];
            title_from_path(ui.queue[idx].path, name, sizeof(name));
            lv_obj_t *t = mk_label(row, name, F_SMALL_CN, C_TEXT);
            lv_obj_set_width(t, 340);
            lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
            lv_obj_set_pos(t, 42, 16);
            lv_obj_t *chev = mk_label(row, LV_SYMBOL_RIGHT, F_BODY, C_MUTED);
            lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -14, 0);

            row_y += 64;
            shown++;
        }
        if (shown == 0) {
            lv_obj_t *empty = mk_label(ui.page, "No next track (repeat is off)",
                                       F_SMALL_CN, C_MUTED);
            lv_obj_set_pos(empty, 16, row_y);
        }
    }

    listening_update();
}

/* -------------------------------------------------------------------------- */
/* Settings                                                                   */
/* -------------------------------------------------------------------------- */

static void volume_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    int value = lv_slider_get_value(slider);
    ui.volume_db = value;
    (void)pod_audio_set_volume_db((float)value);
}

static void brightness_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    int value = lv_slider_get_value(slider);
    ui.brightness = value;
    if (ui.panel) {
        (void)esp_lcd_panel_co5300_set_brightness(ui.panel, value);
    }
}

static lv_obj_t *settings_slider(const char *caption, int y, int min, int max, int value,
                                 lv_event_cb_t cb)
{
    lv_obj_t *card = lv_obj_create(ui.page);
    lv_obj_set_size(card, 412, 96);
    lv_obj_set_pos(card, 16, y);
    card_style(card, C_SURFACE, 16);

    lv_obj_t *cap = mk_label(card, caption, F_MICRO, C_MUTED);
    lv_obj_set_pos(cap, 16, 10);

    /* The slider itself is an invisible 32px interaction surface; this
     * sibling draws the actual 6px rounded track. */
    lv_obj_t *track = lv_obj_create(card);
    lv_obj_set_size(track, 360, 6);
    lv_obj_set_pos(track, 16, 57);
    lv_obj_set_style_bg_color(track, col(C_SURFACE2), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 3, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_CLICKABLE);
    no_scroll(track);

    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_size(slider, 360, 32);
    lv_obj_set_pos(slider, 16, 44);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, col(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 3, LV_PART_INDICATOR);
    slider_as_bar(slider, 32, 6);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return slider;
}

static lv_obj_t *settings_switch(const char *caption, int y, bool on)
{
    lv_obj_t *card = lv_obj_create(ui.page);
    lv_obj_set_size(card, 412, 56);
    lv_obj_set_pos(card, 16, y);
    card_style(card, C_SURFACE, 14);

    lv_obj_t *cap = mk_label(card, caption, F_BODY, C_TEXT);
    lv_obj_set_pos(cap, 16, 18);
    lv_obj_t *sw = lv_switch_create(card);
    lv_obj_set_size(sw, 46, 24);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_bg_color(sw, col(C_SURFACE2), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, col(C_SURFACE2), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR);
    /* The default theme re-applies its own checked colour unless the checked
     * selector is set explicitly. */
    lv_obj_set_style_bg_color(sw, col(C_ACCENT),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER,
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, col(C_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, col(C_TEXT),
                              LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(sw, 0, 0);
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    return sw;
}

static const int auto_off_values[] = {0, 15, 30, 60, 300};
static const char *auto_off_names[] = {"Off", "15 seconds", "30 seconds", "1 minute", "5 minutes"};
static lv_obj_t *auto_off_value_label;

static int auto_off_index(void)
{
    for (size_t i = 0; i < sizeof(auto_off_values) / sizeof(auto_off_values[0]); i++) {
        if (auto_off_values[i] == ui.auto_off_sec) return (int)i;
    }
    return 0;
}

static void auto_off_cb(lv_event_t *event)
{
    (void)event;
    int idx = (auto_off_index() + 1) %
              (int)(sizeof(auto_off_values) / sizeof(auto_off_values[0]));
    ui.auto_off_sec = auto_off_values[idx];
    ui.last_activity_us = esp_timer_get_time();
    if (auto_off_value_label) {
        lv_label_set_text(auto_off_value_label, auto_off_names[idx]);
    }
}

static lv_obj_t *settings_auto_off(int y)
{
    lv_obj_t *card = lv_obj_create(ui.page);
    lv_obj_set_size(card, 412, 56);
    lv_obj_set_pos(card, 16, y);
    card_style(card, C_SURFACE, 14);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, auto_off_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cap = mk_label(card, "Auto screen off", F_BODY, C_TEXT);
    lv_obj_set_pos(cap, 16, 18);
    auto_off_value_label = mk_label(card, auto_off_names[auto_off_index()],
                                    F_SMALL, C_ACCENT);
    lv_obj_align(auto_off_value_label, LV_ALIGN_RIGHT_MID, -42, 0);
    lv_obj_t *chev = mk_label(card, LV_SYMBOL_RIGHT, F_BODY, C_MUTED);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -14, 0);
    return card;
}

static const int autosave_values[] = {0, 5, 10, 15, 30, 45, 60};
static const char *autosave_names[] = {
    "Every song", "5 min", "10 min", "15 min", "30 min", "45 min", "60 min"
};
static lv_obj_t *autosave_value_label;

static int autosave_index(void)
{
    for (size_t i = 0; i < sizeof(autosave_values) / sizeof(autosave_values[0]); i++) {
        if (autosave_values[i] == ui.autosave_min) return (int)i;
    }
    return 0;
}

static void autosave_cb(lv_event_t *event)
{
    (void)event;
    int idx = (autosave_index() + 1) %
              (int)(sizeof(autosave_values) / sizeof(autosave_values[0]));
    ui.autosave_min = autosave_values[idx];
    ui.last_autosave_us = esp_timer_get_time();
    if (autosave_value_label) {
        lv_label_set_text(autosave_value_label, autosave_names[idx]);
    }
}

static lv_obj_t *settings_autosave(int y)
{
    lv_obj_t *card = lv_obj_create(ui.page);
    lv_obj_set_size(card, 412, 56);
    lv_obj_set_pos(card, 16, y);
    card_style(card, C_SURFACE, 14);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, autosave_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cap = mk_label(card, "Auto-save data", F_BODY, C_TEXT);
    lv_obj_set_pos(cap, 16, 18);
    autosave_value_label = mk_label(card, autosave_names[autosave_index()],
                                    F_SMALL, C_ACCENT);
    lv_obj_align(autosave_value_label, LV_ALIGN_RIGHT_MID, -42, 0);
    lv_obj_t *chev = mk_label(card, LV_SYMBOL_RIGHT, F_BODY, C_MUTED);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -14, 0);
    return card;
}

static void gapless_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    ui.gapless = lv_obj_has_state(sw, LV_STATE_CHECKED);
}

static void song_stats_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    ui.song_stats_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
}

static lv_obj_t *format_status;
static bool format_armed;
static lv_obj_t *save_status;

static void save_data_cb(lv_event_t *event)
{
    (void)event;
    settings_save();
    save_all_littlefs_data();
    if (save_status) {
        lv_label_set_text(save_status, "Saved to NVS + LittleFS");
        lv_obj_set_style_text_color(save_status, col(C_GREEN), 0);
    }
}

static void format_fs_cb(lv_event_t *event)
{
    (void)event;
    if (!format_armed) {
        format_armed = true;
        lv_label_set_text(format_status, "Tap FORMAT again to erase /littlefs");
        lv_obj_set_style_text_color(format_status, col(C_ACCENT), 0);
        return;
    }

    format_armed = false;
    lv_label_set_text(format_status, "Formatting...");
    lv_obj_set_style_text_color(format_status, col(C_MUTED), 0);
    esp_err_t err = pod_storage_format_littlefs();
    lv_label_set_text(format_status, err == ESP_OK ? "LittleFS formatted" :
                      "Format failed (partition missing?)");
    lv_obj_set_style_text_color(format_status,
                                err == ESP_OK ? col(C_GREEN) : col(C_ACCENT), 0);
}

static lv_obj_t *settings_action(const char *caption, const char *sub, int y,
                                 const char *button_text, lv_event_cb_t cb)
{
    lv_obj_t *card = lv_obj_create(ui.page);
    lv_obj_set_size(card, 412, 78);
    lv_obj_set_pos(card, 16, y);
    card_style(card, C_SURFACE, 14);

    lv_obj_t *t = mk_label(card, caption, F_BODY, C_TEXT);
    lv_obj_set_pos(t, 16, 12);
    lv_obj_t *s = mk_label(card, sub, F_MICRO, C_MUTED);
    lv_obj_set_pos(s, 16, 40);
    lv_obj_set_width(s, 240);
    lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);

    lv_obj_t *btn = lv_button_create(card);
    lv_obj_set_size(btn, 116, 48);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_bg_color(btn, col(C_SURFACE2), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = mk_label(btn, button_text, F_SMALL, C_ACCENT);
    lv_obj_center(bl);
    return s;
}

static void settings_storage_open_cb(lv_event_t *event)
{
    (void)event;
    ui.settings_page = 1;
    pod_ui_rebuild();
}

static void settings_storage_back_cb(lv_event_t *event)
{
    (void)event;
    ui.settings_page = 0;
    pod_ui_rebuild();
}

static void settings_storage_refresh_cb(lv_event_t *event)
{
    (void)event;
    pod_ui_rebuild();
}

static void storage_card(int y, const char *name, uint64_t used,
                         uint64_t total, uint64_t free_bytes, bool available)
{
    lv_obj_t *card = lv_obj_create(ui.page);
    lv_obj_set_size(card, 412, 70);
    lv_obj_set_pos(card, 16, y);
    card_style(card, C_SURFACE, 14);

    lv_obj_t *n = mk_label(card, name, F_BODY, C_TEXT);
    lv_obj_set_pos(n, 16, 8);

    char u[24], t[24], f[24], line[128];
    if (available && total > 0) {
        format_capacity(used, u, sizeof(u));
        format_capacity(total, t, sizeof(t));
        format_capacity(free_bytes, f, sizeof(f));
        snprintf(line, sizeof(line), "USED %s / %s  FREE %s", u, t, f);
    } else {
        snprintf(line, sizeof(line), "NOT MOUNTED");
    }
    lv_obj_t *v = mk_label(card, line, F_MICRO, available ? C_MUTED : C_ACCENT);
    lv_obj_set_width(v, 380);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -16, 28);

    lv_obj_t *bar = lv_bar_create(card);
    lv_obj_set_size(bar, 380, 10);
    lv_obj_set_pos(bar, 16, 48);
    lv_bar_set_range(bar, 0, 1000);
    uint32_t pct = 0;
    if (available && total > 0) {
        pct = (uint32_t)((used * 1000ULL) / total);
        if (pct > 1000) pct = 1000;
    }
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, col(C_SURFACE2), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, col(pct >= 850 ? C_ACCENT : C_GREEN),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 5, LV_PART_INDICATOR);
}

static void build_settings_storage(void)
{
    page_begin("STORAGE");

    lv_obj_t *back = lv_button_create(ui.page);
    lv_obj_set_size(back, 96, 38);
    lv_obj_set_pos(back, 16, 0);
    button_style(back, C_SURFACE, 12);
    lv_obj_add_event_cb(back, settings_storage_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = mk_label(back, LV_SYMBOL_LEFT "  BACK", F_SMALL, C_TEXT);
    lv_obj_center(bl);

    lv_obj_t *refresh = lv_button_create(ui.page);
    lv_obj_set_size(refresh, 96, 38);
    lv_obj_align(refresh, LV_ALIGN_TOP_RIGHT, -16, 0);
    button_style(refresh, C_SURFACE, 12);
    lv_obj_add_event_cb(refresh, settings_storage_refresh_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = mk_label(refresh, "REFRESH", F_MICRO, C_ACCENT);
    lv_obj_center(rl);

    int y = 50;

    size_t sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t sram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    storage_card(y, "SRAM", sram_total - sram_free, sram_total, sram_free, true);
    y += 76;

    size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    storage_card(y, "PSRAM", ps_total - ps_free, ps_total, ps_free,
                 ps_total > 0);
    y += 76;

    uint64_t tf_total = 0, tf_free = 0;
    bool tf_ok = pod_storage_sd_space(&tf_total, &tf_free) == ESP_OK;
    storage_card(y, "TF", tf_ok ? tf_total - tf_free : 0, tf_total, tf_free,
                 tf_ok);
    y += 76;

    uint64_t lfs_total = 0, lfs_used = 0;
    bool lfs_ok = pod_storage_littlefs_space(&lfs_total, &lfs_used) == ESP_OK;
    storage_card(y, "LITTLEFS", lfs_used, lfs_total,
                 lfs_total >= lfs_used ? lfs_total - lfs_used : 0, lfs_ok);
    y += 76;

    nvs_stats_t ns = {0};
    bool nvs_ok = nvs_get_stats("nvs", &ns) == ESP_OK && ns.total_entries > 0;
    uint64_t nvs_total = (uint64_t)ns.total_entries * 32;
    uint64_t nvs_used = (uint64_t)(ns.total_entries - ns.free_entries) * 32;
    storage_card(y, "NVS", nvs_used, nvs_total,
                 nvs_total >= nvs_used ? nvs_total - nvs_used : 0, nvs_ok);
    y += 76;

    uint64_t cache_size = 0;
    (void)pod_storage_dir_size(POD_STORAGE_CACHE_ROOT, &cache_size);
    /* Cache lives on TF; show its footprint against the card capacity. */
    storage_card(y, ".CACHE", cache_size, tf_total, tf_free, tf_ok);

    ESP_LOGI(TAG, "storage: sram %u/%u psram %u/%u tf %llu/%llu "
             "lfs %llu/%llu nvs %llu/%llu cache %llu",
             (unsigned)(sram_total - sram_free), (unsigned)sram_total,
             (unsigned)(ps_total - ps_free), (unsigned)ps_total,
             (unsigned long long)(tf_total - tf_free),
             (unsigned long long)tf_total,
             (unsigned long long)lfs_used, (unsigned long long)lfs_total,
             (unsigned long long)nvs_used, (unsigned long long)nvs_total,
             (unsigned long long)cache_size);
}

static const int s_cal_target_x[4] = {70, 390, 390, 70};
static const int s_cal_target_y[4] = {70, 70, 390, 390};
static lv_obj_t *s_cal_overlay;
static lv_obj_t *s_cal_target;
static lv_obj_t *s_cal_instruction;
static int s_cal_step;
static uint16_t s_cal_raw[4][2];

static void touch_cal_close(bool rebuild)
{
    if (s_cal_overlay) {
        lv_obj_del(s_cal_overlay);
        s_cal_overlay = NULL;
    }
    s_cal_target = NULL;
    s_cal_instruction = NULL;
    s_cal_step = 0;
    if (rebuild) pod_ui_rebuild();
}

static void touch_cal_cancel_cb(lv_event_t *event)
{
    (void)event;
    touch_cal_close(true);
}

static void touch_cal_update_target(void)
{
    if (!s_cal_target || !s_cal_instruction || s_cal_step < 0 ||
        s_cal_step >= 4) {
        return;
    }
    lv_obj_set_pos(s_cal_target, s_cal_target_x[s_cal_step] - 22,
                   s_cal_target_y[s_cal_step] - 22);
    lv_label_set_text_fmt(s_cal_instruction, "Tap target %d / 4",
                          s_cal_step + 1);
}

static void touch_cal_fail(const char *reason)
{
    s_cal_step = 0;
    memset(s_cal_raw, 0, sizeof(s_cal_raw));
    if (s_cal_instruction) {
        lv_label_set_text_fmt(s_cal_instruction,
                              "%s - tap target 1 again", reason);
    }
    touch_cal_update_target();
}

static void touch_cal_press_cb(lv_event_t *event)
{
    (void)event;
    uint16_t rx = 0, ry = 0;
    if (!pod_display_get_last_raw_touch(&rx, &ry)) return;
    if (s_cal_step < 0 || s_cal_step >= 4) return;

    s_cal_raw[s_cal_step][0] = rx;
    s_cal_raw[s_cal_step][1] = ry;
    s_cal_step++;
    if (s_cal_step < 4) {
        touch_cal_update_target();
        return;
    }

    int32_t xl = ((int32_t)s_cal_raw[0][0] + s_cal_raw[3][0]) / 2;
    int32_t xr = ((int32_t)s_cal_raw[1][0] + s_cal_raw[2][0]) / 2;
    int32_t yt = ((int32_t)s_cal_raw[0][1] + s_cal_raw[1][1]) / 2;
    int32_t yb = ((int32_t)s_cal_raw[2][1] + s_cal_raw[3][1]) / 2;
    if (llabs(xr - xl) < 100 || llabs(yb - yt) < 100) {
        touch_cal_fail("Touch span too small");
        return;
    }

    pod_touch_calibration_t cal = {
        .valid = true,
        .x_scale = 320.0f / (float)(xr - xl),
        .y_scale = 320.0f / (float)(yb - yt),
    };
    cal.x_offset = 70.0f - (float)xl * cal.x_scale;
    cal.y_offset = 70.0f - (float)yt * cal.y_scale;

    esp_err_t err = pod_display_touch_calibration_set(&cal);
    if (err != ESP_OK) {
        touch_cal_fail("Calibration rejected");
        return;
    }
    touch_cal_close(true);
}

static void touch_cal_start_cb(lv_event_t *event)
{
    (void)event;
    if (s_cal_overlay) return;
    nav_apply_visibility(false);

    s_cal_overlay = lv_obj_create(ui.screen);
    lv_obj_set_size(s_cal_overlay, UI_W, UI_H);
    lv_obj_set_pos(s_cal_overlay, 0, 0);
    panel_style(s_cal_overlay, C_BG, 0);
    apply_cover_gradient(s_cal_overlay);
    lv_obj_add_flag(s_cal_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_cal_overlay, touch_cal_press_cb,
                        LV_EVENT_PRESSED, NULL);

    lv_obj_t *title = mk_label(s_cal_overlay, "TOUCH CALIBRATION",
                               F_SECTION, C_TEXT);
    lv_obj_set_pos(title, 20, 16);
    s_cal_instruction = mk_label(s_cal_overlay, "", F_SMALL_CN, C_MUTED);
    lv_obj_set_pos(s_cal_instruction, 20, 48);

    lv_obj_t *cancel = lv_button_create(s_cal_overlay);
    lv_obj_set_size(cancel, 96, 40);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -16, 10);
    button_style(cancel, C_SURFACE, 12);
    lv_obj_add_event_cb(cancel, touch_cal_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = mk_label(cancel, "CANCEL", F_MICRO, C_TEXT);
    lv_obj_center(cl);

    s_cal_target = lv_obj_create(s_cal_overlay);
    lv_obj_set_size(s_cal_target, 44, 44);
    lv_obj_set_style_radius(s_cal_target, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_cal_target, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cal_target, 3, 0);
    lv_obj_set_style_border_color(s_cal_target, col(C_ACCENT), 0);
    lv_obj_clear_flag(s_cal_target, LV_OBJ_FLAG_CLICKABLE);

    s_cal_step = 0;
    memset(s_cal_raw, 0, sizeof(s_cal_raw));
    touch_cal_update_target();
}

static void settings_touch_open_cb(lv_event_t *event)
{
    (void)event;
    ui.settings_page = 2;
    pod_ui_rebuild();
}

static void settings_touch_back_cb(lv_event_t *event)
{
    (void)event;
    ui.settings_page = 0;
    pod_ui_rebuild();
}

static void settings_touch_reset_cb(lv_event_t *event)
{
    (void)event;
    (void)pod_display_touch_calibration_reset();
    pod_ui_rebuild();
}

static void build_settings_touch(void)
{
    page_begin("TOUCH");
    lv_obj_t *back = lv_button_create(ui.page);
    lv_obj_set_size(back, 96, 38);
    lv_obj_set_pos(back, 16, 0);
    button_style(back, C_SURFACE, 12);
    lv_obj_add_event_cb(back, settings_touch_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = mk_label(back, LV_SYMBOL_LEFT "  BACK", F_SMALL, C_TEXT);
    lv_obj_center(bl);

    pod_touch_calibration_t cal;
    pod_display_touch_calibration_get(&cal);

    lv_obj_t *card = lv_obj_create(ui.page);
    lv_obj_set_size(card, 412, 126);
    lv_obj_set_pos(card, 16, 48);
    card_style(card, C_SURFACE, 16);
    lv_obj_t *t = mk_label(card, "Touch compensation", F_BODY, C_TEXT);
    lv_obj_set_pos(t, 16, 12);
    lv_obj_t *d = mk_label(card,
                           "Map raw CST820 coordinates to the 460x460 screen.\n"
                           "Calibration is saved in NVS.",
                           F_MICRO, C_MUTED);
    lv_obj_set_width(d, 380);
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(d, 16, 38);

    char line[128];
    if (cal.valid) {
        snprintf(line, sizeof(line),
                 "X: %.4f * raw %+.2f     Y: %.4f * raw %+.2f",
                 cal.x_scale, cal.x_offset, cal.y_scale, cal.y_offset);
    } else {
        snprintf(line, sizeof(line), "Default mapping (no calibration saved)");
    }
    lv_obj_t *v = mk_label(card, line, F_MICRO, cal.valid ? C_ACCENT : C_MUTED);
    lv_obj_set_width(v, 380);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(v, 16, 92);

    lv_obj_t *start = lv_button_create(ui.page);
    lv_obj_set_size(start, 412, 62);
    lv_obj_set_pos(start, 16, 190);
    button_style(start, C_ACCENT, 16);
    lv_obj_add_event_cb(start, touch_cal_start_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = mk_label(start, "START CALIBRATION", F_BODY, 0x0B0B0C);
    lv_obj_center(sl);

    lv_obj_t *reset = lv_button_create(ui.page);
    lv_obj_set_size(reset, 412, 54);
    lv_obj_set_pos(reset, 16, 264);
    button_style(reset, C_SURFACE, 16);
    lv_obj_add_event_cb(reset, settings_touch_reset_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = mk_label(reset, "RESET TO DEFAULT", F_SMALL, C_ACCENT);
    lv_obj_center(rl);

    lv_obj_t *hint = mk_label(ui.page,
                              "Tap the four targets in order. Use Reset if "
                              "the pointer is unusable.",
                              F_SMALL_CN, C_MUTED);
    lv_obj_set_width(hint, 412);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(hint, 16, 332);
}

static void build_settings_main(void)
{
    page_begin("SETTINGS");
    format_armed = false;
    lv_obj_t *vol = settings_slider("VOLUME", 6, -60, 0, ui.volume_db, volume_cb);
    (void)vol;
    settings_slider("BRIGHTNESS", 110, 10, 100, ui.brightness, brightness_cb);
    lv_obj_t *gapless = settings_switch("Gapless playback", 214, ui.gapless);
    lv_obj_add_event_cb(gapless, gapless_cb, LV_EVENT_VALUE_CHANGED, NULL);
    settings_auto_off(276);
    settings_autosave(340);
    lv_obj_t *song_stats = settings_switch("Song play counts", 404,
                                           ui.song_stats_enabled);
    lv_obj_add_event_cb(song_stats, song_stats_cb, LV_EVENT_VALUE_CHANGED,
                        NULL);

    (void)settings_action("Touch calibration",
                          "Compensate raw CST820 coordinates",
                          468, "OPEN", settings_touch_open_cb);
    (void)settings_action("Storage usage",
                          "SRAM / PSRAM / TF / LittleFS / NVS / .cache",
                          554, "OPEN", settings_storage_open_cb);
    format_status = settings_action("Internal storage", "Erase LittleFS user data",
                                    640, "FORMAT", format_fs_cb);
    save_status = settings_action("Playback data",
                                  "Save play count, song stats and last track now",
                                  726, "SAVE", save_data_cb);

    lv_obj_t *about = lv_obj_create(ui.page);
    lv_obj_set_size(about, 412, 80);
    lv_obj_set_pos(about, 16, 812);
    card_style(about, C_SURFACE, 14);
    lv_obj_t *t = mk_label(about, "POD NANO", F_BODY_CN, C_TEXT);
    lv_obj_set_pos(t, 16, 14);
    lv_obj_t *s = mk_label(about, "ESP32-S3  /  CS43131  /  LVGL 9.4", F_MICRO, C_MUTED);
    lv_obj_set_pos(s, 16, 42);
    lv_obj_t *sd = mk_label(about, pod_storage_sd_ready() ? "microSD ready" : "microSD missing",
                            F_MICRO, pod_storage_sd_ready() ? C_GREEN : C_ACCENT);
    lv_obj_align(sd, LV_ALIGN_RIGHT_MID, -16, 0);
}

static void build_settings(void)
{
    if (ui.settings_page == 1) build_settings_storage();
    else if (ui.settings_page == 2) build_settings_touch();
    else build_settings_main();
}

/* -------------------------------------------------------------------------- */
/* Pure cover mode                                                            */
/* -------------------------------------------------------------------------- */

static void cover_fill_dsc(const uint8_t *data, uint16_t w, uint16_t h,
                           size_t size)
{
    memset(&ui.cover_dsc, 0, sizeof(ui.cover_dsc));
    ui.cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    ui.cover_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
    ui.cover_dsc.header.w = w;
    ui.cover_dsc.header.h = h;
    ui.cover_dsc.header.stride = w * 3;
    ui.cover_dsc.data_size = size;
    ui.cover_dsc.data = data;
}

static void build_cover_only(void)
{
    page_begin("COVER");
    if (ui.top) lv_obj_add_flag(ui.top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(ui.page, 0, 0);
    lv_obj_set_height(ui.page, UI_H);
    lv_obj_set_style_pad_all(ui.page, 0, 0);
    apply_cover_background();
    no_scroll(ui.page);

    if (ui.has_cover && ui.cover_large_data &&
        ui.cover_large_w && ui.cover_large_h) {
        /* Full-resolution 464x464 decode, drawn 1:1.  This is sharp; the
         * previous 200x460 upscale looked blurry. */
        cover_fill_dsc(ui.cover_large_data, ui.cover_large_w,
                       ui.cover_large_h, ui.cover_large_size);
        lv_obj_t *img = lv_image_create(ui.page);
        lv_image_set_src(img, &ui.cover_dsc);
        lv_obj_set_size(img, ui.cover_large_w, ui.cover_large_h);
        lv_obj_center(img);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
        lv_image_set_scale(img, LV_SCALE_NONE);
        lv_image_set_pivot(img, 0, 0);
    } else if (ui.has_cover && ui.cover_data && ui.cover_w && ui.cover_h) {
        cover_fill_dsc(ui.cover_data, ui.cover_w, ui.cover_h, ui.cover_size);
        lv_obj_t *img = lv_image_create(ui.page);
        lv_image_set_src(img, &ui.cover_dsc);
        lv_obj_set_size(img, COVER_SIZE, COVER_SIZE);
        lv_obj_center(img);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
        lv_image_set_scale(img, (UI_W * LV_SCALE_NONE) / COVER_SIZE);
        lv_image_set_pivot(img, COVER_SIZE / 2, COVER_SIZE / 2);
    } else {
        lv_obj_set_style_bg_color(ui.page, col(0x241B33), 0);
        lv_obj_set_style_bg_grad_color(ui.page, col(0x0B0B0C), 0);
        lv_obj_set_style_bg_grad_dir(ui.page, LV_GRAD_DIR_VER, 0);
        lv_obj_t *initial = mk_label(ui.page, "P", F_TITLE_CN, 0xFFFFFF);
        lv_obj_center(initial);
    }
}

/* -------------------------------------------------------------------------- */
/* Rebuild / timer                                                            */
/* -------------------------------------------------------------------------- */

void pod_ui_rebuild(void)
{
    if (!ui.page) return;
    lyrics_anim_stop();
    if (ui.tab != 2) ui.library_add_mode = false;
    if (ui.tab != 1) ui.lib_favorites = false;
    if (ui.tab != 3) ui.listening_page = 0;
    if (ui.tab != 0) ui.lyrics_page = false;
    if (ui.tab != 4 && s_cal_overlay) touch_cal_close(false);
    static int rendered_tab = -1;
    if (rendered_tab == 4 && ui.tab != 4) settings_save();
    if (ui.tab != 4) ui.settings_page = 0;
    rendered_tab = ui.tab;

    switch (ui.tab) {
    case 0:
        if (ui.cover_only) build_cover_only();
        else build_now();
        break;
    case 1:
        build_library();
        break;
    case 2:
        build_files();
        break;
    case 3:
        build_listening();
        break;
    case 4:
        build_settings();
        break;
    default:
        break;
    }
    set_tab_visual(ui.tab);
    if (ui.top_sd) {
        set_label_font(ui.top_sd, F_SMALL, pod_storage_sd_ready() ? C_GREEN : C_MUTED);
    }
    apply_cover_background();
    update_touch_state();
}

static void goto_now(void)
{
    ui.cover_only = false;
    ui.tab = 0;
    set_tab_visual(0);
    pod_ui_rebuild();
}

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    cover_retire_poll();
    apply_ready_metadata();

    if (pod_audio_take_eos()) {
        size_t next_idx;
        if (next_queue_index(&next_idx, true)) {
            play_queue_index(next_idx);
            pod_ui_rebuild();
        }
    }

    int64_t now = esp_timer_get_time();
    if (!ui.screen_off && ui.auto_off_sec > 0 &&
        now - ui.last_activity_us > (int64_t)ui.auto_off_sec * 1000000) {
        screen_power(false);
    }
    /* Interval-based autosave waits for the decoder to be idle; it never
     * writes LittleFS while a song is playing or paused. */
    pod_audio_state_t astate = pod_audio_state();
    if (ui.autosave_min > 0 && autosave_due() &&
        (astate == POD_AUDIO_IDLE || astate == POD_AUDIO_ERROR) &&
        (ui.play_dirty || s_song_stats_dirty)) {
        save_all_littlefs_data();
    }
    int pending_tab = s_pending_tab;
    if (pending_tab >= 0 && pending_tab <= 4) {
        s_pending_tab = -1;
        ui.tab = pending_tab;
        if (pending_tab == 1) ui.lib_root = true;
        if (pending_tab == 4) ui.settings_page = 0;
        set_tab_visual(pending_tab);
        pod_ui_rebuild();
    }
    if (s_pending_queue) {
        s_pending_queue = false;
        queue_overlay_build();
    }
    if (s_pending_files_root) {
        s_pending_files_root = false;
        ui.tab = 2;
        snprintf(ui.files_cwd, sizeof(ui.files_cwd), "%s", "/sdcard");
        ui.files_root = false;
        set_tab_visual(2);
        pod_ui_rebuild();
    }
    if (s_pending_nav_toggle) {
        s_pending_nav_toggle = false;
        nav_toggle();
    }
    if (s_pending_cover_toggle) {
        s_pending_cover_toggle = false;
        cover_only_toggle();
    }
    if (s_pending_storage) {
        s_pending_storage = false;
        ui.tab = 4;
        ui.settings_page = 1;
        set_tab_visual(4);
        pod_ui_rebuild();
    }
    if (s_pending_touch_cal) {
        s_pending_touch_cal = false;
        ui.tab = 4;
        ui.settings_page = 2;
        set_tab_visual(4);
        pod_ui_rebuild();
    }
    if (s_pending_top_list) {
        s_pending_top_list = false;
        ui.tab = 3;
        ui.listening_page = 1;
        s_rank_page = 0;
        set_tab_visual(3);
        pod_ui_rebuild();
    }
    if (s_pending_fav_toggle) {
        s_pending_fav_toggle = false;
        if (ui.now_path[0]) {
            (void)song_favorite_toggle_path(ui.now_path);
            now_favorite_refresh();
        }
    }
    if (s_pending_favorites) {
        s_pending_favorites = false;
        ui.tab = 1;
        ui.lib_favorites = true;
        ui.lib_root = false;
        set_tab_visual(1);
        pod_ui_rebuild();
    }
    if (s_pending_lyrics_toggle) {
        s_pending_lyrics_toggle = false;
        if (ui.has_lyrics) {
            ui.lyrics_page = !ui.lyrics_page;
            pod_ui_rebuild();
        }
    }
    if (s_pending_repeat >= 0) {
        ui.repeat = (repeat_mode_t)s_pending_repeat;
        s_pending_repeat = -1;
        repeat_apply_visual();
    }
    if (s_pending_play_index >= 0) {
        int idx = s_pending_play_index;
        s_pending_play_index = -1;
        if (idx < (int)s_eligible_count) {
            play_file_from_current(&ui.entries[s_eligible[idx]]);
        }
    }
    if (s_pending_play_needle) {
        s_pending_play_needle = false;
        for (size_t i = 0; i < ui.entry_count; i++) {
            if (ui.entries[i].is_audio &&
                strstr(ui.entries[i].path, s_play_needle)) {
                play_file_from_current(&ui.entries[i]);
                break;
            }
        }
    }
    if (s_pending_next) {
        s_pending_next = false;
        if (ui.queue_count > 0) {
            size_t idx = (ui.queue_index + 1) % ui.queue_count;
            play_queue_index(idx);
            pod_ui_rebuild();
        }
    }
    if (s_pending_scroll) {
        s_pending_scroll = false;
        if (s_list.list) {
            lv_obj_scroll_to_y(s_list.list, s_pending_scroll_y, LV_ANIM_OFF);
        }
    }

    pod_audio_state_t state = pod_audio_state();
    if (state == POD_AUDIO_PLAYING) {
        if (++ui.tick_quarter >= 4) {
            ui.tick_quarter = 0;
            ui.played_seconds++;
        }
    } else {
        ui.tick_quarter = 0;
    }
    if (ui.tab == 3) listening_update();
    if (ui.tab == 0) {
        now_format_update();
        if (ui.lyrics_page) lyrics_sync_update();
    }
    if (ui.tab != 0 || !ui.np_slider) return;

    if (state != ui.last_state) {
        ui.last_state = state;
        lv_label_set_text_static(ui.np_play_icon,
                                 state == POD_AUDIO_PLAYING ? LV_SYMBOL_PAUSE
                                                            : LV_SYMBOL_PLAY);
        if (state != POD_AUDIO_PLAYING) ui.tick_quarter = 0;
    }

    int64_t pos = pod_audio_position_ms();
    int64_t dur = pod_audio_duration_ms();
    int pos_s = (int)(pos / 1000);
    int dur_s = (int)(dur / 1000);

    static char cur_text[16];
    static char dur_text[16];
    if (pos_s != ui.last_pos_s) {
        ui.last_pos_s = pos_s;
        format_time(pos, cur_text, sizeof(cur_text));
        lv_label_set_text_static(ui.np_cur, cur_text);
    }
    if (dur_s != ui.last_dur_s) {
        ui.last_dur_s = dur_s;
        format_time(dur, dur_text, sizeof(dur_text));
        lv_label_set_text_static(ui.np_dur, dur_text);
    }

    if (!ui.seeking) {
        int value = 0;
        if (dur > 0) {
            value = (int)((pos * 1000) / dur);
            if (value < 0) value = 0;
            if (value > 1000) value = 1000;
        }
        if (value != ui.last_slider) {
            ui.last_slider = value;
            lv_slider_set_value(ui.np_slider, value, LV_ANIM_OFF);
        }
    }

    if (state == POD_AUDIO_ERROR && ui.np_artist) {
        const char *error = pod_audio_last_error();
        if (!ui.error_shown || strcmp(ui.last_error, error) != 0) {
            snprintf(ui.last_error, sizeof(ui.last_error), "%s", error);
            ui.error_shown = true;
            lv_label_set_text(ui.np_artist, ui.last_error);
            set_label_font(ui.np_artist, F_MICRO, C_ACCENT);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Entry point                                                                */
/* -------------------------------------------------------------------------- */

void pod_ui_show_tab(int tab)
{
    if (!ui.page || tab < 0 || tab > 4) return;
    s_pending_tab = tab;
}

void pod_ui_show_queue(void)
{
    if (!ui.page) return;
    s_pending_queue = true;
}

void pod_ui_show_files_root(void)
{
    if (!ui.page) return;
    s_pending_files_root = true;
}

void pod_ui_scroll_list(int32_t y)
{
    if (!ui.page || y < 0) return;
    s_pending_scroll_y = y;
    s_pending_scroll = true;
}

void pod_ui_toggle_nav(void)
{
    if (!ui.page) return;
    s_pending_nav_toggle = true;
}

void pod_ui_next_track(void)
{
    if (!ui.page) return;
    s_pending_next = true;
}

void pod_ui_play_index(int index)
{
    if (!ui.page || index < 0) return;
    s_pending_play_index = index;
}

void pod_ui_play_path_contains(const char *needle)
{
    if (!ui.page || !needle) return;
    snprintf(s_play_needle, sizeof(s_play_needle), "%s", needle);
    s_pending_play_needle = true;
}

void pod_ui_toggle_cover_only(void)
{
    s_pending_cover_toggle = true;
}

void pod_ui_screen_toggle(void)
{
    ESP_LOGI(TAG, "screen toggle request (off=%d)", ui.screen_off);
    screen_power(ui.screen_off);
}

void pod_ui_save_data(void)
{
    settings_save();
    save_all_littlefs_data();
}

void pod_ui_set_auto_off(int seconds)
{
    if (seconds < 0) seconds = 0;
    ui.auto_off_sec = seconds;
    ui.last_activity_us = esp_timer_get_time();
}

void pod_ui_show_storage(void)
{
    s_pending_storage = true;
}

void pod_ui_show_touch_cal(void)
{
    s_pending_touch_cal = true;
}

void pod_ui_set_repeat(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    s_pending_repeat = mode;
}

void pod_ui_show_top_list(void)
{
    s_pending_top_list = true;
}

void pod_ui_toggle_favorite(void)
{
    s_pending_fav_toggle = true;
}

void pod_ui_show_favorites(void)
{
    s_pending_favorites = true;
}

void pod_ui_toggle_lyrics(void)
{
    s_pending_lyrics_toggle = true;
}

void pod_ui_set_autosave(int minutes)
{
    if (minutes < 0) minutes = 0;
    ui.autosave_min = minutes;
    ui.last_autosave_us = esp_timer_get_time();
}

const uint8_t *pod_ui_cover_data(size_t *len, uint16_t *w, uint16_t *h)
{
    if (!ui.cover_data) return NULL;
    if (len) *len = ui.cover_size;
    if (w) *w = ui.cover_w;
    if (h) *h = ui.cover_h;
    return ui.cover_data;
}

static void boot_button_cb(lv_timer_t *timer)
{
    (void)timer;
    static int64_t press_us;
    static bool long_fired;

    bool pressed = gpio_get_level(BOOT_GPIO) == 0;
    int64_t now = esp_timer_get_time();

    if (pressed && !ui.boot_prev) {
        press_us = now;
        long_fired = false;
        ui.last_activity_us = now;
    }

    /* BOOT: long press = screen off/on, short press = overlay menu. */
    if (pressed && !long_fired && now - press_us > 600000) {
        long_fired = true;
        screen_power(ui.screen_off);
    }

    if (!pressed && ui.boot_prev && !long_fired) {
        if (ui.screen_off) screen_power(true);
        else nav_toggle();
    }

    ui.boot_prev = pressed;
}

static void boot_button_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOOT_GPIO) | (1ULL << BTN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ONLY,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    ui.boot_prev = gpio_get_level(BOOT_GPIO) == 0;
    ui.btn_prev = gpio_get_level(BTN_GPIO) == 0;
}

static void btn_button_cb(lv_timer_t *timer)
{
    (void)timer;
    static uint8_t stable_count;
    bool pressed = gpio_get_level(BTN_GPIO) == 0;

    /* 3 consecutive samples (150 ms) before accepting an edge.  GPIO48 is
     * shared with a strapping/LED-adjacent pin on some boards and can look
     * noisy for a few milliseconds around boot. */
    if (pressed != ui.btn_prev) {
        if (++stable_count >= 3) {
            stable_count = 0;
            ui.btn_prev = pressed;
            if (pressed) {
                ui.last_activity_us = esp_timer_get_time();
                if (ui.screen_off) {
                    screen_power(true);
                } else {
                    cover_only_toggle();
                }
            }
        }
    } else {
        stable_count = 0;
    }
}

esp_err_t pod_ui_start(lv_display_t *display, esp_lcd_panel_handle_t panel)
{
    if (!display) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&ui, 0, sizeof(ui));
    init_cn_fonts();

    /* Both CJK faces come from the flash-mapped font partition; see
     * init_cn_fonts() and doc/FONT_PARTITION.md. */

    ui.panel = panel;
    ui.screen = lv_display_get_screen_active(display);
    ui.tab = 0;
    ui.lib_root = true;
    ui.files_root = true;
    ui.page_h = PAGE_H_FULL;
    ui.nav_visible = false;
    ui.volume_db = -20;
    ui.brightness = 80;
    ui.auto_off_sec = 0;
    ui.autosave_min = 0;
    ui.gapless = true;
    ui.song_stats_enabled = true;
    ui.screen_off = false;
    ui.cover_only = false;
    ui.settings_page = 0;
    ui.cover_bg1 = C_BG;
    ui.cover_bg2 = 0x151318;
    ui.last_activity_us = esp_timer_get_time();
    settings_load();
    playback_load();
    ui.last_autosave_us = esp_timer_get_time();
    if (ui.panel) {
        (void)esp_lcd_panel_co5300_set_brightness(ui.panel, ui.brightness);
    }
    boot_button_init();

    /* Queue lives in PSRAM only.  Falling back to internal RAM here would
     * silently reserve ~100 KB of the exact heap LVGL objects need. */
    ui.queue = heap_caps_malloc(MAX_QUEUE * sizeof(pod_dir_entry_t),
                                MALLOC_CAP_SPIRAM);
    if (!ui.queue) {
        ESP_LOGW(TAG, "queue allocation failed; continuing without queue");
    } else {
        queue_load();
    }
    library_sources_init();

    s_play_queue = xQueueCreate(1, sizeof(play_req_t));
    s_meta_lock = xSemaphoreCreateMutex();
    s_song_stats_lock = xSemaphoreCreateMutex();
    s_playback_lock = xSemaphoreCreateMutex();
    song_stats_load();
    song_map_load();
    if (s_song_stats_lock &&
        xSemaphoreTake(s_song_stats_lock, portMAX_DELAY) == pdTRUE) {
        /* Backfill paths for records created before song_paths.bin existed.
         * The restored queue + last track cover the common case; unknown old
         * records remain "Unknown track" instead of triggering an SD scan. */
        song_map_backfill_locked(ui.now_path);
        for (size_t i = 0; i < ui.queue_count; i++) {
            song_map_backfill_locked(ui.queue[i].path);
        }
        xSemaphoreGive(s_song_stats_lock);
    }
    if (!s_play_queue || !s_meta_lock ||
        xTaskCreatePinnedToCore(play_worker_task, "play_meta", 8192, NULL,
                                4, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "metadata worker init failed");
    }

    lv_obj_clean(ui.screen);
    lv_obj_set_style_bg_color(ui.screen, col(C_BG), 0);
    lv_obj_set_style_bg_opa(ui.screen, LV_OPA_COVER, 0);
    no_scroll(ui.screen);

    build_chrome();

    ui.page = lv_obj_create(ui.screen);
    lv_obj_set_size(ui.page, UI_W, ui.page_h);
    lv_obj_set_pos(ui.page, 0, TOP_H);
    lv_obj_set_style_bg_opa(ui.page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.page, 0, 0);
    lv_obj_set_style_radius(ui.page, 0, 0);
    lv_obj_set_style_pad_left(ui.page, 0, 0);
    lv_obj_set_style_pad_right(ui.page, 0, 0);
    lv_obj_set_style_pad_top(ui.page, 8, 0);
    lv_obj_set_style_pad_bottom(ui.page, 16, 0);
    lv_obj_set_scroll_dir(ui.page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui.page, LV_SCROLLBAR_MODE_OFF);

    pod_ui_rebuild();

    (void)pod_audio_set_volume_db((float)ui.volume_db);

    ui.timer = lv_timer_create(ui_timer_cb, 250, NULL);
    if (!ui.timer) {
        return ESP_ERR_NO_MEM;
    }
    lv_timer_create(boot_button_cb, 50, NULL);
    lv_timer_create(btn_button_cb, 50, NULL);

    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev && lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        indev = lv_indev_get_next(indev);
    }
    ui.indev = indev;
    if (indev) {
        lv_indev_add_event_cb(indev, indev_activity_cb, LV_EVENT_PRESSED, indev);
    }
    update_touch_state();

    ESP_LOGI(TAG, "UI ready: %zu queue slots, SD=%d, LittleFS=%d, "
             "internal free=%u largest=%u",
             (size_t)MAX_QUEUE, pod_storage_sd_ready(), pod_storage_littlefs_ready(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}






