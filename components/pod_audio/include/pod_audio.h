/*
 * SPDX-License-Identifier: MIT
 *
 * Thin application layer over espaudiocore. The audio core remains the
 * source of truth for decoding; this component only tracks UI-facing state.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POD_AUDIO_IDLE = 0,
    POD_AUDIO_PLAYING,
    POD_AUDIO_PAUSED,
    POD_AUDIO_ERROR,
} pod_audio_state_t;

esp_err_t pod_audio_init(void);

esp_err_t pod_audio_play(const char *path);
esp_err_t pod_audio_pause(void);
esp_err_t pod_audio_resume(void);
esp_err_t pod_audio_stop(void);
esp_err_t pod_audio_seek_ms(int64_t ms);
esp_err_t pod_audio_set_volume_db(float db);

pod_audio_state_t pod_audio_state(void);
bool pod_audio_is_running(void);
int64_t pod_audio_position_ms(void);
int64_t pod_audio_duration_ms(void);
esp_err_t pod_audio_get_format(uint32_t *rate, uint8_t *channels, uint8_t *bits);
const char *pod_audio_current_path(void);
const char *pod_audio_last_error(void);

/* One-shot natural end-of-stream notification (manual stop does not set it). */
bool pod_audio_take_eos(void);
void pod_audio_clear_eos(void);

typedef struct {
    bool valid;
    char title[128];
    char artist[128];
    char album[128];
    char *lyrics;
    size_t lyrics_len;
    uint8_t *cover;
    size_t cover_len;
    char mime[32];
    int pic_type;
} pod_audio_metadata_t;

/* Parse metadata with the decoder's own on_meta interface, without playback.
 * Cover bytes are copied to PSRAM and stay valid until probe_clear()/next probe. */
esp_err_t pod_audio_probe(const char *path);
const pod_audio_metadata_t *pod_audio_probe_meta(void);
void pod_audio_probe_clear(void);

#ifdef __cplusplus
}
#endif
