/*
 * SPDX-License-Identifier: MIT
 */

#include "pod_storage.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"

static const char *TAG = "pod_storage";

static bool s_sd_ready;
static bool s_littlefs_ready;
static sdmmc_card_t *s_card;

static bool has_audio_extension(const char *name)
{
    static const char *exts[] = {
        ".mp3", ".flac", ".wav", ".aac", ".m4a",
        ".ogg", ".opus", ".wv",
    };
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) {
        return false;
    }
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        if (strcasecmp(dot, exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool pod_storage_is_audio_file(const char *name)
{
    return name && has_audio_extension(name);
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase: %s", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t mount_sd(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = GPIO_NUM_16;
    slot.cmd = GPIO_NUM_17;
    slot.d0 = GPIO_NUM_15;
    slot.d1 = GPIO_NUM_14;
    slot.d2 = GPIO_NUM_21;
    slot.d3 = GPIO_NUM_18;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(POD_STORAGE_SD_MOUNT, &host, &slot,
                                            &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "microSD mount failed: %s", esp_err_to_name(err));
        s_card = NULL;
        return err;
    }

    s_sd_ready = true;
    ESP_LOGI(TAG, "microSD mounted at %s", POD_STORAGE_SD_MOUNT);
    return ESP_OK;
}

static esp_err_t mount_littlefs(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = POD_STORAGE_LITTLEFS_MOUNT,
        .partition_label = "littlefs",
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_littlefs_ready = true;
    ESP_LOGI(TAG, "LittleFS mounted at %s", POD_STORAGE_LITTLEFS_MOUNT);
    return ESP_OK;
}

esp_err_t pod_storage_mkdir_p(const char *path)
{
    if (!path || !*path) {
        return ESP_ERR_INVALID_ARG;
    }

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        struct stat st;
        if (stat(tmp, &st) != 0) {
            if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
                return ESP_FAIL;
            }
        } else if (!S_ISDIR(st.st_mode)) {
            return ESP_ERR_INVALID_STATE;
        }
        *p = '/';
    }

    struct stat st;
    if (stat(tmp, &st) != 0) {
        if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
            return ESP_FAIL;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t pod_storage_init(void)
{
    esp_err_t nvs_err = init_nvs();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(nvs_err));
        return nvs_err;
    }

    (void)mount_sd();
    (void)mount_littlefs();

    if (s_sd_ready) {
        (void)pod_storage_mkdir_p(POD_STORAGE_MUSIC_ROOT);
        (void)pod_storage_mkdir_p(POD_STORAGE_CACHE_ROOT);
        (void)pod_storage_mkdir_p(POD_STORAGE_CACHE_ROOT "/index");
        (void)pod_storage_mkdir_p(POD_STORAGE_CACHE_ROOT "/art");
    }
    if (s_littlefs_ready) {
        (void)pod_storage_mkdir_p(POD_STORAGE_USER_DATA);
    }

    return ESP_OK;
}

esp_err_t pod_storage_format_littlefs(void)
{
    if (s_littlefs_ready) {
        esp_err_t unreg = esp_vfs_littlefs_unregister("littlefs");
        if (unreg != ESP_OK && unreg != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "LittleFS unregister: %s", esp_err_to_name(unreg));
        }
        s_littlefs_ready = false;
    }

    esp_err_t err = esp_littlefs_format("littlefs");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS format failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mount_littlefs();
    if (err != ESP_OK) {
        return err;
    }
    (void)pod_storage_mkdir_p(POD_STORAGE_USER_DATA);
    ESP_LOGW(TAG, "LittleFS formatted; user data erased");
    return ESP_OK;
}

bool pod_storage_sd_ready(void)
{
    return s_sd_ready;
}

bool pod_storage_littlefs_ready(void)
{
    return s_littlefs_ready;
}

const char *pod_storage_music_root(void)
{
    return POD_STORAGE_MUSIC_ROOT;
}

static int entry_cmp(const void *a, const void *b)
{
    const pod_dir_entry_t *ea = a;
    const pod_dir_entry_t *eb = b;
    if (ea->is_dir != eb->is_dir) {
        return ea->is_dir ? -1 : 1;
    }
    return strcasecmp(ea->name, eb->name);
}

esp_err_t pod_storage_list_dir(const char *path, pod_dir_entry_t **entries,
                               size_t *count, size_t max_entries)
{
    if (!path || !entries || !count || max_entries == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *entries = NULL;
    *count = 0;

    DIR *dir = opendir(path);
    if (!dir) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Directory listings are large and short-lived.  Keep them in PSRAM and
     * never fall back to internal RAM: a 192-entry listing is ~145 KB and
     * would push the LVGL heap straight into fragmentation/OOM. */
    pod_dir_entry_t *arr = heap_caps_calloc(max_entries, sizeof(*arr),
                                            MALLOC_CAP_SPIRAM);
    if (!arr) {
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    size_t n = 0;
    struct dirent *ent;
    while (n < max_entries && (ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char full[512];
        int written = snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        if (written < 0 || written >= (int)sizeof(full)) {
            continue;
        }

        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }

        snprintf(arr[n].name, sizeof(arr[n].name), "%s", ent->d_name);
        snprintf(arr[n].path, sizeof(arr[n].path), "%s", full);
        arr[n].size = (uint32_t)st.st_size;
        arr[n].is_dir = S_ISDIR(st.st_mode);
        arr[n].is_audio = !arr[n].is_dir && has_audio_extension(ent->d_name);
        n++;
    }
    closedir(dir);

    qsort(arr, n, sizeof(*arr), entry_cmp);
    *entries = arr;
    *count = n;
    return ESP_OK;
}

void pod_storage_free_entries(pod_dir_entry_t *entries)
{
    if (entries) {
        heap_caps_free(entries);
    }
}

esp_err_t pod_storage_sd_space(uint64_t *total, uint64_t *free_bytes)
{
    if (!total || !free_bytes || !s_sd_ready) return ESP_ERR_INVALID_STATE;
    return esp_vfs_fat_info(POD_STORAGE_SD_MOUNT, total, free_bytes);
}

esp_err_t pod_storage_littlefs_space(uint64_t *total, uint64_t *used)
{
    if (!total || !used || !s_littlefs_ready) return ESP_ERR_INVALID_STATE;
    size_t t = 0, u = 0;
    esp_err_t err = esp_littlefs_info("littlefs", &t, &u);
    if (err != ESP_OK) return err;
    *total = t;
    *used = u;
    return ESP_OK;
}

esp_err_t pod_storage_dir_size(const char *path, uint64_t *bytes)
{
    if (!path || !bytes) return ESP_ERR_INVALID_ARG;
    *bytes = 0;

    DIR *dir = opendir(path);
    if (!dir) return ESP_ERR_NOT_FOUND;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char full[512];
        int written = snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        if (written < 0 || written >= (int)sizeof(full)) continue;

        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            uint64_t sub = 0;
            if (pod_storage_dir_size(full, &sub) == ESP_OK) *bytes += sub;
        } else {
            *bytes += (uint64_t)st.st_size;
        }
    }
    closedir(dir);
    return ESP_OK;
}
