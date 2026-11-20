/*
 * SPDX-License-Identifier: MIT
 *
 * Storage and folder-first music library access.
 *
 * Rules:
 *   - a folder's direct supported audio files form its playlist
 *   - nested folders are separate playlists, never merged into the parent
 *   - the default library root is /sdcard/music
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POD_STORAGE_SD_MOUNT       "/sdcard"
#define POD_STORAGE_LITTLEFS_MOUNT "/littlefs"
#define POD_STORAGE_MUSIC_ROOT     "/sdcard/music"
#define POD_STORAGE_CACHE_ROOT     "/sdcard/.cache"
#define POD_STORAGE_USER_DATA      "/littlefs/data"

typedef struct {
    char name[256];
    char path[512];
    uint32_t size;
    bool is_dir;
    bool is_audio;
} pod_dir_entry_t;

esp_err_t pod_storage_init(void);

bool pod_storage_sd_ready(void);
bool pod_storage_littlefs_ready(void);
const char *pod_storage_music_root(void);

esp_err_t pod_storage_mkdir_p(const char *path);
bool pod_storage_is_audio_file(const char *name);

/* Erase and recreate the internal LittleFS partition. This is intentionally
 * explicit: it destroys all user data under /littlefs. */
esp_err_t pod_storage_format_littlefs(void);

/* Capacity queries for the Settings > Storage page. */
esp_err_t pod_storage_sd_space(uint64_t *total, uint64_t *free_bytes);
esp_err_t pod_storage_littlefs_space(uint64_t *total, uint64_t *used);
esp_err_t pod_storage_dir_size(const char *path, uint64_t *bytes);

/* List direct children. Directories are returned before files, then by name.
 * The caller owns the returned array and frees it with pod_storage_free_entries. */
esp_err_t pod_storage_list_dir(const char *path, pod_dir_entry_t **entries,
                               size_t *count, size_t max_entries);
void pod_storage_free_entries(pod_dir_entry_t *entries);

#ifdef __cplusplus
}
#endif
