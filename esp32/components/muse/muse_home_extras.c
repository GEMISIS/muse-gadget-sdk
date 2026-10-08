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
 * The face's readouts (muse_home_extras.h). The clock goes on top, above the
 * state (CLOCK_Y), and grows big on the Night face. The battery is Muse's
 * own to show (muse_pixel.h): a badge beside him when it runs low, and the
 * level on his belly while he charges.
 */
#include "muse_home_extras.h"

#include <stdio.h>
#include <string.h>

#include "muse_board.h"
#include "muse_extras.h"
#include "muse_state.h"
#include "muse_dialog.h"
#include "muse_style.h"
#include "muse_up_next.h"

#define COLOR_TEXT MUSE_COLOR_FACE_TEXT   /* the clock: between dim and the captions' colour */
/* "Up next" (muse_up_next.h): a pill centred over the page dots (y 412-446
 * on 480 px), a bell and the line in unscii, Muse's own pixel type, ending
 * in dots past UP_TEXT_W. A reply's page hides it, and the Night face
 * leaves it out. */
#define UP_TEXT_W 290
#define CAPTION_S 6.0f          /* a new caption keeps the pill out of its way this long */
#define UP_H 34
#define UP_BOTTOM 34
#define COLOR_UP_BG 0x1d1733
#define COLOR_UP_EDGE 0x5b3fa0
#define COLOR_UP_ICON MUSE_COLOR_ACCENT
#define COLOR_UP_TEXT 0xe4defa
/* Top centre, over the chat's name (muse_ui.c's STATE_Y) and Muse's head; the
 * corner holds the connectivity icons. Offsets are for a 466 px tall
 * screen, as muse_ui.c's are. */
#define CLOCK_Y 10
#if LV_FONT_MONTSERRAT_48
#define FONT_CLOCK (&lv_font_montserrat_48)   /* big, to read at a glance */
#else
#define FONT_CLOCK (&lv_font_montserrat_28)
#endif
/* The Night face's: big, over Muse in bed, which muse_ui.c moves lower;
 * under the state, and the unpaired gadget's name (NAME_Y). */
#define CLOCK_NIGHT_Y 100
#if LV_FONT_MONTSERRAT_48
#define FONT_NIGHT (&lv_font_montserrat_48)
#else
#define FONT_NIGHT (&lv_font_montserrat_28)
#endif

static lv_obj_t *s_clock;
#if CONFIG_MUSE_GADGET_UP_NEXT
static void on_up_clicked(lv_event_t *e);
#endif
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
#if CONFIG_MUSE_GADGET_UP_NEXT
    s_up = lv_obj_create(face);
    lv_obj_remove_style_all(s_up);
    lv_obj_set_size(s_up, LV_SIZE_CONTENT, UP_H);
    lv_obj_align(s_up, LV_ALIGN_BOTTOM_MID, 0, -UP_BOTTOM);
    lv_obj_remove_flag(s_up, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_up, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_up, 14);
    lv_obj_add_event_cb(s_up, on_up_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_radius(s_up, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_up, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_up, lv_color_hex(COLOR_UP_BG), 0);
    lv_obj_set_style_border_width(s_up, 1, 0);
    lv_obj_set_style_border_color(s_up, lv_color_hex(COLOR_UP_EDGE), 0);
    lv_obj_set_style_pad_hor(s_up, 16, 0);
    lv_obj_set_style_pad_column(s_up, 8, 0);
    lv_obj_set_flex_flow(s_up, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_up, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_opa(s_up, LV_OPA_TRANSP, 0);   /* shown once there's a line (up_show) */
    lv_obj_t *bell = lv_label_create(s_up);
    lv_obj_set_style_text_font(bell, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(bell, lv_color_hex(COLOR_UP_ICON), 0);
    lv_label_set_text(bell, LV_SYMBOL_BELL);
    s_up_lbl = lv_label_create(s_up);
    lv_obj_set_style_text_font(s_up_lbl, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(s_up_lbl, lv_color_hex(COLOR_UP_TEXT), 0);
    lv_label_set_text(s_up_lbl, "");
    lv_label_set_long_mode(s_up_lbl, LV_LABEL_LONG_MODE_DOTS);   /* one line: up_fit() sets its width */
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

#if CONFIG_MUSE_GADGET_UP_NEXT
static void up_fade(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static bool s_up_shown;

/* Tapped: the whole of what Muse said, in a dialog. */
static void on_up_clicked(lv_event_t *e)
{
    (void)e;
    static char full[256];
    if (!s_up_shown || !muse_up_next_full(full, sizeof(full))) {
        return;
    }
    static const muse_dialog_button_t ok = { "Got it", MUSE_DIALOG_ACCENT, NULL, NULL };
    const muse_dialog_t d = {
        .title = "Up next",
        .text = full,
        .buttons = &ok,
        .button_count = 1,
    };
    muse_dialog_open(NULL, &d);
}

/* The label as wide as its line, up to UP_TEXT_W, past which it ends in
 * dots: dots need a fixed width, and the pill sizes itself round it. */
static void up_fit(const char *line)
{
    lv_point_t size;
    lv_text_get_size(&size, line, &lv_font_unscii_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_obj_set_size(s_up_lbl, size.x < UP_TEXT_W ? size.x : UP_TEXT_W,
                    lv_font_get_line_height(&lv_font_unscii_16));   /* one line: the dots need a height too */
}

/* Fades the pill in when there's a line, out when there isn't. */
static void up_show(bool show)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_up);
    lv_anim_set_exec_cb(&a, up_fade);
    lv_anim_set_values(&a, lv_obj_get_style_opa(s_up, 0), show ? LV_OPA_COVER : LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 400);
    lv_anim_start(&a);
}
#endif

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
#if CONFIG_MUSE_GADGET_UP_NEXT
    /* Faded rather than hidden: a reply's layout unhides it on the way out.
     * Only on a quiet face: idle, with no caption (what was heard, the
     * reply) for it to sit on. */
    char line[72] = "";
    if (s_night || !muse_up_next_line(line, sizeof(line))) {
        line[0] = '\0';
    }
    if (line[0] && strcmp(lv_label_get_text(s_up_lbl), line) != 0) {
        set_text(s_up_lbl, line);
        up_fit(line);
    }
    /* A caption stays in the state after it's read ("DESK MODE", say), so
     * one only counts while it's new: the pill steps aside for CAPTION_S. */
    static uint32_t caption_ver;
    static float caption_at = -100.0f;
    char caption[4] = "";
    if (muse_state_caption(caption, sizeof(caption), &caption_ver) && caption[0]) {
        caption_at = now;
    }
    bool show = line[0] && muse_state_mode(NULL) == MUSE_MODE_IDLE && now - caption_at > CAPTION_S;
    if (show != s_up_shown) {
        s_up_shown = show;
        if (show) {
            /* Over the face's later boxes (the caption's, the reply's), which
             * would otherwise take the taps on its lower half. */
            lv_obj_move_foreground(s_up);
        }
        up_show(show);
    }
#endif
}
