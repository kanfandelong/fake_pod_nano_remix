/*
 * SPDX-License-Identifier: MIT
 *
 * Board-level I2S/DAC diagnostic tone.
 * This is intentionally separate from espaudiocore: it validates the
 * ESP32-S3 I2S master + CS43131 hardware path before the full player is wired.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2s_std.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_test_init(void);
esp_err_t audio_test_play_tone(uint32_t frequency_hz, uint32_t duration_ms);
esp_err_t audio_test_stop(void);
bool audio_test_is_playing(void);
i2s_chan_handle_t audio_test_i2s_handle(void);

#ifdef __cplusplus
}
#endif
