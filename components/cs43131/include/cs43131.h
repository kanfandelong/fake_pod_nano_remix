/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong.
 *
 * Minimal CS43131 control-port + I2S-slave configuration.
 *
 * The CS43131 uses a 5-byte I2C transaction for register writes:
 *   [MAP high] [MAP mid] [MAP low] [control byte] [data]
 * This is not a conventional 8-bit register address + data protocol.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 7-bit address: 0110000b (datasheet "Addr = 01100" + ADR = 0). */
#define CS43131_DEFAULT_I2C_ADDR 0x30

typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    uint8_t i2c_addr;          /* 7-bit address; default 0x18 */
    gpio_num_t reset_gpio;     /* active low reset */
    uint32_t sample_rate;      /* initial ASP rate: 32000..384000 */
    bool asp_master;           /* false: ESP32-S3 drives BCLK/LRCK (recommended) */
    float volume_db;           /* initial digital volume, 0 dB = unity */
} cs43131_config_t;

esp_err_t cs43131_init(const cs43131_config_t *config);
esp_err_t cs43131_deinit(void);

esp_err_t cs43131_set_sample_rate(uint32_t sample_rate);
esp_err_t cs43131_set_bit_depth(uint8_t bits);
esp_err_t cs43131_set_volume_db(float volume_db);
esp_err_t cs43131_set_mute(bool mute);

bool cs43131_is_ready(void);

/* Debug-only raw register access. Returns the 5-byte control-port result. */
esp_err_t cs43131_debug_read(uint32_t reg, uint8_t *value);
esp_err_t cs43131_debug_write(uint32_t reg, uint8_t value);

#ifdef __cplusplus
}
#endif
