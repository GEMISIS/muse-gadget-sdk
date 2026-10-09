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
 * The captions as lyrics (muse_lyrics_ui.h). The box is the reply's, a half
 * line taller at top and bottom, where the fades are: two gradients in the
 * face's black over the lines, no blur. A pool of labels, a few more than
 * the box's rows, shows the lines near it, each on the one slot it keeps
 * while it's in view; the line being said is three labels instead, side by
 * side (the caption font's characters are all a cell wide): what's been
 * said, the word being said (which pops), and what's to come.
 *
 * Where the column is, `s`, is in 256ths of a line: the line at the box's
 * focus row (its second, or its first in a two-line box). One animation
 * moves it a line as the speech turns one; a drag moves it with the finger.
 * Per frame there's only the speech's place to look up, and labels moved
 * or re-lit when it's changed.
 */
#include "muse_lyrics_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "muse_lyrics.h"
#include "muse_state.h"
#include "muse_style.h"
#include "muse_text.h"

static const char *TAG = "muse_lyrics";

#define POOL_MAX 12             /* line labels: the rows, and those sliding in and out */
#define LINES_MAX 1024          /* a reply's lines, MUSE_REPLY_MAX at its narrowest */
#define LINE_BYTES 160          /* a line's text: 30 columns of up to 4 bytes, and more */
#define ONE 256                 /* a line, in s's units */
#define SCROLL_MS 250           /* up a line as the speech turns one */
#define SNAP_MS 180             /* let go mid-line: to the nearest */
#define BACK_MS 450             /* back to the live line, */
#define BACK_AFTER_MS 3000      /* this long after a drag's let go */
#define POP_MS 240              /* a word lighting */
#define POP_SCALE 282           /* from 110% */
#define FADE_MS 250             /* the captions coming and going */
#define PRESS_IN_MS 70          /* the plate behind them, touched (muse_style.c's) */
#define PRESS_OUT_MS 200
#define TAP_SLOP 8              /* moved further than this, a touch is a drag */
#define PAD_X 22                /* the box's sides: the pop's room, and the pause mark's */
#define MASK_IN 5               /* the fades reach this far into the rows */
#define READ_CPS 16             /* not spoken: reading pace, the old pages' (muse_chat_session.cpp's TEXT_CHARS_PER_S) */
#define READ_HOLD_S 2.5f        /* read to the end: up this long more */
#define PAUSED_MAX_S 60.0f      /* paused: up this long, then it goes */
#define OPA_SAID 102            /* 40%: the line just said */
#define OPA_NEXT 153            /* 60%: the line to come */
#define OPA_AHEAD 140           /* the words to come on the line being said */
#define COLOR_LYRIC 0xd8d2ff    /* muse_ui.c's caption colour */
#define COLOR_POP 0xffffff
#define COLOR_FACE 0x000000     /* what the fades fade to */

/* The sheet, as the widget sheet's (muse_widget_ui.c) */
#define SHEET_SIDE 12
#define SHEET_BOTTOM 26
#define SHEET_TOP 44            /* its highest top */
#define SHEET_PAD_TOP 8
#define GRABBER_W 40
#define GRABBER_H 5
#define CLOSE_D 36
#define SCROLLBAR_ROOM 10
#define FADE_H 24
#define DRAG_AWAY_PX 60
#define OPEN_MS 320
#define AWAY_MS 220

typedef struct {
    lv_obj_t *face, *box, *plate, *mask[2], *pause;
    lv_obj_t *line[POOL_MAX];
    int line_of[POOL_MAX];      /* the line each shows, or -1 */
    int32_t y_of[POOL_MAX];
    int16_t opa_of[POOL_MAX];   /* -1: to be set */
    lv_obj_t *said, *word, *ahead;   /* the line being said */
    int pool;
    const lv_font_t *font;
    int pitch, cw, margin, sw, sh;
    int w, top, cols, rows, focus;
    lv_text_align_t align;

    char *text, *in;            /* MUSE_REPLY_MAX each: the reply, and the one coming in */
    size_t len, weight;
    muse_lyrics_line_t *lines;
    int n;
    uint32_t ver, caption_ver;
    size_t at;                  /* how much has been said */
    bool spoken, live, up, closed;
    float now;

    int cur;                    /* the line being said, */
    size_t lit, word_at;        /* and how much of it's lit (SIZE_MAX: to be set) */
    int32_t s;                  /* the line at the focus row, in 256ths */

    bool pressed, moved, browsing;
    int32_t drag, drag_s0;
    uint32_t let_go_ms;

    float read_w, read_last;    /* not spoken: how far reading's got, in the text's weight */
    bool paused;
    float paused_at, done_at;

    lv_obj_t *sheet, *sheet_col, *sheet_text, *sheet_line, *sheet_fade[2];
    int32_t sheet_drag;
    uint32_t sheet_ver;
} lyr_t;

EXT_RAM_BSS_ATTR static lyr_t *L;

static void layout(void);
static void open_sheet(void);
static void close_sheet(bool done);

/* ---- Pieces ---- */

/* Characters in n bytes of s: a cell each in the caption font. */
static int cells(const char *s, size_t n)
{
    int c = 0;
    for (size_t i = 0; i < n; i++) {
        c += ((unsigned char)s[i] & 0xC0) != 0x80;
    }
    return c;
}

static lv_obj_t *lyric(lv_obj_t *parent)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, L->font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COLOR_LYRIC), 0);
    lv_label_set_text(l, "");
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

static void set_part(lv_obj_t *l, const char *s, size_t n)
{
    char buf[LINE_BYTES];
    n = n < sizeof(buf) - 1 ? n : sizeof(buf) - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    lv_label_set_text(l, buf);
}

static void set_hidden(lv_obj_t *o, bool hidden)
{
    if (hidden != lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_flag(o, LV_OBJ_FLAG_HIDDEN, hidden);
    }
}

static void anim(void *o, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t ms, lv_anim_path_cb_t path,
                 lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, path);
    lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);   /* in place of one still going */
}

static void fade_opa(void *o, int32_t v)
{
    lv_obj_set_style_opa(o, (lv_opa_t)v, 0);
}

static void translate_y(void *o, int32_t v)
{
    lv_obj_set_style_translate_y(o, v, 0);
}

/* ---- Where the lines go ---- */

/* A line's brightness by its distance below the line at the focus row (256ths of a line). */
static lv_opa_t tier(int32_t d)
{
    if (d >= 0) {
        return (lv_opa_t)(255 - (255 - OPA_NEXT) * (d < ONE ? d : ONE) / ONE);
    }
    return (lv_opa_t)(255 - (255 - OPA_SAID) * (-d < ONE ? -d : ONE) / ONE);
}

/* Where the column is shown from: the reply's first lines fill the box from
 * its top, the focus row reached by the speech before the column moves. */
static int32_t view(void)
{
    return L->s > L->focus * ONE ? L->s : L->focus * ONE;
}

/* Line i's row in the box, in 256ths: 0 its top one. */
static int32_t line_row(int i)
{
    return (L->focus + i) * ONE - view();
}

static int32_t line_y(int i)
{
    int32_t r = line_row(i);
    return L->margin + (r * L->pitch + (r >= 0 ? ONE / 2 : -ONE / 2)) / ONE;
}

/* Gone by a row out of the box, under the fades: slid out, not cut. */
static lv_opa_t edged(int i, lv_opa_t opa)
{
    int32_t r = line_row(i), last = (L->rows - 1) * ONE;
    int32_t out = r < 0 ? -r : r > last ? r - last : 0;
    int32_t f = ONE - out;
    return f <= 0 ? 0 : (lv_opa_t)(opa * f / ONE);
}

static int32_t line_x(int i)
{
    int c = cells(L->text + L->lines[i].start, L->lines[i].len);
    return PAD_X + (L->align == LV_TEXT_ALIGN_CENTER ? (L->w - c * L->cw) / 2 : 0);
}

static void scroll_step(void *box, int32_t v)
{
    (void)box;
    L->s = v;
    layout();
}

static void scroll_to(int line, uint32_t ms, lv_anim_path_cb_t path)
{
    int32_t to = line * ONE;
    lv_anim_delete(L->box, scroll_step);
    if (to == L->s) {
        return;
    }
    if (!ms) {
        scroll_step(L->box, to);
        return;
    }
    anim(L->box, scroll_step, L->s, to, ms, path, NULL);
}

/* The labels for the lines in view, where they go and how bright. */
static void layout(void)
{
    if (!L->n) {
        return;
    }
    int first = view() / ONE - L->focus - 1, last = first + L->rows + 2;
    for (int i = first < 0 ? 0 : first; i <= last && i < L->n; i++) {
        int k = i % L->pool;
        if (L->line_of[k] != i) {
            set_part(L->line[k], L->text + L->lines[i].start, L->lines[i].len);
            L->line_of[k] = i;
            L->y_of[k] = INT32_MIN;
            L->opa_of[k] = -1;
        }
    }
    for (int k = 0; k < L->pool; k++) {
        int i = L->line_of[k];
        bool shown = i >= first && i <= last && i < L->n && i != L->cur;
        set_hidden(L->line[k], !shown);
        if (!shown) {
            continue;
        }
        int32_t y = line_y(i), d = i * ONE - L->s;
        lv_opa_t opa = edged(i, i < L->cur ? tier(d) : LV_MIN(tier(d), OPA_NEXT));
        if (y != L->y_of[k]) {
            lv_obj_set_pos(L->line[k], line_x(i), y);
            L->y_of[k] = y;
        }
        if (opa != L->opa_of[k]) {
            lv_obj_set_style_text_opa(L->line[k], opa, 0);
            L->opa_of[k] = opa;
        }
    }
    /* The line being said, in its three parts. */
    bool shown = L->cur >= first && L->cur <= last;
    set_hidden(L->said, !shown || !L->word_at);
    set_hidden(L->word, !shown || L->lit == L->word_at);
    set_hidden(L->ahead, !shown || L->lit >= L->lines[L->cur].len);
    if (shown) {
        const char *t = L->text + L->lines[L->cur].start;
        int32_t x = line_x(L->cur), y = line_y(L->cur), d = L->cur * ONE - L->s;
        lv_opa_t opa = edged(L->cur, tier(d)), ahead = edged(L->cur, LV_MIN(tier(d), OPA_AHEAD));
        lv_obj_set_pos(L->said, x, y);
        lv_obj_set_pos(L->word, x + cells(t, L->word_at) * L->cw, y);
        lv_obj_set_pos(L->ahead, x + cells(t, L->lit) * L->cw, y);
        lv_obj_set_style_text_opa(L->said, opa, 0);
        lv_obj_set_style_text_opa(L->word, opa, 0);
        lv_obj_set_style_text_opa(L->ahead, ahead, 0);
    }
    if (!lv_obj_has_flag(L->pause, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_pos(L->pause, PAD_X + L->w + 4, L->margin + L->focus * L->pitch);
    }
}

/* ---- Following the speech ---- */

static void pop_step(void *o, int32_t v)
{
    int32_t scale = POP_SCALE - (POP_SCALE - 256) * v / 256;
    lv_obj_set_style_transform_scale(o, scale, 0);
    uint8_t mix = v > 255 ? 255 : (uint8_t)v;
    lv_obj_set_style_text_color(o, lv_color_mix(lv_color_hex(COLOR_LYRIC), lv_color_hex(COLOR_POP), mix), 0);
}

static void set_current(bool pop)
{
    const char *t = L->text + L->lines[L->cur].start;
    size_t len = L->lines[L->cur].len;
    set_part(L->said, t, L->word_at);
    set_part(L->word, t + L->word_at, L->lit - L->word_at);
    set_part(L->ahead, t + L->lit, len - L->lit);
    if (pop) {
        anim(L->word, pop_step, 0, 256, POP_MS, lv_anim_path_ease_out, NULL);
    } else if (!lv_anim_get(L->word, pop_step)) {
        pop_step(L->word, 256);
    }
}

/* The line and words lit for L->at; up a line as the speech turns one. */
static void follow(void)
{
    int cur = muse_lyrics_line_at(L->lines, L->n, L->at);
    size_t word, lit = muse_lyrics_lit(L->text, &L->lines[cur], L->at, &word);
    bool turned = cur != L->cur;
    if (!turned && lit == L->lit && word == L->word_at) {
        return;
    }
    bool pop = L->lit != SIZE_MAX && lit && (turned ? cur > L->cur : lit > L->lit);
    L->cur = cur;
    L->lit = lit;
    L->word_at = word;
    set_current(pop);
    if (turned && !L->browsing && !L->pressed) {
        scroll_to(cur, SCROLL_MS, lv_anim_path_ease_out);
    }
    layout();
}

/* Not spoken: on through the text at reading pace, holding at its pauses. */
static void read_on(float now, bool wait)
{
    float dt = now - L->read_last;
    L->read_last = now;
    dt = dt < 0 ? 0 : dt > 0.5f ? 0.5f : dt;   /* a stall isn't read through */
    if (!L->paused && !L->pressed && !wait) {
        L->read_w += dt * READ_CPS;
    }
    if (L->read_w > (float)L->weight) {
        L->read_w = (float)L->weight;
    }
    L->at = muse_lyrics_at(L->text, L->len, (size_t)L->read_w);
    bool done = L->read_w >= (float)L->weight && !L->live && !wait;
    if (!done) {
        L->done_at = 0;
    } else if (!L->done_at) {
        L->done_at = now;
    }
}

static bool holding(void)
{
    if (L->closed || !L->n) {
        return false;
    }
    if (L->browsing || L->pressed) {
        return true;   /* scrolled back: up till it's back */
    }
    if (L->spoken) {
        return false;
    }
    if (L->paused) {
        return L->now - L->paused_at < PAUSED_MAX_S;
    }
    return !L->done_at || L->now - L->done_at < READ_HOLD_S;
}

/* From the start: a new reply, or one shown again. */
static void restart(void)
{
    lv_anim_delete(L->box, scroll_step);
    lv_anim_delete(L->word, pop_step);
    for (int k = 0; k < POOL_MAX; k++) {
        L->line_of[k] = -1;
    }
    if (!L->spoken) {
        L->at = 0;   /* read from the top */
    }
    L->cur = L->n ? muse_lyrics_line_at(L->lines, L->n, L->at) : 0;
    L->s = L->cur * ONE;
    L->lit = SIZE_MAX;
    L->word_at = 0;
    L->browsing = L->paused = L->closed = false;
    L->read_w = 0;
    L->read_last = L->now;
    L->done_at = 0;
    set_hidden(L->pause, true);
}

/* The reply's text, new or grown, from L->in. */
static void take(void)
{
    int was_n = L->n;
    size_t keep = was_n ? L->lines[was_n - 1].start : 0;   /* the last line may wrap again as it grows */
    bool more = L->up && L->len && was_n && strlen(L->in) >= L->len && !strncmp(L->in, L->text, keep);
    char *t = L->text;
    L->text = L->in;
    L->in = t;
    L->len = strlen(L->text);
    L->weight = muse_lyrics_weight(L->text, L->len);
    L->n = muse_lyrics_wrap(L->text, L->cols, L->lines, LINES_MAX);
    if (!more) {
        restart();
        return;
    }
    for (int k = 0; k < POOL_MAX; k++) {
        if (L->line_of[k] >= was_n - 1) {
            L->line_of[k] = -1;   /* the lines from the last one on: set again */
        }
    }
    L->cur = L->cur < L->n ? L->cur : L->n - 1;
    L->lit = SIZE_MAX;   /* its words again, no pop */
}

/* Faded out: gone, so it takes no taps meant for the face. */
static void faded(lv_anim_t *a)
{
    (void)a;
    if (!L->up) {
        set_hidden(L->box, true);
    }
}

static void show(bool on)
{
    if (on == L->up) {
        return;
    }
    L->up = on;
    if (on) {
        restart();
        lv_obj_set_style_opa(L->box, LV_OPA_TRANSP, 0);
        set_hidden(L->box, false);
        anim(L->box, fade_opa, LV_OPA_TRANSP, LV_OPA_COVER, FADE_MS, lv_anim_path_ease_in_out, NULL);
    } else {
        L->pressed = false;
        anim(L->box, fade_opa, lv_obj_get_style_opa(L->box, 0), LV_OPA_TRANSP, FADE_MS, lv_anim_path_ease_in_out,
             faded);
    }
}

/* ---- Touch ---- */

static void plate_to(lv_opa_t to, uint32_t ms)
{
    anim(L->plate, fade_opa, lv_obj_get_style_opa(L->plate, 0), to, ms, lv_anim_path_ease_out, NULL);
}

static void set_paused(bool paused)
{
    L->paused = paused;
    L->paused_at = L->now;
    set_hidden(L->pause, !paused);
    layout();
    ESP_LOGI(TAG, "%s", paused ? "paused" : "reading on");
}

static void on_touch(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (!L->up) {
        return;
    }
    int32_t lo = 0, hi = (L->spoken ? L->cur : L->n - 1) * ONE;   /* back to the start; ahead only if read */
    switch (code) {
    case LV_EVENT_PRESSED:
        L->pressed = true;
        L->moved = false;
        L->drag = 0;
        L->drag_s0 = L->s;
        plate_to(LV_OPA_COVER, PRESS_IN_MS);
        break;
    case LV_EVENT_PRESSING: {
        lv_point_t v;
        lv_indev_get_vect(lv_indev_active(), &v);
        L->drag += v.y;
        if (!L->moved && abs(L->drag) > TAP_SLOP) {
            L->moved = true;
            lv_anim_delete(L->box, scroll_step);
            L->drag_s0 = L->s + L->drag * ONE / L->pitch;   /* no jump for the slop */
        }
        if (L->moved) {
            int32_t s = L->drag_s0 - L->drag * ONE / L->pitch;
            L->s = s > hi ? hi : s < lo ? lo : s;
            layout();
        }
        break;
    }
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        L->pressed = false;
        plate_to(LV_OPA_TRANSP, PRESS_OUT_MS);
        if (!L->moved) {
            break;
        }
        {
            int line = LV_MIN((LV_MAX(L->s, L->focus * ONE) + ONE / 2) / ONE, hi / ONE);   /* at the focus row */
            scroll_to(line, SNAP_MS, lv_anim_path_ease_out);
            if (L->spoken) {
                L->browsing = line != L->cur;
                L->let_go_ms = lv_tick_get();
            } else {
                /* Moved: reading goes on from the line at the focus row, or
                 * at the top, from the start. */
                line = line <= L->focus ? 0 : line;
                L->at = L->lines[line].start + 1;   /* its first word begun */
                L->read_w = (float)muse_lyrics_weight(L->text, L->at);
                L->done_at = 0;
                follow();
            }
        }
        break;
    case LV_EVENT_SHORT_CLICKED:
        if (L->moved) {
            lv_event_stop_processing(e);   /* a drag: no click */
        } else if (L->spoken) {
            open_sheet();
        } else {
            set_paused(!L->paused);
        }
        break;
    case LV_EVENT_LONG_PRESSED:
        if (!L->moved) {
            muse_style_click(false);   /* held long enough: the tap a hold makes */
            open_sheet();
        }
        break;
    default:
        break;
    }
}

/* ---- The whole reply's sheet ---- */

static void sheet_gone(lv_anim_t *a)
{
    (void)a;
    if (L->sheet) {
        lv_obj_delete_async(L->sheet);   /* we may be in one of its own events */
        L->sheet = L->sheet_col = L->sheet_text = L->sheet_line = L->sheet_fade[0] = L->sheet_fade[1] = NULL;
    }
}

/* Down and off the screen. `done`: done with the reply, the captions too. */
static void close_sheet(bool done)
{
    if (done) {
        L->closed = true;
    }
    if (!L->sheet || lv_anim_get(L->sheet, fade_opa)) {
        return;
    }
    ESP_LOGI(TAG, "sheet %s", done ? "done with" : "put away");
    lv_anim_delete(L->sheet, translate_y);
    anim(L->sheet, translate_y, lv_obj_get_style_translate_y(L->sheet, 0), lv_obj_get_height(L->sheet) + SHEET_BOTTOM,
         AWAY_MS, lv_anim_path_ease_in, sheet_gone);
    anim(L->sheet, fade_opa, LV_OPA_COVER, done ? LV_OPA_TRANSP : LV_OPA_COVER, AWAY_MS, lv_anim_path_ease_in, NULL);
}

static void on_sheet_close(lv_event_t *e)
{
    (void)e;
    close_sheet(true);
}

static void on_sheet_gesture(lv_event_t *e)
{
    (void)e;
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_BOTTOM) {
        close_sheet(false);
    }
}

/* The header follows a finger down; let go far enough down, the sheet goes, else it springs back. */
static void on_sheet_drag(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (!L->sheet || lv_anim_get(L->sheet, fade_opa)) {
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        L->sheet_drag = 0;
        lv_anim_delete(L->sheet, translate_y);
    } else if (code == LV_EVENT_PRESSING) {
        lv_point_t v;
        lv_indev_get_vect(lv_indev_active(), &v);
        L->sheet_drag = L->sheet_drag + v.y > 0 ? L->sheet_drag + v.y : 0;
        lv_obj_set_style_translate_y(L->sheet, L->sheet_drag, 0);
    } else if (L->sheet_drag > DRAG_AWAY_PX) {
        close_sheet(false);
    } else if (L->sheet_drag > 0) {
        anim(L->sheet, translate_y, L->sheet_drag, 0, 200, lv_anim_path_ease_out, NULL);
    }
}

static void on_sheet_scroll(lv_event_t *e)
{
    (void)e;
    if (L->sheet_fade[0]) {
        set_hidden(L->sheet_fade[0], lv_obj_get_scroll_top(L->sheet_col) <= 1);
        set_hidden(L->sheet_fade[1], lv_obj_get_scroll_bottom(L->sheet_col) <= 1);
    }
}

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *edge_fade(bool top)
{
    lv_obj_t *f = box(L->sheet);
    lv_obj_add_flag(f, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(f, lv_color_hex(MUSE_COLOR_CARD), 0);
    lv_obj_set_style_bg_grad_color(f, lv_color_hex(MUSE_COLOR_CARD), 0);
    lv_obj_set_style_bg_grad_dir(f, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_opa(f, top ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_opa(f, top ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    return f;
}

static void sheet_words(void)
{
    int words = 0;
    for (const char *p = L->text; *p; p++) {
        words += *p != ' ' && *p != '\n' && (p == L->text || p[-1] == ' ' || p[-1] == '\n');
    }
    lv_label_set_text_fmt(L->sheet_line, "%d word%s", words, words == 1 ? "" : "s");
}

/* The column as tall as the reply, up to the room there is, then scrolling. */
static void sheet_size(void)
{
    lv_obj_set_height(L->sheet_col, LV_SIZE_CONTENT);
    lv_obj_update_layout(L->sheet);
    int room = (L->sh - SHEET_BOTTOM - SHEET_TOP) - lv_obj_get_style_pad_top(L->sheet, 0)
               - lv_obj_get_style_pad_bottom(L->sheet, 0) - 2 * MUSE_CARD_BORDER
               - lv_obj_get_height(lv_obj_get_child(L->sheet, 0)) - lv_obj_get_style_pad_row(L->sheet, 0);
    if (lv_obj_get_height(L->sheet_col) <= room) {
        return;
    }
    lv_obj_set_height(L->sheet_col, room);
    lv_obj_update_layout(L->sheet);
    int x = lv_obj_get_x(L->sheet_col), y = lv_obj_get_y(L->sheet_col);
    int w = lv_obj_get_width(L->sheet_col) - SCROLLBAR_ROOM;
    lv_obj_set_size(L->sheet_fade[0], w, FADE_H);
    lv_obj_set_pos(L->sheet_fade[0], x, y);
    lv_obj_set_size(L->sheet_fade[1], w, FADE_H);
    lv_obj_set_pos(L->sheet_fade[1], x, y + room - FADE_H);
    on_sheet_scroll(NULL);
}

static void open_sheet(void)
{
    if (L->sheet || !L->len) {
        return;
    }
    lv_obj_t *s = L->sheet = lv_obj_create(L->face);
    lv_obj_remove_style_all(s);
    muse_style_card(s);
    lv_obj_set_style_pad_top(s, SHEET_PAD_TOP, 0);
    lv_obj_set_width(s, L->sw - 2 * SHEET_SIDE);
    lv_obj_set_height(s, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);   /* taps on it stay on it */
    lv_obj_add_event_cb(s, on_sheet_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_align(s, LV_ALIGN_BOTTOM_MID, 0, -SHEET_BOTTOM);

    /* Grabber, then the title and its line beside the close button: dragged down, it takes the sheet. */
    lv_obj_t *head = box(s);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(head, 6, 0);
    lv_obj_add_flag(head, LV_OBJ_FLAG_CLICKABLE);
    static const lv_event_code_t DRAG[] = { LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED,
                                            LV_EVENT_PRESS_LOST };
    for (size_t i = 0; i < sizeof(DRAG) / sizeof(DRAG[0]); i++) {
        lv_obj_add_event_cb(head, on_sheet_drag, DRAG[i], NULL);
    }
    lv_obj_t *grab = box(head);
    lv_obj_set_size(grab, GRABBER_W, GRABBER_H);
    lv_obj_set_style_radius(grab, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(grab, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(grab, lv_color_hex(MUSE_COLOR_OFF), 0);
    lv_obj_t *row = box(head);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, MUSE_ROW_GAP_X, 0);
    lv_obj_t *words = box(row);
    lv_obj_set_height(words, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(words, 1);
    lv_obj_set_flex_flow(words, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(words, 2, 0);
    muse_style_label(words, MUSE_FONT_CARD_TITLE, MUSE_COLOR_TEXT, "What Muse said");
    L->sheet_line = muse_style_label(words, MUSE_FONT_NOTE, MUSE_COLOR_DIM, "");
    lv_obj_t *x = lv_button_create(row);
    lv_obj_remove_style_all(x);
    lv_obj_set_size(x, CLOSE_D, CLOSE_D);
    lv_obj_set_ext_click_area(x, (52 - CLOSE_D) / 2);
    lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(x, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(x, lv_color_hex(MUSE_COLOR_CARD_PRESSED), 0);
    lv_obj_remove_flag(x, LV_OBJ_FLAG_SCROLLABLE);
    muse_style_pressable(x, MUSE_PRESS_KEY, false);
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);   /* its ring, let go, past the row's end */
    lv_obj_center(muse_style_label(x, MUSE_FONT_NOTE, MUSE_COLOR_DIM, LV_SYMBOL_CLOSE));
    lv_obj_add_event_cb(x, on_sheet_close, LV_EVENT_CLICKED, NULL);

    /* The reply: the cards' type, unless it has CJK, which only the caption font has. */
    L->sheet_col = box(s);
    lv_obj_set_width(L->sheet_col, lv_pct(100));
    lv_obj_set_style_pad_right(L->sheet_col, SCROLLBAR_ROOM, 0);
    lv_obj_set_style_pad_bottom(L->sheet_col, 4, 0);
    muse_style_scroll_column(L->sheet_col, 4, 0, 4, 4);
    lv_obj_add_event_cb(L->sheet_col, on_sheet_scroll, LV_EVENT_SCROLL, NULL);
    bool cjk = muse_text_has_cjk(L->text);
    L->sheet_text = muse_style_label(L->sheet_col, cjk ? L->font : MUSE_FONT_BUTTON, MUSE_COLOR_TEXT, L->text);
    lv_obj_set_width(L->sheet_text, lv_pct(100));
    lv_label_set_long_mode(L->sheet_text, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_line_space(L->sheet_text, cjk ? 4 : 6, 0);
    L->sheet_fade[0] = edge_fade(true);
    L->sheet_fade[1] = edge_fade(false);
    sheet_words();
    sheet_size();
    L->sheet_ver = L->ver;
    ESP_LOGI(TAG, "sheet up: %u chars", (unsigned)L->len);
    lv_obj_update_layout(s);
    anim(s, translate_y, lv_obj_get_height(s) + SHEET_BOTTOM, 0, OPEN_MS, lv_anim_path_overshoot, NULL);
}

/* The reply grown while the sheet's up: it grows too, where it's read left alone. */
static void sheet_tick(void)
{
    if (!L->sheet || L->sheet_ver == L->ver || lv_anim_get(L->sheet, fade_opa)) {
        return;
    }
    L->sheet_ver = L->ver;
    lv_label_set_text(L->sheet_text, L->text);
    sheet_words();
    sheet_size();
}

/* ---- The face's side ---- */

bool muse_lyrics_ui_build(lv_obj_t *face, const lv_font_t *font, int line_space, int sw, int sh)
{
    L = heap_caps_calloc(1, sizeof(*L), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *text = heap_caps_calloc(2, MUSE_REPLY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    muse_lyrics_line_t *lines = heap_caps_calloc(LINES_MAX, sizeof(*lines), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!L || !text || !lines) {
        free(L);
        free(text);
        free(lines);
        L = NULL;
        ESP_LOGW(TAG, "no PSRAM: paged captions");
        return false;
    }
    L->text = text;
    L->in = text + MUSE_REPLY_MAX;
    L->lines = lines;
    L->face = face;
    L->font = font;
    L->sw = sw;
    L->sh = sh;
    L->cw = lv_font_get_glyph_width(font, 'M', ' ');
    L->pitch = lv_font_get_line_height(font) + line_space;
    L->margin = L->pitch / 2;
    L->ver = UINT32_MAX;

    L->box = lv_obj_create(face);
    lv_obj_remove_style_all(L->box);
    lv_obj_remove_flag(L->box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(L->box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    static const lv_event_code_t TOUCH[] = { LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED,
                                             LV_EVENT_PRESS_LOST, LV_EVENT_SHORT_CLICKED, LV_EVENT_LONG_PRESSED };
    for (size_t i = 0; i < sizeof(TOUCH) / sizeof(TOUCH[0]); i++) {
        lv_obj_add_event_cb(L->box, on_touch, TOUCH[i], NULL);   /* first: a drag stops the click */
    }
    muse_style_pressable(L->box, MUSE_PRESS_ROW, false);
    /* Touched, a plate lights behind the lines, as a row's colour deepens. */
    L->plate = box(L->box);
    lv_obj_set_style_radius(L->plate, MUSE_ROW_RADIUS, 0);
    lv_obj_set_style_bg_opa(L->plate, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(L->plate, lv_color_hex(MUSE_COLOR_CARD), 0);
    lv_obj_set_style_opa(L->plate, LV_OPA_TRANSP, 0);
    for (int k = 0; k < POOL_MAX; k++) {
        L->line[k] = lyric(L->box);
    }
    L->said = lyric(L->box);
    L->ahead = lyric(L->box);
    L->word = lyric(L->box);
    lv_obj_set_style_transform_pivot_x(L->word, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(L->word, lv_pct(50), 0);
    for (int m = 0; m < 2; m++) {
        lv_obj_t *f = L->mask[m] = box(L->box);
        lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(f, lv_color_hex(COLOR_FACE), 0);
        lv_obj_set_style_bg_grad_color(f, lv_color_hex(COLOR_FACE), 0);
        lv_obj_set_style_bg_grad_dir(f, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_main_opa(f, m ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        lv_obj_set_style_bg_grad_opa(f, m ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    L->pause = muse_style_label(L->box, MUSE_FONT_NOTE, MUSE_COLOR_DIM, LV_SYMBOL_PAUSE);
    lv_obj_add_flag(L->pause, LV_OBJ_FLAG_HIDDEN);
    return true;
}

void muse_lyrics_ui_place(int w, int top, int cols, int rows, lv_text_align_t align)
{
    if (!L) {
        return;
    }
    rows = rows < 2 ? 2 : rows;
    bool rewrap = cols != L->cols;
    L->w = w;
    L->top = top;
    L->cols = cols;
    L->rows = rows;
    L->focus = rows >= 3 ? 1 : 0;
    L->align = align;
    L->pool = rows + 3 < POOL_MAX ? rows + 3 : POOL_MAX;
    int h = rows * L->pitch - (L->pitch - lv_font_get_line_height(L->font)) + 2 * L->margin;
    lv_obj_set_size(L->box, w + 2 * PAD_X, h);
    lv_obj_align(L->box, LV_ALIGN_CENTER, 0, top - L->margin + h / 2);
    lv_obj_set_size(L->plate, w + PAD_X, h - L->margin);
    lv_obj_set_pos(L->plate, PAD_X / 2, L->margin / 2);
    int mh = L->margin + MASK_IN;
    for (int m = 0; m < 2; m++) {
        lv_obj_set_size(L->mask[m], w + 2 * PAD_X, mh);
        lv_obj_set_pos(L->mask[m], 0, m ? h - mh : 0);
    }
    for (int k = 0; k < POOL_MAX; k++) {
        L->line_of[k] = -1;
        set_hidden(L->line[k], true);
    }
    if (rewrap && L->len) {
        /* Another layout's width: the same place in the reply, wrapped again. */
        L->n = muse_lyrics_wrap(L->text, cols, L->lines, LINES_MAX);
        L->cur = muse_lyrics_line_at(L->lines, L->n, L->at);
        L->lit = SIZE_MAX;
        lv_anim_delete(L->box, scroll_step);
        L->s = L->cur * ONE;
        L->browsing = false;
        follow();
    }
    layout();
}

bool muse_lyrics_ui_tick(float now, bool room)
{
    if (!L) {
        return false;
    }
    L->now = now;
    size_t at;
    bool spoken;
    uint32_t was = L->ver;
    bool live = muse_state_reply(L->in, MUSE_REPLY_MAX, &L->ver, &at, &spoken);
    if (live) {
        if (L->spoken && !spoken && L->up) {
            L->read_w = (float)muse_lyrics_weight(L->text, L->at);   /* not spoken after all: read on from there */
        }
        L->spoken = spoken;
        if (spoken) {
            L->at = at;
        }
        if (L->ver != was) {
            take();
        }
    }
    L->live = live;
    char caption[2];
    if (muse_state_caption(caption, sizeof(caption), &L->caption_ver) && !live && caption[0]) {
        L->closed = true;   /* another caption: one held after its turn gives way */
    }
    bool want = room && L->n && (live || (!L->closed && holding()));
    if (!want) {
        show(false);
        return false;
    }
    show(true);
    /* Under the reply's sheet, the captions keep out of sight, and reading waits. */
    bool covered = L->sheet && !lv_anim_get(L->sheet, fade_opa);
    set_hidden(L->box, covered);
    if (!L->spoken) {
        read_on(now, covered);
    } else {
        L->read_last = now;
    }
    if (L->browsing && !L->pressed && lv_tick_elaps(L->let_go_ms) > BACK_AFTER_MS) {
        L->browsing = false;
        scroll_to(L->cur, BACK_MS, lv_anim_path_ease_in_out);
    }
    if (L->paused && L->spoken) {
        set_paused(false);   /* spoken after all (the speaker back on) */
    }
    follow();
    sheet_tick();
    return true;
}

bool muse_lyrics_ui_holding(void)
{
    return L && L->up && !L->live && !L->closed && holding();
}

void muse_lyrics_ui_drop(void)
{
    if (!L) {
        return;
    }
    if (L->sheet) {
        lv_anim_delete(L->sheet, NULL);
        lv_obj_delete(L->sheet);
        L->sheet = L->sheet_col = L->sheet_text = L->sheet_line = L->sheet_fade[0] = L->sheet_fade[1] = NULL;
    }
    lv_anim_delete(L->box, NULL);
    set_hidden(L->box, true);
    L->up = L->pressed = false;
    L->n = 0;
    L->len = 0;
    memset(L->text, 0, MUSE_REPLY_MAX);   /* nothing of it kept */
    L->ver = UINT32_MAX;
    for (int k = 0; k < POOL_MAX; k++) {
        lv_label_set_text(L->line[k], "");
    }
    lv_label_set_text(L->said, "");
    lv_label_set_text(L->word, "");
    lv_label_set_text(L->ahead, "");
}

bool muse_lyrics_ui_sheet_up(void)
{
    return L && L->sheet;
}
