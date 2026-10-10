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
/* The turn the board's been read at lately, not taken yet (it waits to be
 * sure), or -1: for the face to wobble as it's about to go. */
int muse_orient_leaning(void);
#else
static inline int muse_orient_turn(float now) { (void)now; return 0; }
static inline int muse_orient_leaning(void) { return -1; }
#endif

/*
 * Which way the picture goes round on the panel for each turn up the count
 * (muse_board_t.set_turn): 1, a quarter clockwise, as the 2.16's MADCTL_TURN
 * has it (SIDEWAYS_SWAPPED 0): the board's gone a quarter the other way,
 * so the old picture's left edge is down (as SIDE_SIGN 1 has it: turn 1 with
 * +x, the picture's right, reading up). -1 if those two are flipped. Muse
 * falls that way as the screen turns (muse_tumble.h); wrong, and he'd fall up.
 */
#define MUSE_ORIENT_SPIN 1

#ifdef __cplusplus
}
#endif
