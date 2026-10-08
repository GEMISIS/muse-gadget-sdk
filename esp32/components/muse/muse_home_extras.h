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
 * Readouts on the face on the Waveshare 2.16 (CONFIG_MUSE_GADGET_HOME_EXTRAS):
 * the time at the top, above the state, and the battery in the bottom right
 * corner, with the "up next" line above it (CONFIG_MUSE_GADGET_UP_NEXT). They
 * run in the LVGL task (muse_ui.c).
 */
#if CONFIG_MUSE_GADGET_HOME_EXTRAS
void muse_home_extras_build(lv_obj_t *face);
/* Every frame while the screen is on; `now` in seconds. */
void muse_home_extras_tick(float now);
/* 24-hour clock (21:30) or 12-hour (9:30 PM, the default), kept in NVS.
 * The setter writes flash: call it from the LVGL task. */
bool muse_home_extras_24h(void);
void muse_home_extras_set_24h(bool on);
/* The clock label, for muse_ui.c to hide while a reply takes the top. */
lv_obj_t *muse_home_extras_clock(void);
/* The battery's corner, hidden while a reply's page runs over it. */
lv_obj_t *muse_home_extras_corner(void);
/* The "up next" line's box above it (CONFIG_MUSE_GADGET_UP_NEXT), likewise;
 * NULL without it. */
lv_obj_t *muse_home_extras_up_next(void);
/* The Night face's big clock (CONFIG_MUSE_GADGET_NIGHT_FACE), or the usual. */
void muse_home_extras_set_night(bool night);
#else
static inline void muse_home_extras_build(lv_obj_t *face) { (void)face; }
static inline void muse_home_extras_tick(float now) { (void)now; }
static inline lv_obj_t *muse_home_extras_clock(void) { return NULL; }
static inline lv_obj_t *muse_home_extras_corner(void) { return NULL; }
static inline lv_obj_t *muse_home_extras_up_next(void) { return NULL; }
static inline bool muse_home_extras_24h(void) { return false; }
static inline void muse_home_extras_set_24h(bool on) { (void)on; }
static inline void muse_home_extras_set_night(bool night) { (void)night; }
#endif

#ifdef __cplusplus
}
#endif
