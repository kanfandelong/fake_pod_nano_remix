/*
 * POD NANO 鈥?hardware bring-up + LVGL entry point.
 *
 * Stage 1: display/touch/LVGL sanity check. The full UI lives in
 * components/pod_ui and is attached once the panel is proven.
 */

#include <assert.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"

#include "esp_pm.h"

#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst820.h"

#include "lvgl.h"
#include "pod_display.h"
#include "cs43131.h"
#include "pod_debug.h"
#include "pod_storage.h"
#include "pod_font.h"
#include "pod_audio.h"
#include "pod_ui.h"

#include "co5300_init_cmds.h"

static const char *TAG = "pod_nano";

/* -------------------------------------------------------------------------- */
/* Hardware pin map (from the project pinmap)                                  */
/* -------------------------------------------------------------------------- */

#define LCD_CS_GPIO          GPIO_NUM_8
#define LCD_SCK_GPIO         GPIO_NUM_9
#define LCD_D0_GPIO          GPIO_NUM_10
#define LCD_D1_GPIO          GPIO_NUM_11
#define LCD_D2_GPIO          GPIO_NUM_12
#define LCD_D3_GPIO          GPIO_NUM_13
#define LCD_RST_GPIO         GPIO_NUM_7
#define LCD_TE_GPIO          GPIO_NUM_6

#define TOUCH_I2C_PORT       I2C_NUM_0
#define TOUCH_I2C_SCL        GPIO_NUM_2
#define TOUCH_I2C_SDA        GPIO_NUM_3
#define TOUCH_RST_GPIO       GPIO_NUM_5
#define TOUCH_INT_GPIO       GPIO_NUM_4
#define TOUCH_I2C_HZ         (400 * 1000)

#define LCD_HOST             SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ   (80 * 1000 * 1000)
#define LCD_H_RES            460
#define LCD_V_RES            460
#define LCD_X_GAP            10
#define LCD_Y_GAP            0
/* 24-bit RGB888, 24 lines = 33,120 bytes. This must stay small enough for
 * the SPI driver's internal DMA private TX buffer; 80 lines needs 110 KB and
 * fails on this board once the UI heap is populated. */
#define LCD_DRAW_BUF_LINES   24

static i2c_master_bus_handle_t s_i2c_bus;


esp_err_t pod_i2c_bus_init(void)
{
    if (s_i2c_bus) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = TOUCH_I2C_PORT,
        .scl_io_num = TOUCH_I2C_SCL,
        .sda_io_num = TOUCH_I2C_SDA,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
}

i2c_master_bus_handle_t pod_i2c_bus(void)
{
    return s_i2c_bus;
}

static esp_err_t display_touch_init(esp_lcd_panel_handle_t *panel_out,
                                    esp_lcd_panel_io_handle_t *io_out,
                                    esp_lcd_touch_handle_t *touch_out)
{
    ESP_RETURN_ON_ERROR(pod_i2c_bus_init(), TAG, "i2c bus");

    const spi_bus_config_t bus_config = CO5300_PANEL_BUS_QSPI_CONFIG(
        LCD_SCK_GPIO, LCD_D0_GPIO, LCD_D1_GPIO, LCD_D2_GPIO, LCD_D3_GPIO,
        LCD_H_RES * LCD_DRAW_BUF_LINES * 3);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO),
                        TAG, "spi bus");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_CS_GPIO,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 1,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                                &io_config, io_out),
                        TAG, "panel io");

    const co5300_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = lcd_init_cmds_size,
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        /* LVGL RGB888 is stored B,G,R; the CO5300 RGB888 path expects the
         * panel's BGR element order to compensate. */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 24,
        .reset_gpio_num = LCD_RST_GPIO,
        .vendor_config = (void *)&vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(*io_out, &panel_config, panel_out),
                        TAG, "co5300 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel_out), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel_out), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(*panel_out, LCD_X_GAP, LCD_Y_GAP),
                        TAG, "panel gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(*panel_out, true), TAG, "panel on");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_co5300_set_brightness(*panel_out, 80),
                        TAG, "brightness");

    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST820_CONFIG();
    tp_io_config.scl_speed_hz = TOUCH_I2C_HZ;
    esp_lcd_panel_io_handle_t tp_io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_config, &tp_io),
                        TAG, "touch io");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = TOUCH_RST_GPIO,
        .int_gpio_num = TOUCH_INT_GPIO,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst820(tp_io, &tp_cfg, touch_out),
                        TAG, "cst820");
    return ESP_OK;
}

static void touch_test_cb(lv_event_t *event)
{
    lv_obj_t *label = (lv_obj_t *)lv_event_get_user_data(event);
    static uint32_t taps;
    taps++;
    lv_label_set_text_fmt(label, "TOUCH OK  %u", (unsigned)taps);
    ESP_LOGI(TAG, "touch test tap #%u", (unsigned)taps);
}

static void build_bringup_screen(lv_display_t *disp)
{
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_clean(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x050506), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(scr, lv_color_hex(0x2A2A2E), 0);
    lv_obj_set_style_border_width(scr, 1, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "POD NANO");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF4F1EA), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 92);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "460 x 460  |  display + touch + LVGL");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x8A8882), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 132);

    /* Orientation / safe-area markers. */
    lv_obj_t *tl = lv_label_create(scr);
    lv_label_set_text(tl, "TL");
    lv_obj_set_style_text_color(tl, lv_color_hex(0x5E5E66), 0);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_14, 0);
    lv_obj_align(tl, LV_ALIGN_TOP_LEFT, 6, 4);

    lv_obj_t *br = lv_label_create(scr);
    lv_label_set_text(br, "BR");
    lv_obj_set_style_text_color(br, lv_color_hex(0x5E5E66), 0);
    lv_obj_set_style_text_font(br, &lv_font_montserrat_14, 0);
    lv_obj_align(br, LV_ALIGN_BOTTOM_RIGHT, -6, -4);

    lv_obj_t *dot = lv_obj_create(scr);
    lv_obj_set_size(dot, 54, 54);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0xFF7A67), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_align(dot, LV_ALIGN_CENTER, 0, 2);

    lv_obj_t *touch_label = lv_label_create(scr);
    lv_label_set_text(touch_label, "TOUCH TEST");
    lv_obj_set_style_text_color(touch_label, lv_color_hex(0x0B0B0C), 0);
    lv_obj_set_style_text_font(touch_label, &lv_font_montserrat_14, 0);
    lv_obj_center(touch_label);

    lv_obj_t *touch_btn = lv_button_create(scr);
    lv_obj_set_size(touch_btn, 220, 58);
    lv_obj_set_style_radius(touch_btn, 18, 0);
    lv_obj_set_style_bg_color(touch_btn, lv_color_hex(0xF4F1EA), 0);
    lv_obj_set_style_bg_opa(touch_btn, LV_OPA_COVER, 0);
    lv_obj_align(touch_btn, LV_ALIGN_CENTER, 0, 2);
    lv_obj_add_event_cb(touch_btn, touch_test_cb, LV_EVENT_CLICKED, touch_label);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Tap test: button must increment.");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x5E5E66), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -94);
}

void app_main(void)
{
    ESP_LOGI(TAG, "POD NANO starting on ESP-IDF %s", esp_get_idf_version());

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_touch_handle_t touch = NULL;
    ESP_ERROR_CHECK(display_touch_init(&panel, &panel_io, &touch));

    cs43131_config_t dac_cfg = {
        .i2c_bus = s_i2c_bus,
        .i2c_addr = 0, /* auto-detect 0x30/0x31 after reset */
        .reset_gpio = GPIO_NUM_41,
        .sample_rate = 48000,
        .asp_master = false,
        .volume_db = 0.0f,
    };
    esp_err_t storage_err = pod_storage_init();
    if (storage_err != ESP_OK) {
        ESP_LOGW(TAG, "storage init failed: %s", esp_err_to_name(storage_err));
    }

    esp_err_t font_err = pod_font_init();
    if (font_err != ESP_OK) {
        ESP_LOGW(TAG, "font partition unavailable: %s", esp_err_to_name(font_err));
    }

    esp_err_t dac_err = cs43131_init(&dac_cfg);
    if (dac_err != ESP_OK) {
        ESP_LOGW(TAG, "CS43131 init failed (%s); continuing without DAC",
                 esp_err_to_name(dac_err));
    }

    esp_err_t audio_err = pod_audio_init();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "audio init failed: %s", esp_err_to_name(audio_err));
    }

    lv_display_t *disp = NULL;
    ESP_ERROR_CHECK(pod_display_init(panel, panel_io, touch, &disp));

    esp_err_t ui_err = pod_ui_start(disp, panel);
    if (ui_err != ESP_OK) {
        ESP_LOGW(TAG, "pod_ui_start failed (%s); using bring-up screen",
                 esp_err_to_name(ui_err));
        build_bringup_screen(disp);
    }

    ESP_ERROR_CHECK(pod_display_start());
    ESP_LOGI(TAG, "UI ready");

    esp_err_t dbg_err = pod_debug_start(s_i2c_bus);
    if (dbg_err != ESP_OK) {
        ESP_LOGW(TAG, "debug console unavailable: %s", esp_err_to_name(dbg_err));
    }
    
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 80,
        .light_sleep_enable = true
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
}

