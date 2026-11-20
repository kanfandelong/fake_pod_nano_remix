/*
 * SPDX-License-Identifier: MIT
 *
 * CS43131 low-level control driver.
 *
 * References:
 *   - Cirrus Logic CS43131 datasheet DS1155F1, control port §4.13 and
 *     power-up-to-I2S example §5.13.1.
 *   - The board is assumed to provide a 22.5792 MHz crystal for the DAC.
 *     ESP32-S3 is the I2S master; CS43131 ASP is configured as slave.
 *
 * This is intentionally a small, auditable driver. It does not yet include
 * headphone impedance detection or DSD.
 */

#include "cs43131.h"

#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cs43131";

#define CS43131_I2C_TIMEOUT_MS 100
#define CS43131_CTRL_INCR      0x01

/* Register map (selected). */
#define REG_POWER           0x020000
#define REG_XTAL_SETTING    0x020052
#define REG_MCLK_CONFIG     0x010006
#define REG_SAMPLE_RATE     0x01000B
#define REG_SAMPLE_BITS     0x01000C
#define REG_PLL_PREDIV      0x040002
#define REG_PLL_OUTDIV      0x030008
#define REG_PLL_FRAC0       0x030002
#define REG_PLL_FRAC1       0x030003
#define REG_PLL_FRAC2       0x030004
#define REG_PLL_INT         0x030005
#define REG_PLL_MODE        0x03001B
#define REG_PLL_CAL         0x03000A
#define REG_PLL_START       0x030001
#define REG_ASP_CLOCK       0x040018
#define REG_ASP_FRAME       0x040019
#define REG_ASP_CH1_LOC     0x050000
#define REG_ASP_CH2_LOC     0x050001
#define REG_ASP_CH1_SIZE    0x05000A
#define REG_ASP_CH2_SIZE    0x05000B
#define REG_PCM_FILTER      0x090000
#define REG_PCM_VOL_B       0x090001
#define REG_PCM_VOL_A       0x090002
#define REG_PCM_PATH_CTRL1  0x090003
#define REG_PCM_PATH_CTRL2  0x090004
#define REG_HP_OUT_CTRL     0x080000
#define REG_CLASS_H         0x0B0000
#define REG_POP_FREE_A      0x010010
#define REG_POP_FREE_B      0x080032

typedef struct {
    i2c_master_dev_handle_t dev;
    bool ready;
    bool muted;
    bool use_pll;
    uint32_t rate;
    uint8_t volume_raw;
} cs43131_state_t;

static cs43131_state_t s_state;

static esp_err_t cs43131_write_reg(uint32_t reg, uint8_t value)
{
    if (!s_state.dev) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t payload[5] = {
        (uint8_t)((reg >> 16) & 0xFF),
        (uint8_t)((reg >> 8) & 0xFF),
        (uint8_t)(reg & 0xFF),
        CS43131_CTRL_INCR,
        value,
    };

    return i2c_master_transmit(s_state.dev, payload, sizeof(payload), CS43131_I2C_TIMEOUT_MS);
}

static esp_err_t cs43131_read_reg(uint32_t reg, uint8_t *value)
{
    if (!s_state.dev || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t preamble[4] = {
        (uint8_t)((reg >> 16) & 0xFF),
        (uint8_t)((reg >> 8) & 0xFF),
        (uint8_t)(reg & 0xFF),
        CS43131_CTRL_INCR,
    };

    /* CS43131's control port documents a STOP after the MAP/control
     * preamble for a fresh read address; some silicon does not tolerate a
     * repeated START here. Use two transactions and let the MAP address
     * persist on the device. */
    esp_err_t err = i2c_master_transmit(s_state.dev, preamble, sizeof(preamble),
                                        CS43131_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_master_receive(s_state.dev, value, 1, CS43131_I2C_TIMEOUT_MS);
}

static esp_err_t cs43131_write_checked(uint32_t reg, uint8_t value)
{
    esp_err_t err = ESP_FAIL;

    /* The control port can NACK briefly while an internal power/clock
     * transition is settling. Retry those transient failures, but never
     * hammer a real protocol error indefinitely. */
    for (int attempt = 0; attempt < 4; attempt++) {
        err = cs43131_write_reg(reg, value);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (err != ESP_ERR_INVALID_RESPONSE && err != ESP_ERR_TIMEOUT) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGE(TAG, "write 0x%05" PRIx32 " failed: %s", reg, esp_err_to_name(err));
    return err;
}

static uint8_t rate_code(uint32_t sample_rate)
{
    switch (sample_rate) {
    case 32000:  return 0;
    case 44100:  return 1;
    case 48000:  return 2;
    case 88200:  return 3;
    case 96000:  return 4;
    case 176400: return 5;
    case 192000: return 6;
    case 352800: return 7;
    case 384000: return 8;
    default:     return 0xFF;
    }
}

static uint8_t bits_code(uint8_t bits)
{
    switch (bits) {
    case 32: return 0x00;
    case 24: return 0x01;
    case 16: return 0x02;
    case 8:  return 0x03;
    default: return 0xFF;
    }
}

static bool cs43131_probe_addr(i2c_master_bus_handle_t bus, uint8_t addr)
{
    return i2c_master_probe(bus, addr, 10) == ESP_OK;
}

static uint8_t cs43131_find_addr(i2c_master_bus_handle_t bus)
{
    ESP_LOGI(TAG, "scanning I2C bus for CS43131...");
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (cs43131_probe_addr(bus, addr)) {
            ESP_LOGI(TAG, "  I2C device at 0x%02X", addr);
        }
    }

    /* ADR is a single pin on CS43131; the 7-bit base address is 0x30.
     * The scan above also exposes the touch controller (0x15) and the
     * QMI8658 IMU (0x6B), so do not guess from "first non-touch device". */
    if (cs43131_probe_addr(bus, 0x30)) return 0x30;
    if (cs43131_probe_addr(bus, 0x31)) return 0x31;
    return 0;
}

static esp_err_t cs43131_reset(gpio_num_t reset_gpio)
{
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(reset_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "reset gpio");

    /* Active low reset. Idle high, pulse low, then release. */
    gpio_set_level(reset_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(reset_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

static esp_err_t cs43131_configure_pll_22m5792(void)
{
    esp_err_t err;

    /* Table 4-4: 24.576 MHz XTAL -> PLL 22.5792 MHz.
     * PREDIV /8, INT 0x49, FRAC 0x800000, OUTDIV 0x0A, MODE 1, CAL 118. */
    err = cs43131_write_checked(REG_PLL_PREDIV, 0x03);
    if (err != ESP_OK) return err;
    err = cs43131_write_checked(REG_PLL_OUTDIV, 0x0A);
    if (err != ESP_OK) return err;
    err = cs43131_write_checked(REG_PLL_FRAC0, 0x00); /* LSB */
    if (err != ESP_OK) return err;
    err = cs43131_write_checked(REG_PLL_FRAC1, 0x00); /* middle */
    if (err != ESP_OK) return err;
    err = cs43131_write_checked(REG_PLL_FRAC2, 0x80); /* MSB */
    if (err != ESP_OK) return err;
    err = cs43131_write_checked(REG_PLL_INT, 0x49);
    if (err != ESP_OK) return err;
    /* PLL Setting 8: bit1 = PLL_MODE, bit0 is reserved-default-1.
     * Table 4-4 requires PLL_MODE = 1 for the 22.5792 MHz case. */
    err = cs43131_write_checked(REG_PLL_MODE, 0x03);
    if (err != ESP_OK) return err;
    err = cs43131_write_checked(REG_PLL_CAL, 118);
    if (err != ESP_OK) return err;
    err = cs43131_write_checked(REG_PLL_START, 0x01);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

static esp_err_t cs43131_power_up_sequence(uint32_t sample_rate)
{
    esp_err_t err;

    /* The board has a 24.576 MHz crystal. The datasheet allows MCLK_INT to
     * be either 22.5792 or 24.576 MHz for every rate up to 192 kHz. Keeping
     * the PLL at 22.5792 MHz for all rates means normal track changes never
     * have to re-run this power sequence while I2S is streaming. */
    const bool use_pll = true;

    if (sample_rate > 192000) {
        ESP_LOGE(TAG, "unsupported rate: %" PRIu32 " Hz", sample_rate);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 1. XTAL bias. Datasheet Example 5-7: 0x04 (12.5 uA). */
    err = cs43131_write_checked(REG_XTAL_SETTING, 0x04);
    if (err != ESP_OK) return err;

    /* Clear a pending interrupt before starting the crystal. */
    uint8_t scratch = 0;
    (void)cs43131_read_reg(0xF0000, &scratch);

    /* 2. Start XTAL. Keep ASP/HP/PLL/CLKOUT down.
     * 0xF6 = 1111 0110b: bit3 (PDN_XTAL) = 0, bit2 (PDN_PLL) = 1.
     * Do NOT use 0xFA here: its bit3=1 powers the XTAL down and bit2=0
     * powers the PLL up, leaving the ASP without an MCLK. */
    err = cs43131_write_checked(REG_POWER, 0xF6);
    if (err != ESP_OK) return err;
    /* Crystal startup is not instantaneous; the control port can NACK while
     * the internal power state machine is busy. */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Start PLL, keep CLKOUT off. */
    err = cs43131_write_checked(REG_POWER, 0xF2);
    if (err != ESP_OK) return err;
    err = cs43131_configure_pll_22m5792();
    if (err != ESP_OK) return err;
    /* PLL mode, MCLK_INT = 22.5792 MHz. */
    err = cs43131_write_checked(REG_MCLK_CONFIG, 0x05);
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(50));

    /* 4. ASP: 32-bit I2S, slave. The ESP32-S3 supplies BCLK/LRCK. */
    uint8_t rc = rate_code(sample_rate);
    if (rc == 0xFF) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    err = cs43131_write_checked(REG_SAMPLE_RATE, rc);
    if (err != ESP_OK) return err;
    err = cs43131_write_checked(REG_SAMPLE_BITS, 0x04); /* ASP = 32-bit */
    if (err != ESP_OK) return err;
    err = cs43131_write_checked(REG_ASP_CLOCK, 0x0C);  /* slave, I2S */
    if (err != ESP_OK) return err;
    err = cs43131_write_checked(REG_ASP_FRAME, 0x0A);
    if (err != ESP_OK) return err;
    err = cs43131_write_checked(REG_ASP_CH1_LOC, 0x00);
    if (err != ESP_OK) return err;
    err = cs43131_write_checked(REG_ASP_CH2_LOC, 0x00);
    if (err != ESP_OK) return err;
    err = cs43131_write_checked(REG_ASP_CH1_SIZE, 0x07);
    if (err != ESP_OK) return err;
    err = cs43131_write_checked(REG_ASP_CH2_SIZE, 0x0F);
    if (err != ESP_OK) return err;

    /* 5. PCM path: HPF, 0 dB, soft ramp, volume controlled by A. */
    err = cs43131_write_checked(REG_PCM_FILTER, 0x02);
    if (err != ESP_OK) return err;
    err = cs43131_write_checked(REG_PCM_VOL_B, 0x00);
    if (err != ESP_OK) return err;
    err = cs43131_write_checked(REG_PCM_VOL_A, 0x00);
    if (err != ESP_OK) return err;
    /* Datasheet Example 5-7: PCM_RAMP_DOWN + VOL_BEQA + SZC + AMUTE,
     * with both PCM_MUTE bits clear. 0xEC = 1110 1100b. */
    err = cs43131_write_checked(REG_PCM_PATH_CTRL1, 0xEC);
    if (err != ESP_OK) return err;
    err = cs43131_write_checked(REG_PCM_PATH_CTRL2, 0x00);
    if (err != ESP_OK) return err;

    /* 6. Power ASP (bit6 = 0), keep the active clock source up, HP down.
     * 0xB2 = PLL path; 0xB6 = direct-XTAL path. */
    err = cs43131_write_checked(REG_POWER, use_pll ? 0xB2 : 0xB6);
    if (err != ESP_OK) return err;

    /* 7. HP output: 1 Vrms, class-H high voltage mode. */
    err = cs43131_write_checked(REG_HP_OUT_CTRL, 0x10);
    if (err != ESP_OK) return err;
    err = cs43131_write_checked(REG_CLASS_H, 0x1E);
    if (err != ESP_OK) return err;

    /* 8. Pop-free sequence. */
    err = cs43131_write_checked(REG_POP_FREE_A, 0x99);
    if (err != ESP_OK) return err;
    err = cs43131_write_checked(REG_POP_FREE_B, 0x20);
    if (err != ESP_OK) return err;

    /* 9. Power HP (bit4 = 0), keep clock + ASP up.
     * 0xA2 = PLL path; 0xA6 = direct-XTAL path. */
    err = cs43131_write_checked(REG_POWER, use_pll ? 0xA2 : 0xA6);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(22));

    err = cs43131_write_checked(REG_POP_FREE_A, 0x00);
    if (err != ESP_OK) return err;
    err = cs43131_write_checked(REG_POP_FREE_B, 0x00);
    if (err != ESP_OK) return err;

    /* 10. Unmute. */
    err = cs43131_write_checked(REG_PCM_PATH_CTRL1, 0xEC);
    if (err != ESP_OK) return err;

    s_state.muted = false;
    s_state.use_pll = use_pll;
    s_state.rate = sample_rate;
    ESP_LOGI(TAG, "clock: %s, MCLK_INT=%s, ASP slave, %" PRIu32 " Hz",
             use_pll ? "PLL 22.5792 MHz" : "XTAL 24.576 MHz direct",
             use_pll ? "22.5792 MHz" : "24.576 MHz", sample_rate);
    return ESP_OK;
}

esp_err_t cs43131_init(const cs43131_config_t *config)
{
    if (!config || !config->i2c_bus) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_state, 0, sizeof(s_state));
    s_state.volume_raw = 0x00;

    if (config->reset_gpio != GPIO_NUM_NC) {
        esp_err_t err = cs43131_reset(config->reset_gpio);
        if (err != ESP_OK) return err;
    }

    uint8_t addr = config->i2c_addr;
    if (!addr) {
        addr = cs43131_find_addr(config->i2c_bus);
        if (!addr) {
            ESP_LOGE(TAG, "CS43131 not found on I2C bus");
            return ESP_ERR_NOT_FOUND;
        }
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(config->i2c_bus, &dev_cfg, &s_state.dev),
                        TAG, "add i2c device");

    ESP_LOGI(TAG, "using control port address 0x%02X", addr);

    /* Do not issue a read-preamble before the first write. Probe already
     * verified the address; a read would leave the MAP pointer in a state
     * that complicates the first power-up write on some silicon. */
    uint32_t rate = config->sample_rate ? config->sample_rate : 48000;
    esp_err_t err = cs43131_power_up_sequence(rate);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "power-up sequence failed: %s", esp_err_to_name(err));
        i2c_master_bus_rm_device(s_state.dev);
        s_state.dev = NULL;
        return err;
    }

    s_state.ready = true;
    ESP_LOGI(TAG, "ready: 32-bit I2S, %" PRIu32 " Hz, ASP slave", rate);
    return ESP_OK;
}

esp_err_t cs43131_deinit(void)
{
    if (!s_state.dev) return ESP_OK;
    esp_err_t err = i2c_master_bus_rm_device(s_state.dev);
    s_state.dev = NULL;
    s_state.ready = false;
    s_state.muted = false;
    return err;
}

bool cs43131_is_ready(void)
{
    return s_state.ready;
}

esp_err_t cs43131_set_sample_rate(uint32_t sample_rate)
{
    if (!s_state.ready) return ESP_ERR_INVALID_STATE;
    if (sample_rate > 192000) return ESP_ERR_NOT_SUPPORTED;

    /* MCLK stays at PLL 22.5792 MHz for all supported rates, so a track
     * change only needs the ASP rate register. Do not re-run the power
     * sequence while I2S is streaming. */
    uint8_t code = rate_code(sample_rate);
    if (code == 0xFF) return ESP_ERR_NOT_SUPPORTED;
    s_state.rate = sample_rate;
    return cs43131_write_reg(REG_SAMPLE_RATE, code);
}

esp_err_t cs43131_set_bit_depth(uint8_t bits)
{
    uint8_t code = bits_code(bits);
    if (code == 0xFF) return ESP_ERR_NOT_SUPPORTED;
    return cs43131_write_reg(REG_SAMPLE_BITS, code);
}

esp_err_t cs43131_set_volume_db(float volume_db)
{
    if (!s_state.ready) return ESP_ERR_INVALID_STATE;

    /* CS43131 PCM volume is 0.5 dB/step, 0x00 = 0 dB, 0x7F = +63.5 dB?
     * Keep a conservative -63.5..0 dB range in this first driver. */
    /* 0x00 = 0 dB, 0x01 = -0.5 dB, ... 0xFE = -127 dB, 0xFF = mute. */
    int steps = (int)(-volume_db * 2.0f + 0.5f);
    if (steps < 0) steps = 0;
    if (steps > 0xFE) steps = 0xFE;
    s_state.volume_raw = (uint8_t)steps;

    esp_err_t err = cs43131_write_reg(REG_PCM_VOL_A, s_state.volume_raw);
    if (err != ESP_OK) return err;
    return cs43131_write_reg(REG_PCM_VOL_B, s_state.volume_raw);
}

esp_err_t cs43131_set_mute(bool mute)
{
    if (!s_state.ready) return ESP_ERR_INVALID_STATE;
    s_state.muted = mute;
    return cs43131_write_reg(REG_PCM_PATH_CTRL1, mute ? 0xEF : 0xEC);
}

esp_err_t cs43131_debug_read(uint32_t reg, uint8_t *value)
{
    if (!s_state.ready) return ESP_ERR_INVALID_STATE;
    return cs43131_read_reg(reg, value);
}

esp_err_t cs43131_debug_write(uint32_t reg, uint8_t value)
{
    if (!s_state.ready) return ESP_ERR_INVALID_STATE;
    return cs43131_write_checked(reg, value);
}
