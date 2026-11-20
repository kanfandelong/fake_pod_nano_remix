/*
 * SPDX-FileCopyrightText: 2015-2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <sys/reent.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "soc/soc.h"
#include "esp_rom_sys.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"

static const char *TAG = "boot";

typedef struct
{
    int8_t partition; // 低8位：目标分区 (-1, 0, 1, 2...)
    uint8_t flags;    // 中8位：控制标志位
    uint16_t magic;   // 高16位：魔数 0x5A5A
} __attribute__((packed)) bootctl_fields_t;

typedef union
{
    uint32_t reg;
    bootctl_fields_t fields;
} bootctl_t;

// 定义标志位（仅占用一个字节，留出7个扩展位）
#define BOOTCTL_FLAG_PERSIST (1 << 0) // 1: 持久生效（直到RTC域复位/POR），0: 一次性生效

bool get_boot_target_from_rtc(int *out_target)
{
    if (out_target == NULL)
        return false;
    bootctl_t ctl;
    ctl.reg = REG_READ(0x60008050);

    if (ctl.fields.magic != 0x5A5A)
    {
        return false; // 无有效指令
    }
    if (ctl.fields.partition < 16 && ctl.fields.partition > -3)
    {
        *out_target = ctl.fields.partition;
    }
    else
    {
        REG_WRITE(0x60008050, 0);
        return false; // 分区不合法
    }
    // 一次性模式清除寄存器
    if ((ctl.fields.flags & BOOTCTL_FLAG_PERSIST) == 0)
    {
        REG_WRITE(0x60008050, 0);
    }
    return true;
}

static int select_partition_number(bootloader_state_t *bs);

/*
 * We arrive here after the ROM bootloader finished loading this second stage bootloader from flash.
 * The hardware is mostly uninitialized, flash cache is down and the app CPU is in reset.
 * We do have a stack, so we can do the initialization in C.
 */
void __attribute__((noreturn)) call_start_cpu0(void)
{
    // 1. Hardware initialization
    if (bootloader_init() != ESP_OK)
    {
        bootloader_reset();
    }

    ESP_LOGI(TAG, "\n\n"
                  "© 2025 - 2026 看番の龙 | 自定义bootloader\n"
                  "           Powered by 看番の龙\n"
                  "         github.com/kanfandelong\n");
#ifdef CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP
    bootctl_t ctl;
    ctl.reg = REG_READ(0x60008050);
    bool rtc_valid = (ctl.fields.magic == 0x5A5A);

    // 如果 RTC 有有效指令，则不走快速路径，让后续正常流程处理
    if (!rtc_valid)
    {
        // If this boot is a wake up from the deep sleep then go to the short way,
        // try to load the application which worked before deep sleep.
        // It skips a lot of checks due to it was done before (while first boot).
        bootloader_utility_load_boot_image_from_deep_sleep();
        // If it is not successful try to load an application as usual.
    }
    else
    {
        ESP_LOGI(TAG, "RTC override detected, bypassing deep sleep fast path");
    }
#endif

    // 2. Select the number of boot partition
    bootloader_state_t bs = {0};
    int boot_index = select_partition_number(&bs);
    if (boot_index == INVALID_INDEX)
    {
        bootloader_reset();
    }

    int rtc_target;
    if (get_boot_target_from_rtc(&rtc_target))
    {
        boot_index = rtc_target; // 用寄存器值覆盖
    }

    // 3. Load the app image for booting
    bootloader_utility_load_boot_image(&bs, boot_index);
}

// Select the number of boot partition
static int select_partition_number(bootloader_state_t *bs)
{
    // 1. Load partition table
    if (!bootloader_utility_load_partition_table(bs))
    {
        ESP_LOGE(TAG, "load partition table error!");
        return INVALID_INDEX;
    }

    // 2. Select the number of boot partition
    return bootloader_utility_get_selected_boot_partition(bs);
}

#if CONFIG_LIBC_NEWLIB
// Return global reent struct if any newlib functions are linked to bootloader
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
#endif
