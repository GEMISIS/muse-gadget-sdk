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
 * A reply's widgets on the face (muse_widget.h), on the 2.16: one sheet for
 * every kind, a card that slides up from the bottom once the reply's being
 * said, Muse small over it (muse_ui.c's ANSWER_WIDGET layout) with the
 * captions under him. A header (a grabber, the title and its line, a close
 * button) stays put over a column that scrolls, edges fading while there's
 * more. Every kind is rows in the settings' look: options are pills when
 * one to three short ones fit across, else rows like the rest; a list's
 * rows, places and products carry their own icon, price or button.
 *
 * A tap on something that answers (an option, a "send" row, a place, a
 * flight's or a product's button) sends its words to the widget's chat
 * (muse_hatch_reply_text): the sheet folds into a "sent" chip with them and
 * goes. The close button, or a swipe down, puts the sheet away, leaving a
 * chip over the page dots that brings it back, until the next turn. A talk
 * press answers by voice as ever, and the sheet goes with the turn.
 *
 * All of it runs in the LVGL task.
 */

#include <stdbool.h>

#include "lvgl.h"
#include "muse_state.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_MUSE_HATCH && CONFIG_MUSE_BOARD_WAVESHARE_S3_216
#define MUSE_WIDGET_UI 1

/* On the face tile, `w` x `h`; the sheet's top no higher than `top` (px from the screen's top). */
void muse_widget_ui_build(lv_obj_t *face, int w, int h, int top);
/* Every frame on the face: takes a new set, opens the sheet once the reply's
 * being said (`may_open`: nothing else, a held-up photo, has the room). */
void muse_widget_ui_tick(muse_mode_t mode, float now, bool may_open);
/* The sheet's up (or folding away): the face makes room for it. */
bool muse_widget_ui_sheet_up(void);
/* The chip that brings it back is showing, where "up next" goes. */
bool muse_widget_ui_chip_up(void);
#else
#define MUSE_WIDGET_UI 0
static inline void muse_widget_ui_build(lv_obj_t *face, int w, int h, int top)
{
    (void)face;
    (void)w;
    (void)h;
    (void)top;
}
static inline void muse_widget_ui_tick(muse_mode_t mode, float now, bool may_open)
{
    (void)mode;
    (void)now;
    (void)may_open;
}
static inline bool muse_widget_ui_sheet_up(void) { return false; }
static inline bool muse_widget_ui_chip_up(void) { return false; }
#endif

#ifdef __cplusplus
}
#endif
