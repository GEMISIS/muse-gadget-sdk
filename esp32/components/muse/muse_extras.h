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
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The Waveshare 2.16's other peripherals (CONFIG_MUSE_GADGET_EXTRAS): the
 * microSD card (muse_sd.h), the PCF85063 clock and NTP (muse_rtc.h), the
 * QMI8658 IMU (muse_imu.h) and the face's corner readouts
 * (muse_home_extras.h). One low-priority task, its stack in internal RAM
 * since it writes NVS, polls them all.
 */
#if CONFIG_MUSE_GADGET_EXTRAS

/* After the board's init (I2C up), the UI and the input task: applies the time
 * zone, restores the time from the RTC and starts the task. */
void muse_extras_start(void);

/* The "gadget" NVS namespace, for the extras' own settings and counters. The
 * setters write flash: call them from a task whose stack is internal RAM. */
int32_t muse_extras_get_i32(const char *key, int32_t def);
void muse_extras_set_i32(const char *key, int32_t value);

/* The clock has been set (from the RTC or NTP): later than 2024. */
bool muse_time_valid(void);
/* Local time with strftime() `fmt`; false (and out empty) until it's set. */
bool muse_time_format(char *out, size_t cap, const char *fmt);

#else

static inline void muse_extras_start(void) {}

#endif

#ifdef __cplusplus
}
#endif
