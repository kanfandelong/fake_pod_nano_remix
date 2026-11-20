/*
 * SPDX-License-Identifier: MIT
 *
 * Lightweight tag/cover reader for local audio files.
 *
 * Supported now:
 *   - MP3 with ID3v2.3 / ID3v2.4 (TIT2, TPE1, TALB, APIC)
 *   - FLAC Vorbis comments (TITLE, ARTIST, ALBUM) and PICTURE
 *
 * WMA/AAC/M4A metadata is intentionally left for a later parser.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char title[128];
    char artist[128];
    char album[128];
    /* Filesystem path (no LVGL drive prefix) of a JPEG cover, or empty. */
    char cover_path[256];
    /* Owned PSRAM lyric text, or NULL. */
    char *lyrics;
    size_t lyrics_len;
} pod_metadata_t;

esp_err_t pod_metadata_load(const char *audio_path, pod_metadata_t *meta);

#ifdef __cplusplus
}
#endif
