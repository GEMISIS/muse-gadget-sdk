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
 * Like a system prompt per chat: each chat (the main one, the gadget's, each
 * named one) remembers the mode it last heard (muse_settings_chat_told), and
 * the next message sent to a chat that heard another one, typed or spoken,
 * carries the mode's contract after the words (muse_gadget_mode_context). A
 * change sends nothing by itself.
 *
 * With the clock set (year 2025 or later), the schedule picks Night from its
 * start to its end (21:00 to 05:00 unless set in Settings > General > Mode,
 * to the minute, across midnight or not) and Desk otherwise, checked every
 * minute in local time. A mode picked by hand (settings, the set_mode
 * command) holds until the schedule's next boundary. Joined to a network other than the saved home one, the face
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

/* Starts the schedule. */
void muse_gadget_mode_start(void);

/* All of these are safe from any task. */
muse_gadget_mode_t muse_gadget_mode(void);
/* By hand: holds until Night's next start or end. */
void muse_gadget_mode_pick(muse_gadget_mode_t mode);
/* The Night window, in minutes after local midnight (muse_settings_night). */
void muse_gadget_mode_night(int *from_min, int *to_min);
/* Saves a new window (writes flash: a task whose stack is internal RAM, such
 * as the LVGL one). Within two seconds a picked mode's hold moves to the new
 * next boundary and the schedule takes over if it's due. */
void muse_gadget_mode_set_night(int from_min, int to_min);
/*
 * What a message to chat `sid` ("" the main chat) carries after its words:
 * the current mode's contract ("[gadget mode: DESK] ..."), or NULL if that
 * chat heard this mode last. *mode (may be NULL) is set to the current mode;
 * once the Muse takes the message, pass it to muse_gadget_mode_told().
 */
const char *muse_gadget_mode_context(const char *sid, int *mode);
/* Chat `sid` was titled after an audio file: once its turn is done, ask the
 * Muse, in a typed turn whose reply isn't shown, to retitle it. */
void muse_gadget_mode_retitle(const char *sid);
/* The Muse took a message that told chat `sid` this mode (saved, as
 * muse_settings_chat_set_told). */
void muse_gadget_mode_told(const char *sid, int mode);
/* "desk", "night" or "on_the_go"; false for anything else. */
bool muse_gadget_mode_parse(const char *name, muse_gadget_mode_t *out);
/* "Desk", "Night", "On-the-go". */
const char *muse_gadget_mode_name(muse_gadget_mode_t mode);
/* "desk", "night", "on_the_go" (as muse_gadget_mode_parse takes them); NULL for anything else. */
const char *muse_gadget_mode_key(muse_gadget_mode_t mode);
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
/* Locked (muse_lock.h): the offer goes; one made since shows once it's unlocked. */
void muse_gadget_mode_hide_toast(void);
/* Opens a dialog over the whole screen (muse_dialog.h, on the top layer) to
 * pick a mode, the current one ticked; its "?" says what each one does. */
void muse_mode_dialog_open(void);

#ifdef __cplusplus
}
#endif
