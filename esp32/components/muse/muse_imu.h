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
 * The Waveshare 2.16's QMI8658 (CONFIG_MUSE_GADGET_IMU), at 0x6B on the
 * shared I2C bus. The accelerometer runs alone at 125 Hz with the chip's own
 * tap and pedometer engines; the extras task polls it (no interrupt pins):
 *   - double tap: wakes the screen
 *   - picked up or tilted (CONFIG_MUSE_GADGET_MOTION_WAKE): wakes the screen
 *   - shaken hard while idle: an earthquake on the face (muse_ui_quake):
 *     Muse jitters about and goes dizzy for a moment
 *   - steps: the chip's count, reset at local midnight, kept in NVS
 */

esp_err_t muse_imu_init(i2c_master_bus_handle_t bus);

/* From the extras task: reads the chip and acts on what it sees. Returns how
 * long to wait before the next call, in ms: shorter while the screen is on. */
int muse_imu_poll(void);

/* Once a second from the extras task: the day's rollover and saving the count. */
void muse_imu_tick(void);

#if CONFIG_MUSE_GADGET_IMU
/* Today's steps, or -1 without the IMU. Any task. */
int muse_imu_steps(void);
/* The acceleration in g, on the chip's axes, when it last lay still for a
 * second and a half: which way up it is, as the axis pointing up reads +1 g.
 * False until it has. Any task. */
bool muse_imu_gravity(float out[3]);
#else
static inline int muse_imu_steps(void) { return -1; }
static inline bool muse_imu_gravity(float out[3]) { (void)out; return false; }
#endif

#ifdef __cplusplus
}
#endif
