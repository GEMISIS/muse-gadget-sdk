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

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The passcode's keypad (muse_lock.h): the whole screen, slid up over the
 * locked face, or over Settings to set, change or turn off the passcode. A
 * title, six dots that fill as digits go in, and a 3 x 4 keypad of round keys;
 * in the bottom left Muse (unlocking) or Cancel (in Settings). A wrong
 * passcode shakes the dots empty and says how many tries are left; locked out,
 * the keys dim under a countdown; before the final round, a dialog warns that
 * it erases. Swiped down, or left alone, it goes back to the locked face.
 * All of it runs in the LVGL task.
 */

/* Onto the top layer, under the power menu and the sleep cover. */
void muse_lock_ui_build(lv_obj_t *layer, int w, int h);
/* Every frame, asleep or not. */
void muse_lock_ui_tick(float now);

/* To unlock; `animate` slides it up. */
void muse_lock_ui_open(bool animate);
void muse_lock_ui_close(void);
/* It's on screen (or on its way). */
bool muse_lock_ui_up(void);

/* Muse in the keypad's corner while unlocking, for muse_ui.c to draw into
 * (its source the face's own); NULL while he isn't there. */
lv_obj_t *muse_lock_ui_avatar(void);
/* How Muse takes it: dizzy (0..1) after a wrong one, heavy-lidded (0..1)
 * while digits go in, so as not to look. */
void muse_lock_ui_mood(float now, float *dizzy, float *tired);

/* Settings' flows: `done` with whether it was done (not cancelled). */
typedef void (*muse_lock_ui_done_t)(bool ok);
void muse_lock_ui_set_pin(muse_lock_ui_done_t done);      /* a new one, twice */
void muse_lock_ui_change_pin(muse_lock_ui_done_t done);   /* the current one, then a new one twice */
void muse_lock_ui_turn_off(muse_lock_ui_done_t done);     /* the current one */

#ifdef __cplusplus
}
#endif
