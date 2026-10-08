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
 * The face's corner readouts (muse_home_extras.h). On the 480x480 screen the
 * bezel ring is a 236 px circle about the centre; everything here stays
 * outside it: battery and steps in the bottom right corner, the top corners
 * being left to the button icons, as the keys are on the top edge. The clock
 * goes above Muse instead (CLOCK_Y).
 */
#include "muse_home_extras.h"

#include <stdio.h>
#include <string.h>

#include "muse_board.h"
#include "muse_extras.h"
#include "muse_imu.h"
#include "muse_state.h"

#define COLOR_TEXT 0xb9b2d8     /* between muse_ui.c's dim and caption colours */
#define COLOR_DIM 0x8b84a8
#define COLOR_BAR_BG 0x1d1733
#define COLOR_BAR 0xa77dff
#define COLOR_LOW 0xff7a7a      /* battery under LOW_PCT */
#define LOW_PCT 15
#define EDGE 16
#define BAR_W 56
/* The clock sits in the row muse_ui.c keeps for the unpaired gadget's name,
 * under the status line and the state, above Muse's head; once paired that
 * row is empty. Offsets are for a 466 px tall screen, as muse_ui.c's are. */
#define CLOCK_Y 60

static lv_obj_t *s_clock;
static lv_obj_t *s_batt;
static lv_obj_t *s_steps;
static lv_obj_t *s_bar;
static float s_next;
static bool s_24h;

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
    s_clock = label(face, &lv_font_montserrat_20, COLOR_TEXT, LV_ALIGN_TOP_MID, 0,
                    CLOCK_Y + (muse_board->height - 466) / 2);
    s_batt = label(face, &lv_font_unscii_16, COLOR_DIM, LV_ALIGN_BOTTOM_RIGHT, -EDGE, -54);
#if CONFIG_MUSE_GADGET_IMU
    s_steps = label(face, &lv_font_unscii_16, COLOR_DIM, LV_ALIGN_BOTTOM_RIGHT, -EDGE, -34);
    s_bar = lv_bar_create(face);
    lv_obj_set_size(s_bar, BAR_W, 4);
    lv_obj_align(s_bar, LV_ALIGN_BOTTOM_RIGHT, -EDGE, -14);
    lv_bar_set_range(s_bar, 0, CONFIG_MUSE_GADGET_STEP_GOAL);
    lv_obj_set_style_radius(s_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(COLOR_BAR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(COLOR_BAR), LV_PART_INDICATOR);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
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

    muse_power_t p = muse_state_power();
    buf[0] = '\0';
    if (p.battery_pct >= 0) {
        snprintf(buf, sizeof(buf), "%s%d%%", p.charging ? "+" : "", p.battery_pct);
    }
    set_text(s_batt, buf);
    static bool low;
    if (low != (p.battery_pct >= 0 && p.battery_pct < LOW_PCT && !p.charging)) {
        low = !low;
        lv_obj_set_style_text_color(s_batt, lv_color_hex(low ? COLOR_LOW : COLOR_DIM), 0);
    }

#if CONFIG_MUSE_GADGET_IMU
    int steps = muse_imu_steps();
    if (steps < 0) {
        return;   /* no IMU: nothing to show */
    }
    snprintf(buf, sizeof(buf), "%d", steps);
    set_text(s_steps, buf);
    lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    int goal = CONFIG_MUSE_GADGET_STEP_GOAL;
    if (lv_bar_get_value(s_bar) != (steps < goal ? steps : goal)) {
        lv_bar_set_value(s_bar, steps < goal ? steps : goal, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(steps >= goal ? 0x7dffa7 : COLOR_BAR),
                                  LV_PART_INDICATOR);
    }
#endif
}
