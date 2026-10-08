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
 * The face's readouts (muse_home_extras.h). The battery goes in the bottom
 * right corner behind a battery icon, which says what the number is, right
 * of the captions and clear of the page dots; muse_ui.c hides it while a
 * reply's page would run over it. The clock goes on top, above the state
 * (CLOCK_Y), and grows big on the Night face.
 */
#include "muse_home_extras.h"

#include <stdio.h>
#include <string.h>

#include "muse_board.h"
#include "muse_extras.h"
#include "muse_state.h"
#include "muse_up_next.h"

#define COLOR_TEXT 0xb9b2d8     /* between muse_ui.c's dim and caption colours */
#define COLOR_DIM 0x8b84a8
#define COLOR_LOW 0xff7a7a      /* battery under LOW_PCT */
#define LOW_PCT 15
/* In from the right edge: the screen's corners are rounded, and at 16 px in
 * the battery's last digit was cut off by the curve. */
#define EDGE 40
/* The corner: a line of montserrat_16, right-aligned, at the very bottom
 * (x 360-440, y 452-470 on 480 px): under the captions (which end at 419)
 * and a heard reply's page (451), so it stays up through one. The row above
 * it is "up next"'s. */
#define CORNER_W 80
#define CORNER_H 18
#define CORNER_BOTTOM 10
/* The row above it (y 432-450): "up next" (muse_up_next.h), one dim line of
 * montserrat_14 ending at the battery's right edge and in dots past UP_W,
 * clear of the captions and the page dots. A reply's page hides it as it
 * does the battery; the Night face leaves it out. */
#define UP_W 300
#define UP_GAP 2
/* Top centre, over the state (muse_ui.c's STATE_Y) and Muse's head; the
 * corner holds the connectivity icons. Offsets are for a 466 px tall
 * screen, as muse_ui.c's are. */
#define CLOCK_Y 12
#define FONT_CLOCK (&lv_font_montserrat_28)
/* The Night face's: big, over Muse in bed, which muse_ui.c moves lower;
 * under the state, and the unpaired gadget's name (NAME_Y). */
#define CLOCK_NIGHT_Y 100
#if LV_FONT_MONTSERRAT_48
#define FONT_NIGHT (&lv_font_montserrat_48)
#else
#define FONT_NIGHT (&lv_font_montserrat_28)
#endif

static lv_obj_t *s_clock;
static lv_obj_t *s_corner;
static lv_obj_t *s_batt;
static lv_obj_t *s_up;          /* "up next": its box, and the line in it */
static lv_obj_t *s_up_lbl;
static float s_next;
static bool s_24h;
static bool s_night;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, lv_align_t align,
                       int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    lv_obj_align(l, align, x, y);
    return l;
}

void muse_home_extras_build(lv_obj_t *face)
{
    s_clock = label(face, FONT_CLOCK, COLOR_TEXT, LV_ALIGN_TOP_MID, 0, CLOCK_Y + (muse_board->height - 466) / 2);

    /* A fixed box, so muse_ui.c can tell what a reply's page would cover. */
    s_corner = lv_obj_create(face);
    lv_obj_remove_style_all(s_corner);
    lv_obj_set_size(s_corner, CORNER_W, CORNER_H);
    lv_obj_align(s_corner, LV_ALIGN_BOTTOM_RIGHT, -EDGE, -CORNER_BOTTOM);
    lv_obj_remove_flag(s_corner, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_batt = label(s_corner, &lv_font_montserrat_16, COLOR_DIM, LV_ALIGN_TOP_RIGHT, 0, 0);
#if CONFIG_MUSE_GADGET_UP_NEXT
    s_up = lv_obj_create(face);
    lv_obj_remove_style_all(s_up);
    lv_obj_set_size(s_up, UP_W, CORNER_H);
    lv_obj_align(s_up, LV_ALIGN_BOTTOM_RIGHT, -EDGE, -(CORNER_BOTTOM + CORNER_H + UP_GAP));
    lv_obj_remove_flag(s_up, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_up_lbl = label(s_up, &lv_font_montserrat_14, COLOR_DIM, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_width(s_up_lbl, UP_W);
    lv_label_set_long_mode(s_up_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_up_lbl, LV_TEXT_ALIGN_RIGHT, 0);
#endif
    s_next = 0;
    s_24h = muse_extras_get_i32("clock_24h", 0) != 0;
}

bool muse_home_extras_24h(void)
{
    return s_24h;
}

void muse_home_extras_set_24h(bool on)
{
    s_24h = on;
    muse_extras_set_i32("clock_24h", on);
    s_next = 0;   /* redraw on the next frame */
}

lv_obj_t *muse_home_extras_clock(void)
{
    return s_clock;
}

lv_obj_t *muse_home_extras_corner(void)
{
    return s_corner;
}

lv_obj_t *muse_home_extras_up_next(void)
{
    return s_up;
}

void muse_home_extras_set_night(bool night)
{
    s_night = night;
    s_next = 0;   /* "up next" goes or comes back on the next frame */
    lv_obj_set_style_text_font(s_clock, night ? FONT_NIGHT : FONT_CLOCK, 0);
    lv_obj_align(s_clock, LV_ALIGN_TOP_MID, 0, (night ? CLOCK_NIGHT_Y : CLOCK_Y) + (muse_board->height - 466) / 2);
}

static void set_text(lv_obj_t *l, const char *text)
{
    if (l && strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

void muse_home_extras_tick(float now)
{
    if (!s_clock || now < s_next) {
        return;
    }
    s_next = now + 1.0f;

    char buf[24];
    if (!muse_time_format(buf, sizeof(buf), s_24h ? "%H:%M" : "%I:%M %p")) {
        strlcpy(buf, "--:--", sizeof(buf));
    } else if (!s_24h && buf[0] == '0') {
        memmove(buf, buf + 1, strlen(buf));   /* 9:30 PM, not 09:30 PM */
    }
    set_text(s_clock, buf);

    /* The icon says it's the battery: a bolt while charging, else how full.
     * No reading (no battery), nothing: the screen's on, so there's power. */
    muse_power_t p = muse_state_power();
    buf[0] = '\0';
    if (p.battery_pct >= 0) {
        const char *icon = p.charging ? LV_SYMBOL_CHARGE
                         : p.battery_pct >= 88 ? LV_SYMBOL_BATTERY_FULL
                         : p.battery_pct >= 63 ? LV_SYMBOL_BATTERY_3
                         : p.battery_pct >= 38 ? LV_SYMBOL_BATTERY_2
                         : p.battery_pct >= LOW_PCT ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        snprintf(buf, sizeof(buf), "%s %d%%", icon, p.battery_pct);
    }
    set_text(s_batt, buf);
#if CONFIG_MUSE_GADGET_UP_NEXT
    /* Emptied rather than hidden: a reply's layout unhides it on the way out. */
    char next[72], line[80] = "";
    if (!s_night && muse_up_next_line(next, sizeof(next))) {
        snprintf(line, sizeof(line), LV_SYMBOL_BELL " %s", next);
    }
    set_text(s_up_lbl, line);
#endif
    static bool low;
    if (low != (p.battery_pct >= 0 && p.battery_pct < LOW_PCT && !p.charging)) {
        low = !low;
        lv_obj_set_style_text_color(s_batt, lv_color_hex(low ? COLOR_LOW : COLOR_DIM), 0);
    }
}
