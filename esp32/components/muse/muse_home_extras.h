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

#include "lvgl.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Readouts in the face's top corners on the Waveshare 2.16
 * (CONFIG_MUSE_GADGET_HOME_EXTRAS): the time top left; battery and steps
 * toward the goal top right. The square screen's corners are outside the
 * bezel ring, so they cover neither Muse nor the captions. Both run in the
 * LVGL task (muse_ui.c).
 */
#if CONFIG_MUSE_GADGET_HOME_EXTRAS
void muse_home_extras_build(lv_obj_t *face);
/* Every frame while the screen is on; `now` in seconds. */
void muse_home_extras_tick(float now);
#else
static inline void muse_home_extras_build(lv_obj_t *face) { (void)face; }
static inline void muse_home_extras_tick(float now) { (void)now; }
#endif

#ifdef __cplusplus
}
#endif
