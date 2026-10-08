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
 * How the gadget behaves where it is: at the desk, at night or out and about.
 * Each change tells the Muse once, with a typed turn that sets how it should
 * answer (muse_hatch_text_turn), sent as soon as no voice turn is running.
 *
 * With the clock set (year 2025 or later), the schedule picks Night from 21:00
 * to 05:00 and Desk otherwise, checked every minute in local time. A mode
 * picked by hand (settings, the set_mode command) holds until the schedule's
 * next boundary. Joined to a network other than the saved home one, the face
 * offers On-the-go once per network; it never switches on its own.
 *
 * Night dims the screen and On-the-go turns it all the way up; neither speaks
 * replies (muse_gadget_tts_allowed).
 */

typedef enum {
    MUSE_GADGET_DESK,
    MUSE_GADGET_NIGHT,
    MUSE_GADGET_ON_THE_GO,
    MUSE_GADGET_MODE_COUNT,
} muse_gadget_mode_t;

/* Starts the schedule and the Muse messages; after muse_hatch_start(). */
void muse_gadget_mode_start(void);

/* All of these are safe from any task. */
muse_gadget_mode_t muse_gadget_mode(void);
/* By hand: holds until the next 05:00 or 21:00. */
void muse_gadget_mode_pick(muse_gadget_mode_t mode);
/* "desk", "night" or "on_the_go"; false for anything else. */
bool muse_gadget_mode_parse(const char *name, muse_gadget_mode_t *out);
/* "Desk", "Night", "On-the-go". */
const char *muse_gadget_mode_name(muse_gadget_mode_t mode);
/* Saves the network joined now as home; false if there isn't one. */
bool muse_gadget_mode_set_home(void);
/* Saves the network joined now as the On-the-go one: joining it switches to
 * On-the-go, leaving it goes back to the schedule. False if there isn't one. */
bool muse_gadget_mode_set_away(void);
/* Forgets the On-the-go network. */
void muse_gadget_mode_clear_away(void);

/* Whether replies may be spoken: not in Night or On-the-go. */
bool muse_gadget_tts_allowed(void);
/* The screen brightness for the mode, given the one set. */
int muse_gadget_mode_brightness(int pct);

/* LVGL task: the mode's chip in the face's status line, the On-the-go
 * suggestion on the top layer, and keeping both up to date. */
void muse_gadget_mode_build_chip(lv_obj_t *status);
void muse_gadget_mode_build_toast(lv_obj_t *layer, int w);
void muse_gadget_mode_ui_tick(float now);

#ifdef __cplusplus
}
#endif
