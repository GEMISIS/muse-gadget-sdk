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

/*
 * A freeze becomes a restart with a reason (the 2.16's; elsewhere these do
 * nothing). The face beats every frame while it's awake, and the screen's
 * sleep and wake mark their start and end. An esp_timer checks them: a face
 * that stopped drawing while awake, or a sleep or wake that never finished,
 * aborts, so the core dump (in flash) has every task's backtrace, and the
 * reason is kept for the next boot's log (and ">crash").
 */

#include <stdbool.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_MUSE_BOARD_WAVESHARE_S3_216
/* Starts the checks; logs why the last boot ended. */
void muse_watchdog_start(void);
/* The face drew a frame (the LVGL task). */
void muse_watchdog_beat(void);
/* The screen's sleep (pause) or wake starts (true) or is done (false). */
void muse_watchdog_screen(bool busy);
/* Prints the last reset's reason, and a stall's, as "@crash {...}". */
void muse_watchdog_print(void);
#else
static inline void muse_watchdog_start(void) {}
static inline void muse_watchdog_beat(void) {}
static inline void muse_watchdog_screen(bool busy) { (void)busy; }
static inline void muse_watchdog_print(void) {}
#endif

#ifdef __cplusplus
}
#endif
