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
 * A reply's captions as lyrics, in the full layout's reply box (muse_ui.c's
 * answer layouts): no pages, the whole reply (muse_state_reply) a column
 * that glides up a line as the speech reaches it.
 *
 *   the line being said    full brightness, its words lighting as they're
 *                          said, each with a little pop; those to come dim
 *   the line just said     drifted up, at 40%
 *   the next line          waiting under it, at 60%
 *
 * and the box's top and bottom edges fade, so lines slide in and out rather
 * than being cut. It follows the speech (muse_lyrics.h), so it holds at a
 * comma or a sentence's end as Muse does, and never runs ahead.
 *
 * Dragged down, it scrolls back through the reply, easing back to the live
 * line a few seconds after it's let go. A tap opens the whole reply in a
 * sheet like the widgets' (muse_widget_ui.h): x is done with it, a swipe
 * down puts it away. A reply that isn't spoken (the speaker off, or no
 * speech) goes at reading pace instead: a tap pauses and goes on, a drag
 * moves it, and it stays up till it's read, even after the turn; a long
 * press opens the sheet.
 *
 * Without PSRAM it isn't built, and the paged captions stay. All of it runs
 * in the LVGL task.
 */

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* On the face (`sw` x `sh`), in `font` (the captions', CJK fallback and
 * all), lines `line_space` apart. False if it can't be (no PSRAM). */
bool muse_lyrics_ui_build(lv_obj_t *face, const lv_font_t *font, int line_space, int sw, int sh);

/* The reply box: `w` wide, its top `top` px from the screen's centre,
 * `cols` x `rows` (2 or more), its lines aligned so. */
void muse_lyrics_ui_place(int w, int top, int cols, int rows, lv_text_align_t align);

/*
 * Each frame on the face: takes the reply and moves the captions along.
 * `room`: an answer layout is up for them. True while they have the
 * caption, so the face's own caption labels stay out of the way.
 */
bool muse_lyrics_ui_tick(float now, bool room);

/* A reply still up after its turn: being read (it isn't spoken), paused,
 * or scrolled back. The face keeps the answer layout for it; another
 * caption (an error, "SPEAKER ON") takes its place. */
bool muse_lyrics_ui_holding(void);

/* Locked, or a new turn: the captions and the sheet go at once, and what
 * they held is forgotten. */
void muse_lyrics_ui_drop(void);

/* The whole reply's sheet is up. */
bool muse_lyrics_ui_sheet_up(void);

#ifdef __cplusplus
}
#endif
