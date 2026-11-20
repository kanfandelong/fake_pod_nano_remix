/*
 * SPDX-License-Identifier: MIT
 *
 * Debug shell modelled on LiClock's Serial_cmd.cpp. The output macros,
 * colours, table headers, and field ordering intentionally follow that file.
 * Commands that do not exist in the reference (DAC/tone/coredump/i2c) are
 * added only as extra utilities and use the same PRINT_* macros.
 */

#include "pod_debug.h"

#include <ctype.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "cs43131.h"
#include "pod_ui.h"
#include "driver/i2c_master.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_debug_helpers.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc.h"

static const char *TAG = "pod_debug";
static i2c_master_bus_handle_t s_i2c_bus;

/* -------------------------------------------------------------------------- */
/* LiClock Serial_cmd.h colour/macros                                         */
/* -------------------------------------------------------------------------- */

#define ERROR_COLOR     "\033[91m"
#define WARNING_COLOR   "\033[93m"
#define SUCCESS_COLOR   "\033[92m"
#define INFO_COLOR      "\033[96m"
#define HEADER_COLOR    "\033[95m"
#define RESET_COLOR     "\033[0m"

#define PRINT_ERROR(fmt, ...)           \
    do {                                \
        printf(ERROR_COLOR fmt RESET_COLOR "\n", ##__VA_ARGS__); \
    } while (0)

#define PRINT_WARNING(fmt, ...)         \
    do {                                \
        printf(WARNING_COLOR fmt RESET_COLOR "\n", ##__VA_ARGS__); \
    } while (0)

#define PRINT_SUCCESS(fmt, ...)         \
    do {                                \
        printf(SUCCESS_COLOR fmt RESET_COLOR "\n", ##__VA_ARGS__); \
    } while (0)

#define PRINT_INFO(fmt, ...)            \
    do {                                \
        printf(INFO_COLOR fmt RESET_COLOR "\n", ##__VA_ARGS__); \
    } while (0)

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static uint32_t parse_u32(const char *s, bool *ok)
{
    char *end = NULL;
    unsigned long value = strtoul(s, &end, 0);
    if (!s || !*s || !end || *end != '\0') {
        *ok = false;
        return 0;
    }
    *ok = true;
    return (uint32_t)value;
}

static void strip_trailing_slash(char *path)
{
    if (!path || !*path) return;
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
        len--;
    }
}

static void uint32_to_bin_str(uint32_t val, char *buf, size_t buf_size)
{
    if (buf_size < 40) return;
    int idx = 0;
    for (int i = 31; i >= 0; i--) {
        buf[idx++] = (val & (1U << i)) ? '1' : '0';
        if (i % 4 == 0 && i != 0) {
            buf[idx++] = ' ';
        }
    }
    buf[idx] = '\0';
}

/* -------------------------------------------------------------------------- */
/* heap                                                                       */
/* -------------------------------------------------------------------------- */

static void print_mem_caps_info(uint32_t caps, const char *caps_name)
{
    multi_heap_info_t info;
    size_t total = heap_caps_get_total_size(caps);
    heap_caps_get_info(&info, caps);

    printf("%s Memory Info:\n", caps_name);
    printf("------------------------------------------\n");
    printf("  Total Size        : %8lu B (%6.1f KB)\n",
           (unsigned long)total, (float)total / 1024.0f);
    printf("  Free Bytes        : %8lu B (%6.1f KB)\n",
           (unsigned long)info.total_free_bytes, (float)info.total_free_bytes / 1024.0f);
    printf("  Allocated Bytes   : %8lu B (%6.1f KB)\n",
           (unsigned long)info.total_allocated_bytes,
           (float)info.total_allocated_bytes / 1024.0f);
    printf("  Minimum Free Bytes: %8lu B (%6.1f KB)\n",
           (unsigned long)info.minimum_free_bytes,
           (float)info.minimum_free_bytes / 1024.0f);
    printf("  Largest Free Block: %8lu B (%6.1f KB)\n",
           (unsigned long)info.largest_free_block,
           (float)info.largest_free_block / 1024.0f);
}

static int cmd_heap(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    print_mem_caps_info(MALLOC_CAP_INTERNAL, "INTERNAL");
    print_mem_caps_info(MALLOC_CAP_SPIRAM, "SPIRAM");
    print_mem_caps_info(MALLOC_CAP_DMA, "DMA");
    print_mem_caps_info(MALLOC_CAP_8BIT, "8BIT");
    return 0;
}

static int cmd_heapdump(int argc, char **argv)
{
    uint32_t caps = MALLOC_CAP_DEFAULT;
    if (argc >= 2) {
        if (strcmp(argv[1], "internal") == 0) {
            caps = MALLOC_CAP_INTERNAL;
        } else if (strcmp(argv[1], "spiram") == 0) {
            caps = MALLOC_CAP_SPIRAM;
        } else if (strcmp(argv[1], "dma") == 0) {
            caps = MALLOC_CAP_DMA;
        } else {
            bool ok = false;
            caps = parse_u32(argv[1], &ok);
            if (!ok) {
                PRINT_ERROR("Unknown heap type: %s", argv[1]);
                return 1;
            }
        }
    }
    PRINT_INFO("Dumping heap caps=0x%08" PRIx32, caps);
    heap_caps_print_heap_info(caps);
    PRINT_SUCCESS("Dump completed");
    return 0;
}

/* -------------------------------------------------------------------------- */
/* tasklist / taskload                                                        */
/* -------------------------------------------------------------------------- */

static int cmd_tasklist(int argc, char **argv)
{
    (void)argc;
    (void)argv;
#if (configUSE_TRACE_FACILITY == 1 && configUSE_STATS_FORMATTING_FUNCTIONS == 1)
    const size_t buffer_size = 2048;
    char *buffer = malloc(buffer_size);
    if (!buffer) {
        PRINT_ERROR("Memory allocation failed.");
        return 2;
    }
    vTaskList(buffer);

    printf(HEADER_COLOR "Task List\n" RESET_COLOR);
    PRINT_INFO("Task name       State   Prio    Stack   Num     Core");
    printf("--------------- ------- ------- ------- ------- ----\n");
    printf("%s", buffer);
    free(buffer);
    return 0;
#else
    PRINT_ERROR("vTaskList requires configUSE_TRACE_FACILITY and configUSE_STATS_FORMATTING_FUNCTIONS");
    return 2;
#endif
}

static int cmd_taskload(int argc, char **argv)
{
#if (configGENERATE_RUN_TIME_STATS == 1 && configUSE_TRACE_FACILITY == 1)
    int window_sec = 5;
    if (argc >= 2) {
        window_sec = atoi(argv[1]);
        if (window_sec <= 0 || window_sec > 60) {
            PRINT_ERROR("Invalid window. Use 1-60 seconds.");
            return 1;
        }
    }
    PRINT_INFO("Measuring CPU load over the last %d seconds...", window_sec);

    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    if (task_count == 0) {
        PRINT_INFO("No tasks running.");
        return 0;
    }

    TaskStatus_t *snap1 = malloc(task_count * sizeof(TaskStatus_t));
    if (!snap1) {
        PRINT_ERROR("Memory allocation failed.");
        return 2;
    }

    uint32_t total_time1 = 0;
    UBaseType_t count1 = uxTaskGetSystemState(snap1, task_count, &total_time1);

    vTaskDelay(pdMS_TO_TICKS(window_sec * 1000));

    UBaseType_t max_count2 = uxTaskGetNumberOfTasks();
    TaskStatus_t *snap2 = malloc(max_count2 * sizeof(TaskStatus_t));
    if (!snap2) {
        free(snap1);
        PRINT_ERROR("Memory allocation failed.");
        return 2;
    }

    uint32_t total_time2 = 0;
    UBaseType_t count2 = uxTaskGetSystemState(snap2, max_count2, &total_time2);

    unsigned long elapsed = total_time2 - total_time1;
    if (elapsed == 0) {
        PRINT_WARNING("Elapsed time zero, stats unavailable.");
        free(snap1);
        free(snap2);
        return 0;
    }

    size_t max_name_len = 4;
    for (UBaseType_t i = 0; i < count2; i++) {
        size_t len = strlen(snap2[i].pcTaskName);
        if (len > max_name_len) max_name_len = len;
    }
    max_name_len += 2;

    printf("\n--- CPU Load in last %d seconds ---\n", window_sec);
    printf("%-*s %5s %5s %5s %5s %5s %10s %10s %9s\n",
           (int)max_name_len, "Name",
           "Core", "Num", "State", "Prio", "WDT", "Stack", "Time(us)", "Load%");
    printf("%-*s ----- ----- ----- ----- ----- ---------- ---------- ---------\n",
           (int)max_name_len, "----");

    const char state[6][2] = {"X", "R", "B", "S", "D", "?"};
    const int state_count = sizeof(state) / sizeof(state[0]);

    for (UBaseType_t j = 0; j < count2; j++) {
        const TaskStatus_t *t2 = &snap2[j];
        unsigned long run_time1 = 0;
        for (UBaseType_t i = 0; i < count1; i++) {
            if (snap1[i].xHandle == t2->xHandle) {
                run_time1 = snap1[i].ulRunTimeCounter;
                break;
            }
        }

        unsigned long delta = t2->ulRunTimeCounter - run_time1;
        float percent = ((float)delta * 100.0f) / (float)elapsed;

        const char *class_color;
        if (strncmp(t2->pcTaskName, "IDLE", 4) == 0) {
            class_color = "\033[36m";
        } else if (percent >= 90.0f) {
            class_color = "\033[91m";
        } else if (percent >= 70.0f) {
            class_color = "\033[31m";
        } else if (percent >= 30.0f) {
            class_color = "\033[33m";
        } else if (percent >= 5.0f) {
            class_color = "\033[32m";
        } else if (percent >= 1.0f) {
            class_color = "\033[37m";
        } else {
            class_color = "\033[90m";
        }

        const char *state_str =
            (t2->eCurrentState < (eTaskState)state_count) ? state[t2->eCurrentState] : "?";
        esp_err_t wdt_ret = esp_task_wdt_status(t2->xHandle);
        const char *wdt_str = "    -";
        if (wdt_ret == ESP_OK) {
            wdt_str = "    +";
        } else if (wdt_ret == ESP_ERR_NOT_FOUND) {
            wdt_str = "    -";
        }
        int core = (t2->xCoreID == tskNO_AFFINITY) ? -1 : (int)t2->xCoreID;

        printf("%s%-*s %5d %5u %5s %2u/%2u %5s %10lu %10lu %8.02f%%\033[0m\n",
               class_color,
               (int)max_name_len, t2->pcTaskName,
               core,
               t2->xTaskNumber,
               state_str,
               t2->uxBasePriority,
               t2->uxCurrentPriority,
               wdt_str,
               (unsigned long)t2->usStackHighWaterMark,
               delta,
               percent);
    }

    free(snap1);
    free(snap2);
    return 0;
#else
    PRINT_ERROR("Requires configGENERATE_RUN_TIME_STATS and configUSE_TRACE_FACILITY");
    return 2;
#endif
}

/* -------------------------------------------------------------------------- */
/* backtrace                                                                  */
/* -------------------------------------------------------------------------- */

static int cmd_backtrace(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const char *name = pcTaskGetName(NULL);
    esp_backtrace_frame_t fr = {0};
    esp_backtrace_get_start(&fr.pc, &fr.sp, &fr.next_pc);
    printf("%s (current task)\n", name);
    esp_backtrace_print_from_frame(50, &fr, false);
    return 0;
}

static int cmd_backtrace_all(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("All task backtraces:\n");
    esp_backtrace_print_all_tasks(50);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* espreg                                                                     */
/* -------------------------------------------------------------------------- */

static int cmd_espreg(int argc, char **argv)
{
    if (argc < 3) {
        return 1;
    }

    bool is_read;
    if (strcmp(argv[1], "-r") == 0) {
        is_read = true;
    } else if (strcmp(argv[1], "-w") == 0) {
        is_read = false;
    } else {
        PRINT_ERROR("Invalid operation: must be -r or -w");
        return 1;
    }

    char *endptr;
    uint32_t addr = strtoul(argv[2], &endptr, 0);
    if (*endptr != '\0') {
        PRINT_ERROR("Invalid address: %s", argv[2]);
        return 1;
    }
    if (addr & 0x3) {
        PRINT_WARNING("Address 0x%08" PRIx32 " is not 32-bit aligned, access may cause exception", addr);
    }

    int bit_pos = -1;
    int value_arg_idx = 3;
    if (argc >= 5 && strcmp(argv[3], "-bit") == 0) {
        bit_pos = atoi(argv[4]);
        if (bit_pos < 0 || bit_pos > 31) {
            PRINT_ERROR("Bit position must be between 0 and 31");
            return 1;
        }
        value_arg_idx = 5;
    }

    if (is_read) {
        if (argc != value_arg_idx) {
            return 1;
        }
        uint32_t val = *(volatile uint32_t *)(uintptr_t)addr;
        if (bit_pos >= 0) {
            PRINT_INFO("Register 0x%08" PRIx32 " bit %d = %d", addr, bit_pos,
                       (int)((val >> bit_pos) & 1));
        } else {
            char bin_str[64];
            uint32_to_bin_str(val, bin_str, sizeof(bin_str));
            PRINT_INFO("Register 0x%08" PRIx32 " = 0x%08" PRIx32 " (bin: %s)", addr, val, bin_str);
        }
        return 0;
    }

    if (argc != value_arg_idx + 1) {
        return 1;
    }
    uint32_t new_val = strtoul(argv[value_arg_idx], &endptr, 0);
    if (*endptr != '\0') {
        PRINT_ERROR("Invalid value: %s", argv[value_arg_idx]);
        return 1;
    }

    uint32_t old_val = *(volatile uint32_t *)(uintptr_t)addr;
    uint32_t write_val;
    if (bit_pos >= 0) {
        if (new_val != 0 && new_val != 1) {
            PRINT_ERROR("Bit value must be 0 or 1 when using -bit");
            return 1;
        }
        write_val = old_val;
        if (new_val) {
            write_val |= (1U << bit_pos);
        } else {
            write_val &= ~(1U << bit_pos);
        }
        PRINT_INFO("Modifying bit %d of register 0x%08" PRIx32 " from %d to %d",
                   bit_pos, addr, (int)((old_val >> bit_pos) & 1), (int)new_val);
    } else {
        write_val = new_val;
        PRINT_INFO("Writing 0x%08" PRIx32 " to register 0x%08" PRIx32 " (old: 0x%08" PRIx32 ")",
                   write_val, addr, old_val);
    }

    *(volatile uint32_t *)(uintptr_t)addr = write_val;
    uint32_t verify_val = *(volatile uint32_t *)(uintptr_t)addr;
    if (verify_val == write_val) {
        PRINT_SUCCESS("Register updated successfully");
    } else {
        PRINT_ERROR("Register write verification failed! Read back 0x%08" PRIx32, verify_val);
        return 2;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* hexdump / md                                                               */
/* -------------------------------------------------------------------------- */

static void hexdump_bytes(const uint8_t *buf, size_t len, uint32_t base)
{
    size_t addr = 0;
    while (addr < len) {
        size_t actual = len - addr;
        if (actual > 16) actual = 16;

        printf("%s0x%08" PRIX32 ":%s ", INFO_COLOR, base + (uint32_t)addr, RESET_COLOR);
        for (size_t i = 0; i < 16; i++) {
            if (i < actual) {
                printf("%02X ", buf[addr + i]);
            } else {
                printf("   ");
            }
            if (i == 7) printf(" ");
        }
        printf(" |");
        for (size_t i = 0; i < actual; i++) {
            uint8_t c = buf[addr + i];
            printf("%c", (c >= 0x20 && c <= 0x7E) ? c : '.');
        }
        printf("|\n");
        addr += actual;
    }
}

static int cmd_md(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        PRINT_ERROR("Usage: md <addr> [bytes]");
        return 1;
    }
    bool ok = false;
    uint32_t addr = parse_u32(argv[1], &ok);
    uint32_t len = 64;
    if (!ok) {
        PRINT_ERROR("Invalid address: %s", argv[1]);
        return 1;
    }
    if (argc == 3) {
        len = parse_u32(argv[2], &ok);
        if (!ok || len == 0 || len > 1024) {
            PRINT_ERROR("Length must be 1..1024");
            return 1;
        }
    }
    hexdump_bytes((const uint8_t *)(uintptr_t)addr, len, addr);
    return 0;
}

static int cmd_hexdump(int argc, char **argv)
{
    if (argc < 2 || argc > 4) {
        PRINT_ERROR("Usage: hexdump <file> [offset] [length]");
        return 1;
    }

    const char *filepath = argv[1];
    size_t offset = 0;
    size_t length = 0;
    bool has_length = false;
    bool ok = true;

    if (argc >= 3) {
        offset = parse_u32(argv[2], &ok);
        if (!ok) {
            PRINT_ERROR("Invalid offset: %s", argv[2]);
            return 1;
        }
    }
    if (argc >= 4) {
        length = parse_u32(argv[3], &ok);
        if (!ok || length == 0) {
            PRINT_ERROR("Invalid length: %s", argv[3]);
            return 1;
        }
        has_length = true;
    }

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        PRINT_ERROR("Failed to open file '%s'", filepath);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);
    if (file_size < 0 || offset > (size_t)file_size) {
        PRINT_ERROR("Offset exceeds file size");
        fclose(f);
        return 2;
    }
    size_t remaining = (size_t)file_size - offset;
    if (has_length && length < remaining) {
        remaining = length;
    }
    fseek(f, (long)offset, SEEK_SET);

    uint8_t buf[256];
    size_t addr = offset;
    while (remaining > 0) {
        size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
        size_t got = fread(buf, 1, want, f);
        if (!got) break;
        hexdump_bytes(buf, got, (uint32_t)addr);
        addr += got;
        remaining -= got;
    }
    fclose(f);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* file operations                                                            */
/* -------------------------------------------------------------------------- */

static void list_dir(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        PRINT_ERROR("Cannot open directory: %s", path);
        return;
    }

    printf("Directory: %s\n", path);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0) {
            PRINT_WARNING("Cannot stat: %s", full_path);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            printf("\033[36m[DIR] %s\033[0m\n", entry->d_name);
        } else if (S_ISREG(st.st_mode)) {
            printf("[FILE] %s (%u bytes)\n", entry->d_name, (unsigned)st.st_size);
        } else {
            printf("[UNKN] %s\n", entry->d_name);
        }
    }
    closedir(dir);
}

static int cmd_ls(int argc, char **argv)
{
    if (argc == 1) {
        list_dir("/");
        struct stat st;
        if (stat("/sdcard", &st) == 0 && S_ISDIR(st.st_mode)) {
            list_dir("/sdcard");
        }
        if (stat("/littlefs", &st) == 0 && S_ISDIR(st.st_mode)) {
            list_dir("/littlefs");
        }
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        strip_trailing_slash(argv[i]);
        list_dir(argv[i]);
    }
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        return 1;
    }

    const size_t buf_size = 4096;
    uint8_t buffer[buf_size];
    for (int i = 1; i < argc; i++) {
        strip_trailing_slash(argv[i]);
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            PRINT_ERROR("Cannot open file: %s", argv[i]);
            continue;
        }
        size_t total = 0;
        size_t len;
        while ((len = fread(buffer, 1, buf_size, f)) > 0) {
            size_t written = fwrite(buffer, 1, len, stdout);
            if (written != len) {
                PRINT_ERROR("Write error while reading %s", argv[i]);
                break;
            }
            total += len;
        }
        fclose(f);
        if (total == 0) {
            PRINT_WARNING("File is empty: %s", argv[i]);
        }
    }
    fflush(stdout);
    return 0;
}

static int cmd_stat(int argc, char **argv)
{
    if (argc != 2) {
        PRINT_ERROR("Usage: stat <path>");
        return 1;
    }
    struct stat st;
    if (stat(argv[1], &st) != 0) {
        PRINT_ERROR("Cannot stat: %s", argv[1]);
        return 2;
    }
    PRINT_INFO("Path: %s", argv[1]);
    PRINT_INFO("  Type: %s", S_ISDIR(st.st_mode) ? "directory" : "file");
    PRINT_INFO("  Size: %" PRId64, (int64_t)st.st_size);
    PRINT_INFO("  Mode: 0%o", (unsigned)st.st_mode);
    return 0;
}

static int cmd_df(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const char *mounts[] = {"/littlefs", "/sdcard", "/"};
    for (size_t i = 0; i < sizeof(mounts) / sizeof(mounts[0]); i++) {
        struct statvfs vfs;
        if (statvfs(mounts[i], &vfs) != 0) {
            PRINT_WARNING("%s not mounted", mounts[i]);
            continue;
        }
        uint64_t total = (uint64_t)vfs.f_blocks * vfs.f_frsize;
        uint64_t freeb = (uint64_t)vfs.f_bavail * vfs.f_frsize;
        uint64_t used = total - freeb;
        PRINT_INFO("%s FILESYSTEM INFORMATION:", mounts[i]);
        PRINT_INFO("  Total space: %5" PRIu64 " KB", total / 1024);
        PRINT_INFO("  Used space:  %5" PRIu64 " KB (%.02f%%)",
                   used / 1024, total ? (float)used * 100.0f / (float)total : 0.0f);
        PRINT_INFO("  Free space:  %5" PRIu64 " KB", freeb / 1024);
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* extra board commands                                                       */
/* -------------------------------------------------------------------------- */

static int cmd_i2c(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!s_i2c_bus) {
        PRINT_ERROR("I2C bus not available");
        return 2;
    }
    PRINT_INFO("Scanning I2C bus...");
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_i2c_bus, addr, 10) == ESP_OK) {
            PRINT_INFO("  I2C device at 0x%02X", addr);
        }
    }
    return 0;
}

static int cmd_dac(int argc, char **argv)
{
    if (argc < 2) {
        PRINT_ERROR("Usage: dac r <reg> | dac w <reg> <value>");
        return 1;
    }
    if (strcmp(argv[1], "r") == 0 && argc == 3) {
        bool ok = false;
        uint32_t reg = parse_u32(argv[2], &ok);
        if (!ok) {
            PRINT_ERROR("Invalid register: %s", argv[2]);
            return 1;
        }
        uint8_t value = 0;
        esp_err_t err = cs43131_debug_read(reg, &value);
        if (err != ESP_OK) {
            PRINT_ERROR("DAC read failed: %s", esp_err_to_name(err));
            return 2;
        }
        PRINT_INFO("DAC[0x%05" PRIx32 "] = 0x%02X", reg, value);
        return 0;
    }
    if (strcmp(argv[1], "w") == 0 && argc == 4) {
        bool ok1 = false, ok2 = false;
        uint32_t reg = parse_u32(argv[2], &ok1);
        uint32_t value = parse_u32(argv[3], &ok2);
        if (!ok1 || !ok2 || value > 0xFF) {
            PRINT_ERROR("Invalid register/value");
            return 1;
        }
        esp_err_t err = cs43131_debug_write(reg, (uint8_t)value);
        if (err != ESP_OK) {
            PRINT_ERROR("DAC write failed: %s", esp_err_to_name(err));
            return 2;
        }
        PRINT_SUCCESS("DAC[0x%05" PRIx32 "] <- 0x%02" PRIx32, reg, value);
        return 0;
    }
    PRINT_ERROR("Usage: dac r <reg> | dac w <reg> <value>");
    return 1;
}

static int cmd_chipinfo(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BASE);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    PRINT_INFO("CHIP INFORMATION:");
    PRINT_INFO("  Model:             %s", CONFIG_IDF_TARGET);
    PRINT_INFO("  Revision:          %.2f", (float)info.revision / 100.0f);
    PRINT_INFO("  Cores:             %u", info.cores);
    PRINT_INFO("  MAC:               %02X:%02X:%02X:%02X:%02X:%02X",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    PRINT_INFO("  Flash size:        %" PRIu32 " MB", flash_size / (1024 * 1024));
    PRINT_INFO("  IDF:               %s", esp_get_idf_version());
    PRINT_INFO("  Reset reason:      %d", (int)esp_reset_reason());
    PRINT_INFO("  Uptime:            %" PRIu64 " ms",
               (uint64_t)(esp_timer_get_time() / 1000));
    return 0;
}

static int cmd_coredump(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!p) {
        PRINT_ERROR("Coredump partition not found");
        return 2;
    }
    PRINT_INFO("COREDUMP PARTITION:");
    PRINT_INFO("  Label:             %s", p->label);
    PRINT_INFO("  Address:           0x%08" PRIx32, p->address);
    PRINT_INFO("  Size:              0x%" PRIx32 " (%" PRIu32 " KB)",
               p->size, p->size / 1024);
    return 0;
}

static int cmd_b64(int argc, char **argv)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    if (argc < 2) {
        PRINT_ERROR("Usage: b64 <file>");
        return 1;
    }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) {
        PRINT_ERROR("Cannot open %s", argv[1]);
        return 2;
    }

    printf("-----BEGIN-B64-----\n");
    uint8_t in[57];
    size_t n;
    while ((n = fread(in, 1, sizeof(in), fp)) > 0) {
        for (size_t i = 0; i < n; i += 3) {
            uint32_t v = (uint32_t)in[i] << 16;
            if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
            if (i + 2 < n) v |= in[i + 2];
            putchar(table[(v >> 18) & 0x3F]);
            putchar(table[(v >> 12) & 0x3F]);
            putchar(i + 1 < n ? table[(v >> 6) & 0x3F] : '=');
            putchar(i + 2 < n ? table[v & 0x3F] : '=');
        }
        putchar('\n');
    }
    printf("-----END-B64-----\n");
    fclose(fp);
    fflush(stdout);
    return 0;
}

static int cmd_coverraw(int argc, char **argv)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    (void)argc;
    (void)argv;

    size_t len = 0;
    uint16_t w = 0, h = 0;
    const uint8_t *data = pod_ui_cover_data(&len, &w, &h);
    if (!data || len == 0) {
        PRINT_ERROR("No decoded cover");
        return 2;
    }
    PRINT_INFO("cover raw %ux%u, %u bytes", w, h, (unsigned)len);

    printf("-----BEGIN-B64-----\n");
    for (size_t i = 0; i < len; i += 57) {
        size_t n = len - i;
        if (n > 57) n = 57;
        for (size_t j = 0; j < n; j += 3) {
            uint32_t v = (uint32_t)data[i + j] << 16;
            if (j + 1 < n) v |= (uint32_t)data[i + j + 1] << 8;
            if (j + 2 < n) v |= data[i + j + 2];
            putchar(table[(v >> 18) & 0x3F]);
            putchar(table[(v >> 12) & 0x3F]);
            putchar(j + 1 < n ? table[(v >> 6) & 0x3F] : '=');
            putchar(j + 2 < n ? table[v & 0x3F] : '=');
        }
        putchar('\n');
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    printf("-----END-B64-----\n");
    fflush(stdout);
    return 0;
}

static int cmd_echo(int argc, char **argv)
{
    if (argc < 2) {
        printf("\n");
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        printf("%s%s", argv[i], i + 1 == argc ? "" : " ");
    }
    printf("\n");
    return 0;
}

static int cmd_uitab(int argc, char **argv)
{
    if (argc < 2) {
        PRINT_ERROR("Usage: uitab <0-4>");
        return 1;
    }
    pod_ui_show_tab(atoi(argv[1]));
    return 0;
}

static int cmd_uiqueue(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    pod_ui_show_queue();
    return 0;
}

static int cmd_uiplay(int argc, char **argv)
{
    if (argc < 2) {
        PRINT_ERROR("Usage: uiplay <eligible-index>");
        return 1;
    }
    pod_ui_play_index(atoi(argv[1]));
    return 0;
}

static int cmd_uiplayname(int argc, char **argv)
{
    if (argc < 2) {
        PRINT_ERROR("Usage: uiplayname <substring>");
        return 1;
    }
    pod_ui_play_path_contains(argv[1]);
    return 0;
}

static int cmd_uinext(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    pod_ui_next_track();
    return 0;
}

static int cmd_uitouch(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    pod_ui_show_touch_cal();
    return 0;
}

static int cmd_uitopall(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    pod_ui_show_top_list();
    return 0;
}

static int cmd_uifav(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    pod_ui_toggle_favorite();
    return 0;
}

static int cmd_uifavlib(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    pod_ui_show_favorites();
    return 0;
}

static int cmd_uilyrics(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    pod_ui_toggle_lyrics();
    return 0;
}

static int cmd_uirepeat(int argc, char **argv)
{
    if (argc != 2) {
        PRINT_ERROR("Usage: uirepeat <0|1|2>  (0=off, 1=all, 2=one)");
        return 1;
    }
    bool ok = true;
    uint32_t mode = parse_u32(argv[1], &ok);
    if (!ok || mode > 2) {
        PRINT_ERROR("Repeat mode must be 0, 1 or 2");
        return 1;
    }
    pod_ui_set_repeat((int)mode);
    PRINT_SUCCESS("Repeat mode set to %u", (unsigned)mode);
    return 0;
}

static int cmd_uiautosave(int argc, char **argv)
{
    if (argc < 2) {
        PRINT_ERROR("Usage: uiautosave <minutes>");
        return 1;
    }
    pod_ui_set_autosave(atoi(argv[1]));
    return 0;
}

static int cmd_uistorage(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    pod_ui_show_storage();
    return 0;
}

static int cmd_uiautooff(int argc, char **argv)
{
    if (argc < 2) {
        PRINT_ERROR("Usage: uiautooff <seconds>");
        return 1;
    }
    pod_ui_set_auto_off(atoi(argv[1]));
    return 0;
}

static int cmd_uisave(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    pod_ui_save_data();
    return 0;
}

static int cmd_uicovermode(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    pod_ui_toggle_cover_only();
    return 0;
}

static int cmd_uiscreen(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    pod_ui_screen_toggle();
    return 0;
}

static int cmd_uinav(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    pod_ui_toggle_nav();
    return 0;
}

static int cmd_uifiles(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    pod_ui_show_files_root();
    return 0;
}

static int cmd_uiscroll(int argc, char **argv)
{
    if (argc < 2) {
        PRINT_ERROR("Usage: uiscroll <y>");
        return 1;
    }
    pod_ui_scroll_list(atoi(argv[1]));
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    PRINT_INFO("Restarting device...");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
}

/* -------------------------------------------------------------------------- */
/* registration                                                               */
/* -------------------------------------------------------------------------- */

static const esp_console_cmd_t s_commands[] = {
    {.command = "heap", .help = "Show heap usage", .hint = NULL, .func = cmd_heap},
    {.command = "heapdump", .help = "Dump heap structure", .hint = "heapdump [internal|spiram|dma|caps]", .func = cmd_heapdump},
    {.command = "tasklist", .help = "Print task list and stack info", .hint = NULL, .func = cmd_tasklist},
    {.command = "tasks", .help = "Alias for tasklist", .hint = NULL, .func = cmd_tasklist},
    {.command = "taskload", .help = "Show CPU load over the last N seconds", .hint = "taskload [seconds]", .func = cmd_taskload},
    {.command = "backtrace", .help = "Print current task backtrace", .hint = NULL, .func = cmd_backtrace},
    {.command = "backtrace_all", .help = "Print all task backtraces", .hint = NULL, .func = cmd_backtrace_all},
    {.command = "espreg", .help = "Read or write a register (with optional bit)", .hint = "espreg <-r|-w> <address> [-bit <bit>] [value]", .func = cmd_espreg},
    {.command = "md", .help = "Memory dump", .hint = "md <addr> [bytes]", .func = cmd_md},
    {.command = "hexdump", .help = "Hexdump a file", .hint = "hexdump <file> [offset] [length]", .func = cmd_hexdump},
    {.command = "ls", .help = "List a directory", .hint = "ls [path]", .func = cmd_ls},
    {.command = "cat", .help = "Print file contents", .hint = "cat <path> [path...]", .func = cmd_cat},
    {.command = "stat", .help = "Show file status", .hint = "stat <path>", .func = cmd_stat},
    {.command = "df", .help = "Show filesystem usage", .hint = NULL, .func = cmd_df},
    {.command = "i2c", .help = "Scan I2C bus", .hint = NULL, .func = cmd_i2c},
    {.command = "dac", .help = "CS43131 register access", .hint = "dac r <reg> | dac w <reg> <value>", .func = cmd_dac},
    {.command = "chipinfo", .help = "Show chip information", .hint = NULL, .func = cmd_chipinfo},
    {.command = "uitab", .help = "Switch UI tab (debug)", .hint = "uitab <0-4>", .func = cmd_uitab},
    {.command = "uiqueue", .help = "Open the Up Next overlay (debug)", .hint = NULL, .func = cmd_uiqueue},
    {.command = "uiplay", .help = "Play an index from the current list (debug)", .hint = "uiplay <n>", .func = cmd_uiplay},
    {.command = "uiplayname", .help = "Play a path substring (debug)", .hint = "uiplayname <text>", .func = cmd_uiplayname},
    {.command = "uinext", .help = "Next track (debug)", .hint = NULL, .func = cmd_uinext},
    {.command = "uistorage", .help = "Open Settings > Storage", .hint = NULL, .func = cmd_uistorage},
    {.command = "uitouch", .help = "Open Settings > Touch calibration", .hint = NULL, .func = cmd_uitouch},
    {.command = "uirepeat", .help = "Set repeat mode", .hint = "uirepeat <0|1|2>", .func = cmd_uirepeat},
    {.command = "uitopall", .help = "Open full most-played list", .hint = NULL, .func = cmd_uitopall},
    {.command = "uifav", .help = "Toggle favorite for current track", .hint = NULL, .func = cmd_uifav},
    {.command = "uifavlib", .help = "Open Favorites library", .hint = NULL, .func = cmd_uifavlib},
    {.command = "uilyrics", .help = "Toggle lyrics view", .hint = NULL, .func = cmd_uilyrics},
    {.command = "uiautosave", .help = "Set LittleFS autosave minutes", .hint = "uiautosave <min>", .func = cmd_uiautosave},
    {.command = "uiautooff", .help = "Set auto screen-off seconds", .hint = "uiautooff <sec>", .func = cmd_uiautooff},
    {.command = "uisave", .help = "Save settings/playback data now", .hint = NULL, .func = cmd_uisave},
    {.command = "uicovermode", .help = "Toggle pure cover mode", .hint = NULL, .func = cmd_uicovermode},
    {.command = "uiscreen", .help = "Toggle screen power", .hint = NULL, .func = cmd_uiscreen},
    {.command = "uinav", .help = "Toggle the bottom nav (debug)", .hint = NULL, .func = cmd_uinav},
    {.command = "uifiles", .help = "Open /sdcard in Files (debug)", .hint = NULL, .func = cmd_uifiles},
    {.command = "uiscroll", .help = "Scroll the active list (debug)", .hint = "uiscroll <y>", .func = cmd_uiscroll},
    {.command = "coredump", .help = "Show coredump partition", .hint = NULL, .func = cmd_coredump},
    {.command = "b64", .help = "Dump a file as base64", .hint = "b64 <file>", .func = cmd_b64},
    {.command = "coverraw", .help = "Dump the current decoded cover", .hint = NULL, .func = cmd_coverraw},
    {.command = "echo", .help = "Echo arguments", .hint = "echo [text ...]", .func = cmd_echo},
    {.command = "reboot", .help = "Restart the device", .hint = NULL, .func = cmd_reboot},
};

esp_err_t pod_debug_start(i2c_master_bus_handle_t i2c_bus)
{
    s_i2c_bus = i2c_bus;

    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "pod>";
    repl_cfg.max_cmdline_length = 512;
    repl_cfg.max_cmdline_args = 16;
    repl_cfg.task_stack_size = 6144;
    repl_cfg.task_priority = 5;
    repl_cfg.task_core_id = 0;

    esp_console_dev_usb_serial_jtag_config_t dev_cfg = {};
    esp_console_repl_t *repl = NULL;
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB Serial/JTAG REPL init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_console_register_help_command());
    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_console_cmd_register(&s_commands[i]));
    }

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "REPL start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "debug console ready on USB Serial/JTAG");
    return ESP_OK;
}



