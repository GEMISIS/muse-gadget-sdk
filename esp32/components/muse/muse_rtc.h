/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Time on the Waveshare 2.16 (CONFIG_MUSE_GADGET_RTC): its PCF85063 at 0x51
 * on the shared I2C bus keeps UTC across power-offs on the PMU's backup rail.
 * At boot it sets the system clock; once Wi-Fi is up SNTP takes over, and
 * every sync is written back to the RTC. Also the daily alarm
 * (CONFIG_MUSE_GADGET_ALARM).
 */

/* Reads the RTC and sets the system clock if the RTC's time is good. */
esp_err_t muse_rtc_init(i2c_master_bus_handle_t bus);

/* Once a second from the extras task: starts SNTP when Wi-Fi first connects,
 * writes each sync back to the RTC, and sounds the alarm when it's due. */
void muse_rtc_tick(void);

#if CONFIG_MUSE_GADGET_ALARM
/* The daily alarm's time, in minutes after local midnight, and whether it's
 * on (*on may be NULL). Any task. */
int muse_rtc_alarm(bool *on);
/* Sets it and saves it in the "gadget" NVS namespace ("alarm_min",
 * "alarm_on"): from a task whose stack is internal RAM (the LVGL one). It
 * rings at the new time from the next minute on. */
void muse_rtc_set_alarm(int minute, bool on);
#endif

#ifdef __cplusplus
}
#endif
