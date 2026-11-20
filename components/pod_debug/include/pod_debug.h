/*
 * SPDX-License-Identifier: MIT
 *
 * Runtime debug console over the USB-Serial-JTAG port.
 */

#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pod_debug_start(i2c_master_bus_handle_t i2c_bus);

#ifdef __cplusplus
}
#endif
