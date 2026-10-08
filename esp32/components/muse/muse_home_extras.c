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
 * level on his belly once plugged in, patted or low (muse_ui.c).
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
/* "Up next" (muse_up_next.h): a pill centred over the page dots (its bottom
 * at y 446 on 480 px), just the line in unscii, Muse's own pixel type.
 * A line wider than UP_TEXT_W wraps, centred, to as few lines as it takes
 * (up_fit), the pill growing upwards: two take it to y 394, three to 376,
 * still under Muse's feet (367). Past UP_LINES it ends in dots. A reply's
 * page hides it, and the Night face leaves it out. */
#define UP_TEXT_W 360           /* 22 columns of unscii_16 */
#define UP_LINES 3
#define UP_LINE_SPACE 2
#define UP_PAD_V 9
#define CAPTION_S 6.0f          /* a new caption keeps the pill out of its way this long */
#define UP_BOTTOM 34
#define FADE_MS 250             /* the clock's minute, and a new line in the pill: out, then in */
#define COLOR_UP_BG 0x1d1733
#define COLOR_UP_EDGE 0x5b3fa0
#define COLOR_UP_TEXT 0xe4defa
/* Top centre, over the chat's name (muse_ui.c's STATE_Y) and Muse's head;
 * the corner holds the connectivity icons. Big, to read at a glance, and the
 * same on every face, the Night one too. Offsets are for a 466 px tall
 * screen, as muse_ui.c's are. */
#define CLOCK_Y 6
LV_FONT_DECLARE(muse_font_clock_72);
#define FONT_CLOCK (&muse_font_clock_72)
#define FONT_UP (&lv_font_unscii_16)

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
    lv_obj_set_size(s_up, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(s_up, UP_PAD_V, 0);
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
    s_up_lbl = lv_label_create(s_up);
    lv_obj_set_style_text_font(s_up_lbl, FONT_UP, 0);
    lv_obj_set_style_text_color(s_up_lbl, lv_color_hex(COLOR_UP_TEXT), 0);
    lv_obj_set_style_text_align(s_up_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_up_lbl, UP_LINE_SPACE, 0);
    lv_label_set_text(s_up_lbl, "");
    lv_label_set_long_mode(s_up_lbl, LV_LABEL_LONG_MODE_DOTS);   /* up_fit() sets its size */
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

static void fade_opa(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void fade(lv_obj_t *o, lv_opa_t to, uint32_t ms, lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, fade_opa);
    lv_anim_set_values(&a, lv_obj_get_style_opa(o, 0), to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);   /* in place of one still going */
}

/* A new minute: the old one fades out, then the new one in. */
static char s_clock_text[24];

static void clock_faded_out(lv_anim_t *a)
{
    (void)a;
    lv_label_set_text(s_clock, s_clock_text);
    fade(s_clock, LV_OPA_COVER, FADE_MS / 2, NULL);
}

static void clock_set(const char *text)
{
    if (!strcmp(text, s_clock_text)) {
        return;
    }
    bool was = s_clock_text[0];
    strlcpy(s_clock_text, text, sizeof(s_clock_text));
    if (!was) {
        lv_label_set_text(s_clock, text);   /* the first: no fade */
    } else {
        fade(s_clock, LV_OPA_TRANSP, FADE_MS / 2, clock_faded_out);
    }
}

#if CONFIG_MUSE_GADGET_UP_NEXT
static bool s_up_shown;
static char s_up_text[72];      /* the line the label has, or is fading to */

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

/* Lines `line` takes wrapped at `w` px, and the widest of them in *wide. */
static int up_lines(const char *line, int w, int *wide)
{
    lv_point_t size;
    lv_text_get_size(&size, line, FONT_UP, 0, UP_LINE_SPACE, w, LV_TEXT_FLAG_NONE);
    *wide = size.x;
    int pitch = lv_font_get_line_height(FONT_UP) + UP_LINE_SPACE;
    return (size.y + UP_LINE_SPACE) / pitch;
}

/*
 * The label sized to its line, which the pill sizes itself round: one line
 * if it fits in UP_TEXT_W, or else the fewest lines that hold it, as evenly
 * long as they'll go (the narrowest width that wraps it to that many), so
 * it reads as a block rather than a line and a straggler. Past UP_LINES it
 * ends in dots, which need the fixed size too.
 */
static void up_fit(const char *line)
{
    int cw = lv_font_get_glyph_width(FONT_UP, 'M', ' ');   /* unscii: every glyph's */
    int wide, n = up_lines(line, UP_TEXT_W, &wide);
    if (n > 1 && n <= UP_LINES) {
        int word = 0;   /* no narrower than its longest word, which would break */
        for (int i = 0, run = 0; line[i]; i++) {
            run = line[i] == ' ' ? 0 : run + cw;
            word = run > word ? run : word;
        }
        for (int w = word > UP_TEXT_W / n ? word : UP_TEXT_W / n; w < UP_TEXT_W; w += cw) {
            int at_w;
            if (up_lines(line, w, &at_w) == n) {
                wide = at_w;
                break;
            }
        }
    }
    n = n < UP_LINES ? n : UP_LINES;
    int h = n * lv_font_get_line_height(FONT_UP) + (n - 1) * UP_LINE_SPACE;
    lv_obj_set_size(s_up_lbl, wide < UP_TEXT_W ? wide : UP_TEXT_W, h);
}

static void up_take_text(void)
{
    if (strcmp(lv_label_get_text(s_up_lbl), s_up_text) != 0) {
        lv_label_set_text(s_up_lbl, s_up_text);
        up_fit(s_up_text);
    }
}

/* Fades the pill in when there's a line, out when there isn't, with the
 * line a fade it cuts short (up_set) was bringing. */
static void up_show(bool show)
{
    up_take_text();
    fade(s_up, show ? LV_OPA_COVER : LV_OPA_TRANSP, 400, NULL);
}

static void up_faded_out(lv_anim_t *a)
{
    (void)a;
    up_take_text();
    if (s_up_shown) {
        fade(s_up, LV_OPA_COVER, FADE_MS, NULL);
    }
}

/* A new line: showing, the pill fades out with the old and back with it. */
static void up_set(const char *line)
{
    if (!strcmp(line, s_up_text)) {
        return;
    }
    strlcpy(s_up_text, line, sizeof(s_up_text));
    if (s_up_shown && lv_obj_get_style_opa(s_up, 0) > LV_OPA_TRANSP) {
        fade(s_up, LV_OPA_TRANSP, FADE_MS, up_faded_out);
    } else {
        up_take_text();
    }
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
    clock_set(buf);
#if CONFIG_MUSE_GADGET_UP_NEXT
    /* Faded rather than hidden: a reply's layout unhides it on the way out.
     * Only on a quiet face: idle, with no caption (what was heard, the
     * reply) for it to sit on. */
    char line[72] = "";
    if (s_night || !muse_up_next_line(line, sizeof(line))) {
        line[0] = '\0';
    }
    if (line[0]) {
        up_set(line);
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
