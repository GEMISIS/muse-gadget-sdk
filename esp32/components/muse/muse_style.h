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

/*
 * One look for the screens and the cards over them: the palette, the type
 * for each role, the sizes of rows, buttons and cards, and the few widgets
 * more than one screen builds (a row, a button, the "?" button, a card on a
 * dimmed backdrop, a scrolling column). The settings pages, the Chats screen,
 * the dialogs, the power menu and the face's overlays all take them from
 * here, so they can't drift apart. All of it runs in the LVGL task.
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- palette ---------- */

#define MUSE_COLOR_TEXT 0xf2efff
#define MUSE_COLOR_DIM 0x8b84a8              /* notes, values, scrollbars */
#define MUSE_COLOR_ACCENT 0xa77dff           /* titles, icons, the current choice */
#define MUSE_COLOR_ACCENT_PRESSED 0xc8adff
#define MUSE_COLOR_CARD 0x1a1530             /* a row on black, a card's own colour */
#define MUSE_COLOR_CARD_PRESSED 0x2e2552     /* that pressed; a row on a card */
#define MUSE_COLOR_RAISED_PRESSED 0x3d3270   /* a row on a card, pressed */
#define MUSE_COLOR_OK 0x6ff0bf               /* connected, working */
#define MUSE_COLOR_WARN 0xffb45c             /* needs a look; On-the-go */
#define MUSE_COLOR_DANGER 0xff5c5c           /* can't be undone */
#define MUSE_COLOR_DANGER_PRESSED 0xff8a8a
#define MUSE_COLOR_NIGHT 0x7d8cff            /* the Night mode */
#define MUSE_COLOR_TRACK 0x2a2345            /* a slider's or bar's empty part */
#define MUSE_COLOR_OFF 0x3a3358              /* a switch off, a page dot not on */
/* The face's readouts (clock, battery) are dimmer than text on a page, so
 * they don't pull the eye from Muse. */
#define MUSE_COLOR_FACE_TEXT 0xb9b2d8

/* ---------- type, by role ---------- */

#define MUSE_FONT_TITLE (&lv_font_unscii_16)         /* a page's, in capitals, letter-spaced */
#define MUSE_TITLE_LETTER_SPACE 2
#define MUSE_FONT_CARD_TITLE (&lv_font_montserrat_20) /* a dialog's or menu card's, sentence case */
#define MUSE_FONT_BUTTON (&lv_font_montserrat_20)     /* a button or option on a card */
#define MUSE_FONT_NOTE (&lv_font_montserrat_16)       /* notes under rows, a card's text */

/* ---------- sizes ---------- */

#define MUSE_TITLE_Y 44         /* a page's title, top centre */
#define MUSE_LIST_W 330         /* a page's rows: inside a round panel, and fits a 368 px one */
#define MUSE_LIST_GUTTER 12     /* either side of the rows; the scrollbar runs down the right one */
#define MUSE_ROW_RADIUS 18      /* rows and buttons */
#define MUSE_ROW_PAD 16         /* in from a row's ends */
#define MUSE_ROW_GAP_X 12       /* between a row's icon, text and value */
#define MUSE_CARD_RADIUS 24     /* cards over the screen: dialogs, menus, the volume, a suggestion */
#define MUSE_CARD_BORDER 2
#define MUSE_CARD_PAD 16
#define MUSE_CARD_GAP 8
#define MUSE_BACKDROP_OPA LV_OPA_80   /* what's under a card */

#if CONFIG_MUSE_BOARD_WAVESHARE_S3_216
/* The 2.16's 480 px screen has room for taller rows, easier to hit, in bigger
 * type; General's four rows and a Back still fit without scrolling. */
#define MUSE_LIST_TOP 72
#define MUSE_ROW_H 70
#define MUSE_ROW_GAP 6
#define MUSE_LIST_PAD_BOTTOM 24   /* just clear of the page dots */
#define MUSE_INFO_H 52
#define MUSE_BUTTON_H 56          /* a button or option on a card */
#define MUSE_HELP_D 48            /* the "?" circle */
#define MUSE_FONT_ROW (&lv_font_montserrat_24)     /* a row's text and icon */
#define MUSE_FONT_VALUE (&lv_font_montserrat_20)   /* the value or tick on its right */
#else
#define MUSE_LIST_TOP 84
#define MUSE_ROW_H 58
#define MUSE_ROW_GAP 10
#define MUSE_LIST_PAD_BOTTOM 40
#define MUSE_INFO_H 44
#define MUSE_BUTTON_H 52
#define MUSE_HELP_D 36
#define MUSE_FONT_ROW (&lv_font_montserrat_20)
#define MUSE_FONT_VALUE (&lv_font_montserrat_16)
#endif
#define MUSE_TICK_W 24   /* a tick's room on a row's right, kept whether or not it shows */

/* ---------- widgets ---------- */

typedef enum {
    MUSE_BUTTON_NEUTRAL,   /* Back, Cancel and the like: outlined */
    MUSE_BUTTON_ACCENT,    /* the usual choice: filled */
    MUSE_BUTTON_DANGER,    /* can't be undone: filled red */
} muse_button_kind_t;

/* A label in `font` and `color` (an 0xRRGGBB). */
lv_obj_t *muse_style_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text);

/* A page's title, top centre (MUSE_TITLE_Y). */
lv_obj_t *muse_style_title(lv_obj_t *parent, const char *text);

/*
 * A row, full width and MUSE_ROW_H tall, laid out left to right and centred
 * up and down: a button if `clickable`. On a card (`on_card`) it's a shade
 * lighter, so it shows.
 */
lv_obj_t *muse_style_row(lv_obj_t *parent, bool clickable, bool on_card);

/* A full-width button `h` tall with `text` in `font`, centred, ending in
 * "..." if it's too long; the label is its first child. */
lv_obj_t *muse_style_button(lv_obj_t *parent, const char *text, muse_button_kind_t kind, const lv_font_t *font,
                            int h);

/* The round "?" (MUSE_HELP_D) that opens a screen's or a card's help: the
 * same wherever it is. The caller places it and handles its click. */
lv_obj_t *muse_style_help_button(lv_obj_t *parent);

/*
 * The whole of `parent` (or the top layer, if NULL) dimmed, taking every tap
 * and holding drags from the screens either side, and a card on it, centred,
 * `w` wide (at most the screen less a margin), as tall as what's on it: a
 * column, its items centred.
 */
lv_obj_t *muse_style_backdrop(lv_obj_t *parent, int w, lv_obj_t **card_out);

/* A card's look on its own, for one that isn't on a backdrop. */
void muse_style_card(lv_obj_t *card);

/*
 * Scrolls up and down only when what's in it doesn't fit, with no bounce,
 * and a dim scrollbar while it doesn't: `inset` px in from the right edge,
 * its ends `top` and `bottom` px in (clear of a curve or a rounded corner).
 */
void muse_style_scroll_column(lv_obj_t *o, int width, int inset, int top, int bottom);

#ifdef __cplusplus
}
#endif
