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

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Which way up the Waveshare 2.16's screen should be (CONFIG_MUSE_GADGET_AUTO_FLIP):
 * the quarter turn that puts its picture upright however the board stands,
 * from the IMU; lying flat, as it was.
 */
#if CONFIG_MUSE_GADGET_AUTO_FLIP
/* LVGL task, every frame; `now` in seconds. 0 upright (keys on top), 2 upside
 * down, 1 and 3 on its sides (muse_board_t.set_turn). */
int muse_orient_turn(float now);
#else
static inline int muse_orient_turn(float now) { (void)now; return 0; }
#endif

#ifdef __cplusplus
}
#endif
