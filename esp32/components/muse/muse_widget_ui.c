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
 * The widget sheet (muse_widget_ui.h), on the 2.16's 480 px square:
 *
 *   y   0..122  Muse at 2 px cells (muse_ui.c's ANSWER_WIDGET)
 *   y 126..160  the reply's captions, two lines
 *   y  ~168     the sheet's highest top: a card 456 px wide, 12 px in from
 *               either side, its bottom at 454 (clear of the page dots);
 *               as tall as what's on it, the column scrolling past that
 *   y 412..446  the chip that brings it back, where "up next" goes
 *
 * Inside the card (16 px padding, the dialogs'): a grabber, the header (a
 * back arrow on a row's card, the title in the card title's type, its line
 * dim under it, the close button on the right), then the column. Rows are
 * muse_style_row's on a card, at least MUSE_BUTTON_H tall, the text wrapping
 * rather than cut: a leading icon in the accent colour (a map's places their
 * pin's number), the title over a dim line or two, then on the right a
 * price, a site or how far, and a button where the row answers by one.
 *
 * What a tap does, by row: an option, a "send" row or a flight's button
 * answers at once; a choice ticks; a field opens the keyboard
 * (muse_widget_keys.h); the rest (places, products, links, events, mail)
 * open their card in the sheet, its own buttons answering. A form or a
 * multi-select ends in its button, filled once it can send.
 *
 * Putting it away, everywhere the same: a swipe down (or the header dragged
 * down) tucks the sheet into the chip, which a tap brings back; the close
 * button, the chip's own x, or the chip flung sideways is done with it: it
 * goes in a puff. Answering is done with it too. The browser's history
 * ("What Muse did") is the same sheet and the same chip.
 */
#include "muse_widget_ui.h"

#if MUSE_WIDGET_UI

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "muse_browse.h"
#include "muse_chat.h"
#include "muse_style.h"
#include "muse_widget.h"
#include "muse_widget_keys.h"
#include "muse_widget_map.h"

static const char *TAG = "muse_widget";

#define SHEET_SIDE 12           /* in from the screen's sides */
#define SHEET_BOTTOM 26         /* up from its bottom: the page dots show under it */
#define SHEET_PAD_TOP 8         /* the grabber's room */
#define GRABBER_W 40
#define GRABBER_H 5
#define CLOSE_D 36              /* the close and back buttons, 52 px to hit */
#define ROW_PAD_V 10
#define ICON_W 28               /* a row's leading icon's column */
#define PILL_PAD 18             /* an option pill's text from its ends */
#define PILLS_MAX 3
#define SCROLLBAR_ROOM 10       /* the column's right, while it scrolls */
#define FADE_H 24               /* the column's edges, while there's more that way */
#define DRAG_AWAY_PX 60         /* the header dragged down this far puts the sheet away */
#define DOT_GAP 8.0f            /* "dotted_lines": dots round an option pill */
#define DOT_PX 3
#define MAP_H 150               /* a map widget's map, the column's width */
#define MAP_CARD_H 100          /* on a place's card */
#define CARD_BUTTON_H 48        /* a card's buttons, two across */
#define PIC_D 112               /* a product's picture on its card */
#define OPEN_MS 320
#define AWAY_MS 220
#define FOLD_MS 260
#define PAGE_MS 220             /* a card in, or back to the rows */
#define SENT_HOLD_MS 1300       /* the "sent" chip, before it goes */
#define TOAST_HOLD_MS 1600
#define THINKING_OPEN_S 4.0f    /* widgets in, the speech held up (an image): opened anyway */
#define CHIP_CAPTION_S 3.0f     /* a new caption keeps the chip out of its way this long */
#define CHIP_BOTTOM 34          /* "up next"'s place (muse_home_extras.c) */
#define CHIP_H 40
#define CHIP_TEXT_W 260
#define CHIP_FLING_PX 70        /* dragged sideways this far, let go: flung away */
#define BROWSE_DONE_S 2.4f      /* Muse's flourish and the laptop shut, after the task's done */
#define BROWSE_QUIET_MS 90000   /* no word of a running task this long: he's done with it */
#define FIELDS_MAX 6            /* a form's fields that keep what's typed */
#define VALUE_MAX 201
#define COLOR_CHIP_BG 0x1d1733  /* "up next"'s look */
#define COLOR_CHIP_EDGE 0x5b3fa0
#define COLOR_CHIP_TEXT 0xe4defa
#define COLOR_LINE 0x3a3358     /* the browser's timeline */
#define FONT_SMALL (&lv_font_montserrat_14)
#define NONE (-1)

typedef enum {
    ST_NONE,      /* no widgets, or done with them */
    ST_WAITING,   /* in, the reply not yet being said */
    ST_OPEN,      /* the sheet's up */
    ST_SENT,      /* answered: folding into the "sent" chip, then gone */
    ST_AWAY,      /* put away: the chip brings it back */
} state_t;

typedef enum { VIEW_WIDGETS, VIEW_BROWSE } view_t;

EXT_RAM_BSS_ATTR static muse_widget_set_t *s_set;   /* the face's copy, PSRAM */
EXT_RAM_BSS_ATTR static uint32_t s_seq;
EXT_RAM_BSS_ATTR static state_t s_state;
EXT_RAM_BSS_ATTR static view_t s_view;               /* what the sheet (and the chip) is of */
EXT_RAM_BSS_ATTR static int s_detail;                /* the row whose card is up (wi << 8 | ri), or NONE */
EXT_RAM_BSS_ATTR static float s_since;               /* when the set came */
EXT_RAM_BSS_ATTR static int s_w, s_h, s_top;
EXT_RAM_BSS_ATTR static lv_obj_t *s_face, *s_sheet, *s_scroll, *s_fade[2], *s_toast;
EXT_RAM_BSS_ATTR static lv_obj_t *s_chip, *s_chip_icon, *s_chip_lbl, *s_chip_x;
EXT_RAM_BSS_ATTR static int32_t s_drag;               /* the header dragged down this far */
EXT_RAM_BSS_ATTR static int32_t s_chip_dx;            /* the chip dragged sideways this far */
EXT_RAM_BSS_ATTR static bool s_chip_dragged;
EXT_RAM_BSS_ATTR static char s_sent[MUSE_WIDGET_ROW_TITLE];
EXT_RAM_BSS_ATTR static lv_style_transition_dsc_t s_press_tr;
EXT_RAM_BSS_ATTR static uint32_t s_caption_ver;      /* the caption the chip last saw, */
EXT_RAM_BSS_ATTR static float s_caption_at;          /* and when it was new (0: never) */
EXT_RAM_BSS_ATTR static uint32_t s_checked[MUSE_WIDGET_MAX];   /* a multi-select's ticks, by row */
EXT_RAM_BSS_ATTR static char s_values[MUSE_WIDGET_MAX][FIELDS_MAX][VALUE_MAX];   /* a form's typed words */
EXT_RAM_BSS_ATTR static int s_typing;                /* the field being typed (wi << 8 | ri) */
/* The browser's history */
EXT_RAM_BSS_ATTR static muse_browse_t *s_browse;
EXT_RAM_BSS_ATTR static uint32_t s_browse_seq, s_browse_turn;
EXT_RAM_BSS_ATTR static bool s_browse_dismissed;     /* done with it: no chip, till the next turn */
EXT_RAM_BSS_ATTR static int64_t s_browse_heard_ms;   /* its last change */
EXT_RAM_BSS_ATTR static lv_obj_t *s_browse_times[MUSE_BROWSE_STEPS];
EXT_RAM_BSS_ATTR static int s_browse_shown;          /* steps on the sheet */
EXT_RAM_BSS_ATTR static int64_t s_times_at;

static void open_sheet(bool slide_in, int dir);

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* ---- Pieces ---- */

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *t, bool wrap)
{
    lv_obj_t *l = muse_style_label(parent, font, color, t);
    if (wrap) {
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_line_space(l, 2, 0);
    }
    return l;
}

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* Pressed, a thing gives a little, as the settings' rows darken: both. */
static void press_look(lv_obj_t *o)
{
    lv_obj_set_style_transform_width(o, -3, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(o, -2, LV_STATE_PRESSED);
    lv_obj_set_style_transition(o, &s_press_tr, 0);
    lv_obj_set_style_transition(o, &s_press_tr, LV_STATE_PRESSED);
}

static void fade_opa(void *o, int32_t v)
{
    lv_obj_set_style_opa(o, (lv_opa_t)v, 0);
}

static void translate_x(void *o, int32_t v)
{
    lv_obj_set_style_translate_x(o, v, 0);
}

static void translate_y(void *o, int32_t v)
{
    lv_obj_set_style_translate_y(o, v, 0);
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
    lv_anim_start(&a);
}

static void show_toast(const char *t)
{
    if (!s_toast) {
        return;
    }
    lv_label_set_text(lv_obj_get_child(s_toast, 0), t);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_toast);
    lv_anim_set_exec_cb(&a, fade_opa);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, 150);
    lv_anim_set_reverse_delay(&a, TOAST_HOLD_MS);
    lv_anim_set_reverse_duration(&a, 250);
    lv_anim_start(&a);
}

/* A round button: the close (x) and back (<) in the header, the chip's x. */
static lv_obj_t *round_button(lv_obj_t *parent, int d, const char *sym, lv_event_cb_t cb)
{
    lv_obj_t *x = lv_button_create(parent);
    lv_obj_remove_style_all(x);
    lv_obj_set_size(x, d, d);
    lv_obj_set_ext_click_area(x, (52 - d) / 2);
    lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(x, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(x, lv_color_hex(MUSE_COLOR_CARD_PRESSED), 0);
    lv_obj_set_style_bg_color(x, lv_color_hex(MUSE_COLOR_RAISED_PRESSED), LV_STATE_PRESSED);
    lv_obj_remove_flag(x, LV_OBJ_FLAG_SCROLLABLE);
    press_look(x);
    lv_obj_center(muse_style_label(x, d < 32 ? FONT_SMALL : MUSE_FONT_NOTE, MUSE_COLOR_DIM, sym));
    lv_obj_add_event_cb(x, cb, LV_EVENT_CLICKED, NULL);
    return x;
}

/* A pill button; `primary` filled in the accent colour, else outlined. */
static lv_obj_t *pill(lv_obj_t *parent, const char *label, bool primary, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = muse_style_button(parent, label, primary ? MUSE_BUTTON_ACCENT : MUSE_BUTTON_NEUTRAL, MUSE_FONT_BUTTON,
                                    MUSE_BUTTON_H);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(MUSE_COLOR_CARD_PRESSED), LV_STATE_DISABLED);
    lv_obj_set_style_border_opa(b, LV_OPA_TRANSP, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lv_color_hex(MUSE_COLOR_DIM), LV_STATE_DISABLED);
    press_look(b);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

/* ---- Icons: Montserrat's symbols, and the few it lacks, drawn ---- */

static void calendar_icon(lv_obj_t *col)
{
    lv_obj_t *c = box(col);
    lv_obj_set_size(c, 20, 20);
    lv_obj_set_style_radius(c, 4, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(MUSE_COLOR_ACCENT), 0);
    lv_obj_t *bar = box(c);
    lv_obj_set_size(bar, lv_pct(100), 6);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(MUSE_COLOR_ACCENT), 0);
    lv_obj_center(c);
}

static lv_obj_t *dot_in(lv_obj_t *col, int d, uint32_t color)
{
    lv_obj_t *p = box(col);
    lv_obj_set_size(p, d, d);
    lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(color), 0);
    lv_obj_center(p);
    return p;
}

/* A map's place: its pin, numbered as on the map. */
static void number_icon(lv_obj_t *col, int n)
{
    lv_obj_t *p = dot_in(col, 26, MUSE_COLOR_ACCENT);
    char t[4];
    snprintf(t, sizeof(t), "%d", n);
    lv_obj_center(muse_style_label(p, FONT_SMALL, MUSE_COLOR_CARD, t));
}

/* An option's ring, filled for "center_aligned_filled"; a choice's, ticked when it's on. */
static void ring_icon(lv_obj_t *col, bool filled, bool ticked)
{
    lv_obj_t *r = box(col);
    lv_obj_set_size(r, 22, 22);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(r, 2, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(ticked ? MUSE_COLOR_ACCENT : filled ? MUSE_COLOR_ACCENT : MUSE_COLOR_DIM),
                                  0);
    lv_obj_set_style_bg_opa(r, ticked ? LV_OPA_COVER : filled ? LV_OPA_40 : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(MUSE_COLOR_ACCENT), 0);
    if (ticked) {
        lv_obj_center(muse_style_label(r, FONT_SMALL, MUSE_COLOR_CARD, LV_SYMBOL_OK));
    }
    lv_obj_center(r);
}

/* A site's icon: its first letter on a rounded square of its colour. */
static lv_obj_t *site_icon(lv_obj_t *parent, const char *site, int d)
{
    lv_obj_t *s = box(parent);
    lv_obj_set_size(s, d, d);
    lv_obj_set_style_radius(s, d / 4, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s, lv_color_hex(site && site[0] ? muse_browse_color(site) : MUSE_COLOR_OFF), 0);
    char t[2] = { site && site[0] ? (char)toupper((unsigned char)site[0]) : '?', 0 };
    if (!site || !site[0]) {
        lv_obj_center(muse_style_label(s, FONT_SMALL, MUSE_COLOR_TEXT, LV_SYMBOL_REFRESH));
    } else {
        lv_obj_center(muse_style_label(s, d >= 40 ? MUSE_FONT_CARD_TITLE : MUSE_FONT_NOTE, 0x14111f, t));
    }
    return s;
}

static const char *kind_symbol(uint8_t kind)
{
    switch (kind) {
    case MUSE_WIDGET_OPTIONS: return LV_SYMBOL_NEW_LINE;   /* an answer to give */
    case MUSE_WIDGET_MAP: return LV_SYMBOL_GPS;
    case MUSE_WIDGET_SHOPPING: return "$";
    case MUSE_WIDGET_CARD: return LV_SYMBOL_FILE;
    case MUSE_WIDGET_FORM: return LV_SYMBOL_EDIT;
    case MUSE_WIDGET_CHECKS: return LV_SYMBOL_OK;
    default: return LV_SYMBOL_LIST;
    }
}

/* A row's leading icon, in a column of its own so every row's text lines up. */
static void leading(lv_obj_t *row, const muse_widget_row_t *r, bool filled, bool ticked, int number)
{
    lv_obj_t *col = box(row);
    lv_obj_set_size(col, ICON_W, 26);
    const char *sym = NULL;
    switch (r->type) {
    case MUSE_WIDGET_ROW_OPTION: ring_icon(col, filled, false); return;
    case MUSE_WIDGET_ROW_CHECK: ring_icon(col, false, ticked); return;
    case MUSE_WIDGET_ROW_CALENDAR: calendar_icon(col); return;
    case MUSE_WIDGET_ROW_PLACE:
        if (number > 0) {
            number_icon(col, number);
        } else {
            lv_obj_center(muse_style_label(col, MUSE_FONT_BUTTON, MUSE_COLOR_ACCENT, LV_SYMBOL_GPS));
        }
        return;
    case MUSE_WIDGET_ROW_LINK:
        if (r->meta[0]) {
            lv_obj_center(site_icon(col, r->meta, 26));
            return;
        }
        sym = LV_SYMBOL_UPLOAD;
        break;
    case MUSE_WIDGET_ROW_SEND: sym = LV_SYMBOL_NEW_LINE; break;
    case MUSE_WIDGET_ROW_EMAIL: sym = LV_SYMBOL_ENVELOPE; break;
    case MUSE_WIDGET_ROW_FLIGHT: sym = LV_SYMBOL_GPS; break;
    case MUSE_WIDGET_ROW_PRODUCT: sym = "$"; break;
    case MUSE_WIDGET_ROW_FIELD: sym = LV_SYMBOL_EDIT; break;
    default: sym = LV_SYMBOL_BULLET; break;
    }
    lv_obj_center(muse_style_label(col, MUSE_FONT_BUTTON, MUSE_COLOR_ACCENT, sym));
}

/* ---- Answering ---- */

static void fold(void *unused);

static const muse_widget_row_t *row_at(int at)
{
    int wi = at >> 8, ri = at & 0xff;
    if (!s_set || at < 0 || wi >= s_set->count || ri >= s_set->w[wi].count) {
        return NULL;
    }
    return &s_set->w[wi].rows[ri];
}

/* `send` to the widgets' chat as the user's message; the sheet folds into a chip saying `shown`, and they're done. */
static void answer_text(const char *send, const char *shown)
{
    if (s_state != ST_OPEN || !send || !send[0]) {
        return;
    }
    if (!muse_hatch_reply_text(s_set->sid, send)) {
        show_toast("Muse can't be reached right now");
        return;
    }
    ESP_LOGI(TAG, "answered: \"%s\"", send);
    muse_widget_text(s_sent, sizeof(s_sent), shown && shown[0] ? shown : send, false);
    s_state = ST_SENT;
    muse_widget_clear();   /* answered: none of them comes back */
    lv_async_call(fold, NULL);   /* out of the tapped thing's event before its sheet's emptied */
}

static void answer(const muse_widget_row_t *r)
{
    if (r) {
        answer_text(r->send, r->type == MUSE_WIDGET_ROW_OPTION || r->type == MUSE_WIDGET_ROW_SEND ? r->title : NULL);
    }
}

static void page_async(void *dir)
{
    if (s_sheet && (s_view == VIEW_BROWSE || s_state == ST_OPEN)) {
        open_sheet(false, (int)(intptr_t)dir);
    }
}

/* A row's card, or back to the rows: the sheet again (once out of the tap's event), sliding that way. */
static void go_detail(int at)
{
    s_detail = at;
    lv_async_call(page_async, (void *)(intptr_t)(at == NONE ? -1 : 1));
}

static void on_typed(const char *t, void *ctx);

static void open_keys(int at)
{
    const muse_widget_row_t *r = row_at(at);
    if (!r) {
        return;
    }
    const muse_widget_t *w = &s_set->w[at >> 8];
    int fields = 0;
    for (int i = 0; i < w->count; i++) {
        fields += w->rows[i].type == MUSE_WIDGET_ROW_FIELD;
    }
    EXT_RAM_BSS_ATTR static char prompt[MUSE_WIDGET_TITLE + MUSE_WIDGET_ROW_TITLE + 4];   /* the LVGL task's stack is internal RAM */
    if (fields > 1 && r->title[0]) {
        snprintf(prompt, sizeof(prompt), "%s", r->title);
    } else {
        snprintf(prompt, sizeof(prompt), "%s", w->title[0] ? w->title : r->title);
    }
    s_typing = at;
    const char *value = (at & 0xff) < FIELDS_MAX ? s_values[at >> 8][at & 0xff] : "";
    muse_widget_keys_open(s_face, prompt, r->sub, value, fields > 1 ? "Done" : (w->action[0] ? w->action : "Send"),
                          on_typed, NULL);
}

static void form_send(int wi);

static void on_typed(const char *t, void *ctx)
{
    (void)ctx;
    int at = s_typing;
    s_typing = NONE;
    const muse_widget_row_t *r = row_at(at);
    if (!t || !r || (at & 0xff) >= FIELDS_MAX) {
        return;
    }
    strlcpy(s_values[at >> 8][at & 0xff], t, VALUE_MAX);
    const muse_widget_t *w = &s_set->w[at >> 8];
    int fields = 0;
    for (int i = 0; i < w->count; i++) {
        fields += w->rows[i].type == MUSE_WIDGET_ROW_FIELD;
    }
    if (fields == 1) {
        form_send(at >> 8);   /* the one answer: sent as it is */
    } else {
        open_sheet(false, 0);   /* its words on the field */
    }
}

/* A form's answer: the one field's words, or a "Label: words" line each. */
static void form_send(int wi)
{
    const muse_widget_t *w = &s_set->w[wi];
    EXT_RAM_BSS_ATTR static char out[MUSE_WIDGET_SEND * 2];
    out[0] = '\0';
    int fields = 0, filled = 0;
    for (int i = 0; i < w->count && i < FIELDS_MAX; i++) {
        fields += w->rows[i].type == MUSE_WIDGET_ROW_FIELD;
        filled += w->rows[i].type == MUSE_WIDGET_ROW_FIELD && s_values[wi][i][0];
    }
    if (!filled) {
        for (int i = 0; i < w->count; i++) {
            if (w->rows[i].type == MUSE_WIDGET_ROW_FIELD) {
                open_keys(wi << 8 | i);   /* nothing typed yet: type it */
                return;
            }
        }
        return;
    }
    for (int i = 0, n = 0; i < w->count && i < FIELDS_MAX; i++) {
        const muse_widget_row_t *r = &w->rows[i];
        if (r->type != MUSE_WIDGET_ROW_FIELD || !s_values[wi][i][0]) {
            continue;
        }
        size_t o = strlen(out);
        if (fields == 1) {
            snprintf(out + o, sizeof(out) - o, "%s", s_values[wi][i]);
        } else {
            snprintf(out + o, sizeof(out) - o, "%s%s: %s", n++ ? "\n" : "", r->send[0] ? r->send : "Answer",
                     s_values[wi][i]);
        }
    }
    answer_text(out, NULL);
}

/* A multi-select's answer: "Pizza toppings: Pepperoni, Basil", or just the choices. */
static void checks_send(int wi)
{
    const muse_widget_t *w = &s_set->w[wi];
    EXT_RAM_BSS_ATTR static char out[MUSE_WIDGET_SEND * 2], shown[MUSE_WIDGET_ROW_TITLE];
    int n = snprintf(out, sizeof(out), "%s%s", w->lead, w->lead[0] ? ": " : "");
    int k = 0;
    for (int i = 0; i < w->count && n < (int)sizeof(out); i++) {
        if (s_checked[wi] & (1u << i)) {
            n += snprintf(out + n, sizeof(out) - n, "%s%s", k++ ? ", " : "", w->rows[i].send);
        }
    }
    if (!k) {
        show_toast("Pick one or more first");
        return;
    }
    muse_widget_text(shown, sizeof(shown), out + (w->lead[0] ? strlen(w->lead) + 2 : 0), false);
    answer_text(out, shown);
}

static void on_row(lv_event_t *e)
{
    int at = (int)(intptr_t)lv_event_get_user_data(e);
    const muse_widget_row_t *r = row_at(at);
    if (!r || s_state != ST_OPEN) {
        return;
    }
    switch (r->type) {
    case MUSE_WIDGET_ROW_OPTION:
    case MUSE_WIDGET_ROW_SEND:
        answer(r);
        return;
    case MUSE_WIDGET_ROW_FLIGHT:
        answer(r);   /* its button */
        return;
    case MUSE_WIDGET_ROW_CHECK: {
        s_checked[at >> 8] ^= 1u << (at & 0xff);
        /* The ring and the button, in place: no rebuild under the finger. */
        lv_obj_t *row = lv_event_get_current_target_obj(e);
        lv_obj_t *col = lv_obj_get_child(row, 0);
        lv_obj_clean(col);
        bool on = s_checked[at >> 8] & (1u << (at & 0xff));
        ring_icon(col, false, on);
        lv_obj_set_style_outline_width(row, on ? 2 : 0, 0);   /* inside its edge: nothing in it moves */
        lv_obj_set_style_outline_pad(row, -2, 0);
        lv_obj_set_style_outline_color(row, lv_color_hex(MUSE_COLOR_ACCENT), 0);
        lv_obj_t *done = lv_obj_get_user_data(row);
        if (done) {
            lv_obj_set_state(done, LV_STATE_DISABLED, !s_checked[at >> 8]);
        }
        return;
    }
    case MUSE_WIDGET_ROW_FIELD:
        open_keys(at);
        return;
    default:
        go_detail(at);   /* its card */
        return;
    }
}

/* A row's inline button (a product's "Add"): answers without the card. */
static void on_row_button(lv_event_t *e)
{
    answer(row_at((int)(intptr_t)lv_event_get_user_data(e)));
}

static void on_card_button(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    const muse_widget_row_t *r = row_at(v & 0xffff);
    if (!r) {
        return;
    }
    if (v >> 16) {
        answer_text(r->send2, NULL);
    } else {
        answer_text(r->send, NULL);
    }
}

static void on_action(lv_event_t *e)
{
    int wi = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_state != ST_OPEN || wi >= s_set->count) {
        return;
    }
    if (s_set->w[wi].kind == MUSE_WIDGET_CHECKS) {
        checks_send(wi);
    } else {
        form_send(wi);
    }
}

static void on_pin(int row)
{
    for (int wi = 0; wi < s_set->count; wi++) {
        if (s_set->w[wi].kind == MUSE_WIDGET_MAP) {
            int at = wi << 8 | row;
            if (s_detail == at) {
                return;
            }
            go_detail(at);
            return;
        }
    }
}

/* ---- Rows ---- */

/* The title over its dim lines, taking the row's width that's left. */
static void row_words(lv_obj_t *row, const muse_widget_row_t *r, const char *value)
{
    lv_obj_t *col = box(row);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    char title[MUSE_WIDGET_ROW_TITLE + 8];
    const char *t = r->title;
    const char *arrow = r->type == MUSE_WIDGET_ROW_FLIGHT ? strstr(r->title, " -> ") : NULL;
    if (arrow) {
        /* "SEA -> SFO", with the font's arrow */
        snprintf(title, sizeof(title), "%.*s  " LV_SYMBOL_RIGHT "  %s", (int)(arrow - r->title), r->title, arrow + 4);
        t = title;
    }
    if (r->type == MUSE_WIDGET_ROW_FIELD) {
        /* A field: its label small over what's typed, or its placeholder dim. */
        if (r->title[0]) {
            text(col, FONT_SMALL, MUSE_COLOR_DIM, r->title, true);
        }
        bool has = value && value[0];
        text(col, MUSE_FONT_BUTTON, has ? MUSE_COLOR_TEXT : MUSE_COLOR_DIM, has ? value : r->sub[0] ? r->sub : "Tap to type",
             true);
        return;
    }
    text(col, MUSE_FONT_BUTTON, MUSE_COLOR_TEXT, t, true);
    if (r->sub[0]) {
        text(col, MUSE_FONT_NOTE, MUSE_COLOR_DIM, r->sub, true);
    }
    if (r->extra[0] && r->type != MUSE_WIDGET_ROW_PRODUCT) {
        text(col, MUSE_FONT_NOTE, MUSE_COLOR_DIM, r->extra, true);
    }
}

/* On the right: a price (a product's old one struck under it), a site, or how far. */
static void row_meta(lv_obj_t *row, const muse_widget_row_t *r)
{
    char far[32];
    const char *meta = r->meta;
    bool dim = false;
    if (r->type == MUSE_WIDGET_ROW_LINK) {
        return;   /* its icon is the site's */
    }
    if (r->type == MUSE_WIDGET_ROW_PLACE && muse_widget_map_distance(r, far, sizeof(far))) {
        meta = far;
        dim = true;
    }
    if (!meta[0]) {
        return;
    }
    lv_obj_t *col = box(row);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    const lv_font_t *font = dim ? MUSE_FONT_NOTE : MUSE_FONT_VALUE;
    lv_obj_t *m = muse_style_label(col, font, dim ? MUSE_COLOR_DIM : MUSE_COLOR_ACCENT, meta);
    lv_obj_set_style_max_width(m, 150, 0);
    lv_obj_set_height(m, lv_font_get_line_height(font));   /* one line, ending in dots */
    lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_DOTS);
    if (r->type == MUSE_WIDGET_ROW_PRODUCT && r->extra[0]) {
        lv_obj_t *was = muse_style_label(col, FONT_SMALL, MUSE_COLOR_DIM, r->extra);
        lv_obj_set_style_text_decor(was, LV_TEXT_DECOR_STRIKETHROUGH, 0);
    }
}

/* A small filled button on the row's right (a flight's "Book", a product's "Add"), 48 px to hit. */
static void row_button(lv_obj_t *row, const char *label, int at, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(row);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 40);
    lv_obj_set_style_pad_hor(b, 16, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(MUSE_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(MUSE_COLOR_ACCENT_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    press_look(b);
    lv_obj_center(muse_style_label(b, MUSE_FONT_NOTE, MUSE_COLOR_CARD, label));
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)at);
}

/* What ends a row: a send arrow where it answers, a chevron where it opens a card. */
static void row_trailing(lv_obj_t *row, const muse_widget_row_t *r)
{
    if (r->type == MUSE_WIDGET_ROW_SEND) {
        lv_obj_t *c = box(row);
        lv_obj_set_size(c, 32, 32);
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(MUSE_COLOR_ACCENT), 0);
        lv_obj_center(muse_style_label(c, MUSE_FONT_NOTE, MUSE_COLOR_CARD, LV_SYMBOL_UP));
    } else if (r->type != MUSE_WIDGET_ROW_OPTION && r->type != MUSE_WIDGET_ROW_CHECK) {
        muse_style_label(row, MUSE_FONT_NOTE, MUSE_COLOR_DIM, LV_SYMBOL_RIGHT);
    }
}

static bool cart_button(const muse_widget_row_t *r)
{
    return r->type == MUSE_WIDGET_ROW_PRODUCT && strstr(r->send, "to my cart");
}

static lv_obj_t *add_row(lv_obj_t *col, const muse_widget_t *w, int wi, int ri, int number)
{
    const muse_widget_row_t *r = &w->rows[ri];
    int at = wi << 8 | ri;
    bool flight = r->type == MUSE_WIDGET_ROW_FLIGHT && r->button[0] && r->send[0];
    bool tap = !flight;
    lv_obj_t *row = muse_style_row(col, tap, true);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, MUSE_BUTTON_H, 0);
    lv_obj_set_style_pad_ver(row, ROW_PAD_V, 0);
    bool ticked = r->type == MUSE_WIDGET_ROW_CHECK && (s_checked[wi] & (1u << ri));
    if (r->type == MUSE_WIDGET_ROW_FIELD) {
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(MUSE_COLOR_ACCENT), 0);
        lv_obj_set_style_border_opa(row, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(MUSE_COLOR_CARD), 0);
    }
    if (ticked) {
        lv_obj_set_style_outline_width(row, 2, 0);
        lv_obj_set_style_outline_pad(row, -2, 0);
        lv_obj_set_style_outline_color(row, lv_color_hex(MUSE_COLOR_ACCENT), 0);
    }
    if (tap) {
        press_look(row);
        lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, (void *)(intptr_t)at);
    }
    leading(row, r, w->filled, ticked, number);
    row_words(row, r, r->type == MUSE_WIDGET_ROW_FIELD && ri < FIELDS_MAX ? s_values[wi][ri] : NULL);
    row_meta(row, r);
    if (flight) {
        row_button(row, r->button, at, on_row_button);
    } else if (cart_button(r)) {
        row_button(row, "Add", at, on_row_button);
    } else {
        row_trailing(row, r);
    }
    return row;
}

/* ---- Options as pills ---- */

/* "dotted_lines": dots round the pill, inside its edge, as it's drawn (pressed, smaller). */
static void on_dots(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int32_t tw = lv_obj_get_style_transform_width(o, LV_PART_MAIN);
    int32_t th = lv_obj_get_style_transform_height(o, LV_PART_MAIN);
    float x1 = a.x1 - tw + 2.5f, x2 = a.x2 + tw - 2.5f, y1 = a.y1 - th + 2.5f, y2 = a.y2 + th - 2.5f;
    float r = (y2 - y1) / 2, cy = (y1 + y2) / 2, straight = x2 - x1 - 2 * r;
    if (straight < 0) {
        straight = 0;
    }
    float per = 2 * straight + 2 * (float)M_PI * r;
    int n = (int)(per / DOT_GAP);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(MUSE_COLOR_ACCENT);
    d.bg_opa = LV_OPA_COVER;
    d.radius = LV_RADIUS_CIRCLE;
    for (int i = 0; i < n; i++) {
        float s = per * i / n, x, y;
        if (s < straight) {   /* along the top, rightwards */
            x = x1 + r + s;
            y = y1;
        } else if ((s -= straight) < (float)M_PI * r) {   /* round the right end */
            float t = s / r - (float)M_PI / 2;
            x = x2 - r + r * cosf(t);
            y = cy + r * sinf(t);
        } else if ((s -= (float)M_PI * r) < straight) {   /* along the bottom, leftwards */
            x = x2 - r - s;
            y = y2;
        } else {   /* round the left end */
            float t = (s - straight) / r + (float)M_PI / 2;
            x = x1 + r + r * cosf(t);
            y = cy + r * sinf(t);
        }
        lv_area_t dot = { (int32_t)lroundf(x) - DOT_PX / 2, (int32_t)lroundf(y) - DOT_PX / 2, 0, 0 };
        dot.x2 = dot.x1 + DOT_PX - 1;
        dot.y2 = dot.y1 + DOT_PX - 1;
        lv_draw_rect(lv_event_get_layer(e), &d, &dot);
    }
}

/* The column's width for rows: the sheet's, less its border and padding. */
static int inner_w(void)
{
    return s_w - 2 * SHEET_SIDE - 2 * MUSE_CARD_BORDER - 2 * MUSE_CARD_PAD;
}

/* One to three options that each fit on one line across: pills. More, or a long one: rows. */
static bool pills_fit(const muse_widget_t *w)
{
    if (w->count > PILLS_MAX) {
        return false;
    }
    int pill_w = (inner_w() - SCROLLBAR_ROOM - (w->count - 1) * MUSE_CARD_GAP) / w->count;   /* room either way */
    for (int i = 0; i < w->count; i++) {
        lv_point_t size;
        lv_text_get_size(&size, w->rows[i].title, MUSE_FONT_BUTTON, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x + 2 * PILL_PAD > pill_w) {
            return false;
        }
    }
    return true;
}

static void add_pills(lv_obj_t *col, const muse_widget_t *w, int wi)
{
    lv_obj_t *line = box(col);
    lv_obj_set_size(line, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(line, MUSE_CARD_GAP, 0);
    for (int i = 0; i < w->count; i++) {
        lv_obj_t *b = lv_button_create(line);
        lv_obj_remove_style_all(b);
        lv_obj_set_height(b, MUSE_BUTTON_H);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(w->filled ? MUSE_COLOR_ACCENT : MUSE_COLOR_CARD_PRESSED), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(w->filled ? MUSE_COLOR_ACCENT_PRESSED : MUSE_COLOR_RAISED_PRESSED),
                                  LV_STATE_PRESSED);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        press_look(b);
        if (!w->filled) {
            lv_obj_add_event_cb(b, on_dots, LV_EVENT_DRAW_MAIN_END, NULL);
        }
        lv_obj_center(muse_style_label(b, MUSE_FONT_BUTTON, w->filled ? MUSE_COLOR_CARD : MUSE_COLOR_TEXT,
                                       w->rows[i].title));
        lv_obj_add_event_cb(b, on_row, LV_EVENT_CLICKED, (void *)(intptr_t)(wi << 8 | i));
    }
}

/* ---- A card: its words, to see here, the rest in the app ---- */

static void add_card(lv_obj_t *col, const muse_widget_t *w)
{
    lv_obj_t *panel = muse_style_row(col, false, true);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(panel, MUSE_BUTTON_H, 0);
    lv_obj_set_style_pad_ver(panel, 14, 0);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *icon = box(panel);
    lv_obj_set_size(icon, ICON_W, 24);
    lv_obj_center(muse_style_label(icon, MUSE_FONT_BUTTON, MUSE_COLOR_ACCENT, kind_symbol(w->kind)));
    lv_obj_t *words = box(panel);
    lv_obj_set_height(words, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(words, 1);
    text(words, MUSE_FONT_NOTE, MUSE_COLOR_TEXT, w->text[0] ? w->text : w->title, true);
    lv_obj_t *note = text(col, FONT_SMALL, MUSE_COLOR_DIM, "There's more of this in the Muse app", true);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
}

/* ---- A map widget: the map over its places ---- */

static void add_map(lv_obj_t *col, const muse_widget_t *w, int wi, int selected, int height)
{
    uint8_t order[MUSE_WIDGET_ROWS_MAX];
    int n = muse_widget_map_order(w, order);
    lv_obj_update_layout(col);
    int width = lv_obj_get_content_width(col);
    width = width > 0 ? width : inner_w() - SCROLLBAR_ROOM;
    lv_obj_t *map = muse_widget_map_build(col, w, order, n, width, height, selected, on_pin);
    if (map && muse_widget_map_rough()) {
        lv_obj_t *note = text(col, FONT_SMALL, MUSE_COLOR_DIM, "Distances from your approximate location", true);
        lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    }
    if (selected >= 0) {
        return;   /* a place's card: the map over it */
    }
    for (int i = 0; i < n; i++) {
        add_row(col, w, wi, order[i], map ? i + 1 : 0);
    }
}

/* ---- A form, a multi-select: their rows, then their button ---- */

static void add_form(lv_obj_t *col, const muse_widget_t *w, int wi)
{
    lv_obj_t *done = NULL;
    lv_obj_t *rows[MUSE_WIDGET_ROWS_MAX];
    for (int i = 0; i < w->count; i++) {
        rows[i] = add_row(col, w, wi, i, 0);
    }
    bool can = w->kind == MUSE_WIDGET_CHECKS ? s_checked[wi] != 0 : true;
    const char *label = w->action[0] ? w->action : w->kind == MUSE_WIDGET_CHECKS ? "Done" : "Send";
    done = pill(col, label, true, on_action, (void *)(intptr_t)wi);
    lv_obj_set_style_margin_top(done, 4, 0);
    lv_obj_set_state(done, LV_STATE_DISABLED, !can);
    for (int i = 0; i < w->count; i++) {
        lv_obj_set_user_data(rows[i], done);   /* a tick fills it in */
    }
}

/* ---- A row's card ---- */

static void buttons(lv_obj_t *col, const muse_widget_row_t *r, int at)
{
    const char *first = r->button[0] ? r->button : "Tell me more";
    bool two = r->send2[0] && r->button2[0];
    lv_obj_t *line = box(col);
    lv_obj_set_size(line, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(line, MUSE_CARD_GAP, 0);
    lv_obj_set_style_margin_top(line, 4, 0);
    if (two) {
        lv_obj_t *b2 = pill(line, r->button2, false, on_card_button, (void *)(intptr_t)(1 << 16 | at));
        lv_obj_set_size(b2, LV_SIZE_CONTENT, CARD_BUTTON_H);
        lv_obj_set_flex_grow(b2, 1);
    }
    if (r->send[0]) {
        lv_obj_t *b1 = pill(line, first, true, on_card_button, (void *)(intptr_t)at);
        lv_obj_set_size(b1, LV_SIZE_CONTENT, CARD_BUTTON_H);
        lv_obj_set_flex_grow(b1, 1);
    }
}

/* Its picture (on white, as shops show them; a placeholder till it comes) beside its price and shop. */
static void add_product(lv_obj_t *col, const muse_widget_row_t *r)
{
    lv_obj_t *line = box(col);
    lv_obj_set_size(line, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(line, 14, 0);
    lv_obj_t *frame = box(line);
    lv_obj_set_size(frame, PIC_D, PIC_D);
    lv_obj_set_style_radius(frame, MUSE_ROW_RADIUS, 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    const lv_image_dsc_t *pic = muse_widget_pic(r->image);
    lv_obj_set_style_bg_color(frame, lv_color_hex(pic ? 0xffffff : MUSE_COLOR_CARD_PRESSED), 0);
    if (pic) {
        lv_obj_t *img = lv_image_create(frame);
        lv_image_set_src(img, pic);
        lv_obj_center(img);
    } else {
        lv_obj_center(muse_style_label(frame, &lv_font_montserrat_28, MUSE_COLOR_ACCENT, r->image[0] ? LV_SYMBOL_IMAGE : "$"));
        if (r->image[0]) {
            muse_widget_pic_want(r->image, PIC_D, PIC_D);
            lv_obj_set_user_data(frame, (void *)r->image);   /* pic_tick swaps it in */
            lv_obj_add_flag(frame, LV_OBJ_FLAG_USER_1);
        }
    }
    lv_obj_t *words = box(line);
    lv_obj_set_height(words, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(words, 1);
    lv_obj_set_flex_flow(words, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(words, 4, 0);
    if (r->meta[0]) {
        muse_style_label(words, &lv_font_montserrat_28, MUSE_COLOR_ACCENT, r->meta);
    }
    if (r->extra[0]) {
        lv_obj_t *was = muse_style_label(words, MUSE_FONT_NOTE, MUSE_COLOR_DIM, r->extra);
        lv_obj_set_style_text_decor(was, LV_TEXT_DECOR_STRIKETHROUGH, 0);
    }
    if (r->sub[0]) {
        text(words, MUSE_FONT_NOTE, MUSE_COLOR_TEXT, r->sub, true);
    }
}

static void add_detail(lv_obj_t *col, int at)
{
    const muse_widget_row_t *r = row_at(at);
    if (!r) {
        return;
    }
    const muse_widget_t *w = &s_set->w[(at >> 8) & 0xff];
    if (!w->count) {
        return;
    }
    lv_obj_set_style_pad_row(col, 6, 0);
    if (r->type == MUSE_WIDGET_ROW_PLACE && r->pos) {
        add_map(col, w, at >> 8, at & 0xff, MAP_CARD_H);
    }
    if (r->type == MUSE_WIDGET_ROW_PRODUCT) {
        add_product(col, r);
        buttons(col, r, at);
        return;
    }
    if (r->type == MUSE_WIDGET_ROW_LINK) {
        lv_obj_t *head = box(col);
        lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(head, 12, 0);
        site_icon(head, r->meta, 44);
        muse_style_label(head, MUSE_FONT_BUTTON, MUSE_COLOR_ACCENT, r->meta);
    }
    if (r->sub[0]) {
        text(col, MUSE_FONT_NOTE, r->type == MUSE_WIDGET_ROW_LINK ? MUSE_COLOR_DIM : MUSE_COLOR_TEXT, r->sub, true);
    }
    if (r->extra[0]) {
        text(col, MUSE_FONT_NOTE, MUSE_COLOR_DIM, r->extra, true);
    }
    char far[32];
    if (r->type == MUSE_WIDGET_ROW_PLACE && muse_widget_map_distance(r, far, sizeof(far))) {
        char t[48];
        snprintf(t, sizeof(t), LV_SYMBOL_GPS "  %s from you", far);
        text(col, MUSE_FONT_NOTE, MUSE_COLOR_ACCENT, t, true);
    }
    if (r->type == MUSE_WIDGET_ROW_LINK && r->url[0]) {
        char url[MUSE_WIDGET_URL];
        muse_widget_text(url, sizeof(url), r->url, false);
        text(col, FONT_SMALL, MUSE_COLOR_DIM, url, true);
    }
    buttons(col, r, at);
}

/* ---- The browser's history ---- */

static void ago(char *dst, size_t cap, int64_t at_ms)
{
    int s = (int)((now_ms() - at_ms) / 1000);
    if (s < 5) {
        snprintf(dst, cap, "now");
    } else if (s < 60) {
        snprintf(dst, cap, "%ds", s);
    } else if (s < 3600) {
        snprintf(dst, cap, "%dm", s / 60);
    } else {
        snprintf(dst, cap, "%dh", s / 3600);
    }
}

/* "Central Library | The Seattle Public Library" -> "Central Library": the site's name is under it. */
static void page_title(char *dst, size_t cap, const char *t)
{
    muse_widget_text(dst, cap, t, false);
    char *bar = strstr(dst, " | ");
    if (!bar) {
        bar = strstr(dst, " - ");
        char *more;
        while (bar && (more = strstr(bar + 3, " - "))) {
            bar = more;   /* the last */
        }
    }
    if (bar && bar > dst + 3 && strlen(bar) < 40) {
        *bar = '\0';
    }
}

static void breathe(void *o, int32_t v)
{
    lv_obj_set_style_shadow_opa(o, (lv_opa_t)(v * LV_OPA_80 / 255), 0);
    lv_obj_set_style_shadow_spread(o, (int32_t)(v * 4 / 255), 0);
}

/* Where it's up to, its site's icon breathing a glow. */
static void live_dot(lv_obj_t *icon)
{
    lv_obj_set_style_shadow_color(icon, lv_color_hex(MUSE_COLOR_ACCENT), 0);
    lv_obj_set_style_shadow_width(icon, 14, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, icon);
    lv_anim_set_exec_cb(&a, breathe);
    lv_anim_set_values(&a, 40, 255);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_reverse_duration(&a, 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void add_browse(lv_obj_t *col)
{
    const muse_browse_t *b = s_browse;
    lv_obj_set_style_pad_row(col, 0, 0);
    s_browse_shown = b->count;
    for (int i = 0; i < b->count; i++) {
        const muse_browse_step_t *s = &b->steps[i];
        bool now = i == b->count - 1 && muse_browse_running(b);
        lv_obj_t *row = box(col);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_column(row, 12, 0);
        /* The icon, and the timeline's line down from it to the next. */
        lv_obj_t *rail = box(row);
        lv_obj_set_size(rail, 36, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(rail, 64, 0);
        lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *icon = site_icon(rail, s->site, 36);
        if (now) {
            live_dot(icon);
        }
        if (i < b->count - 1) {
            lv_obj_t *line = box(rail);
            lv_obj_set_size(line, 2, 28);
            lv_obj_set_flex_grow(line, 1);
            lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(line, lv_color_hex(COLOR_LINE), 0);
        }
        lv_obj_t *words = box(row);
        lv_obj_set_height(words, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(words, 1);
        lv_obj_set_flex_flow(words, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(words, 2, 0);
        lv_obj_set_style_pad_bottom(words, 12, 0);
        EXT_RAM_BSS_ATTR static char title[MUSE_BROWSE_TITLE], what[MUSE_BROWSE_WHAT + MUSE_BROWSE_SITE + 8];
        page_title(title, sizeof(title), s->title[0] ? s->title : s->what);
        text(words, MUSE_FONT_NOTE, now ? MUSE_COLOR_TEXT : 0xcfc9e8, title, true);
        if (s->site[0] && s->title[0]) {
            snprintf(what, sizeof(what), "%s  %s", s->what, s->site);
        } else {
            snprintf(what, sizeof(what), "%s", s->title[0] ? s->what : s->site);
        }
        if (what[0]) {
            text(words, FONT_SMALL, now ? MUSE_COLOR_ACCENT : MUSE_COLOR_DIM, what, true);
        }
        char t[12];
        ago(t, sizeof(t), s->at_ms);
        s_browse_times[i] = muse_style_label(row, FONT_SMALL, MUSE_COLOR_DIM, t);
    }
    if (!b->count) {
        text(col, MUSE_FONT_NOTE, MUSE_COLOR_DIM, "Starting the browser...", true);
    }
}

/* ---- The sheet ---- */

static void delete_sheet(void)
{
    if (s_sheet) {
        lv_anim_delete(s_sheet, NULL);
        lv_obj_delete_async(s_sheet);   /* we may be in one of its own events */
        s_sheet = s_scroll = s_toast = s_fade[0] = s_fade[1] = NULL;
        memset(s_browse_times, 0, sizeof(s_browse_times));
    }
}

static void slide(int32_t from, int32_t to, uint32_t ms, lv_anim_path_cb_t path, lv_anim_completed_cb_t done)
{
    anim(s_sheet, translate_y, from, to, ms, path, done);
}

static void gone(lv_anim_t *a)
{
    (void)a;
    delete_sheet();
}

/* Down and off the screen; then (`tuck`) the chip brings it back, or nothing does. */
static void put_away(bool tuck)
{
    if (s_view == VIEW_WIDGETS) {
        s_state = tuck ? ST_AWAY : ST_NONE;
    } else {
        s_browse_dismissed = !tuck;
    }
    if (!s_sheet) {
        return;
    }
    ESP_LOGI(TAG, "sheet %s", tuck ? "put away" : "done with");
    s_detail = NONE;
    slide(lv_obj_get_style_translate_y(s_sheet, 0), lv_obj_get_height(s_sheet) + SHEET_BOTTOM, AWAY_MS,
          lv_anim_path_ease_in, gone);
    if (!tuck) {
        anim(s_sheet, fade_opa, LV_OPA_COVER, LV_OPA_TRANSP, AWAY_MS, lv_anim_path_ease_in, NULL);
    }
}

/* Done with it: the widgets (or the history) go for good. */
static void dismiss(void)
{
    if (s_view == VIEW_WIDGETS) {
        muse_widget_clear();
        memset(s_checked, 0, sizeof(s_checked));
        memset(s_values, 0, sizeof(s_values));
    }
    put_away(false);
}

static bool sheet_live(void)
{
    return s_sheet && (s_view == VIEW_BROWSE || s_state == ST_OPEN);
}

static void on_close(lv_event_t *e)
{
    (void)e;
    if (sheet_live()) {
        dismiss();
    }
}

static void on_back(lv_event_t *e)
{
    (void)e;
    if (sheet_live() && s_detail != NONE) {
        go_detail(NONE);
    }
}

/* Swiped down where nothing scrolls: tucked away. Right, on a card: back. */
static void on_gesture(lv_event_t *e)
{
    (void)e;
    if (!sheet_live()) {
        return;
    }
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_BOTTOM) {
        put_away(true);
    } else if (dir == LV_DIR_RIGHT && s_detail != NONE) {
        go_detail(NONE);
    }
}

/* The header follows a finger down; let go far enough down, the sheet tucks away, else it springs back. */
static void on_header_drag(lv_event_t *e)
{
    if (!sheet_live()) {
        return;
    }
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_drag = 0;
        lv_anim_delete(s_sheet, translate_y);
    } else if (code == LV_EVENT_PRESSING) {
        lv_point_t v;
        lv_indev_get_vect(lv_indev_active(), &v);
        s_drag = s_drag + v.y > 0 ? s_drag + v.y : 0;
        lv_obj_set_style_translate_y(s_sheet, s_drag, 0);
    } else if (s_drag > DRAG_AWAY_PX) {
        put_away(true);
    } else if (s_drag > 0) {
        slide(s_drag, 0, 200, lv_anim_path_ease_out, NULL);
    }
}

/* The column's edges fade while there's more that way. */
static void on_scroll(lv_event_t *e)
{
    (void)e;
    if (!s_scroll || !s_fade[0]) {
        return;
    }
    lv_obj_set_flag(s_fade[0], LV_OBJ_FLAG_HIDDEN, lv_obj_get_scroll_top(s_scroll) <= 1);
    lv_obj_set_flag(s_fade[1], LV_OBJ_FLAG_HIDDEN, lv_obj_get_scroll_bottom(s_scroll) <= 1);
}

static lv_obj_t *edge_fade(bool top)
{
    lv_obj_t *f = box(s_sheet);
    lv_obj_add_flag(f, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(f, lv_color_hex(MUSE_COLOR_CARD), 0);
    lv_obj_set_style_bg_grad_color(f, lv_color_hex(MUSE_COLOR_CARD), 0);
    lv_obj_set_style_bg_grad_dir(f, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_opa(f, top ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_opa(f, top ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    return f;
}

static const char *widget_title(const muse_widget_t *w)
{
    return w->title[0] ? w->title : w->kind == MUSE_WIDGET_OPTIONS ? "Choose one" : "From Muse";
}

/* Grabber, then (on a card, a back arrow and) the title and its line beside the close button. */
static void build_header(const char *title, const char *line, bool back)
{
    lv_obj_t *head = box(s_sheet);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(head, 6, 0);
    lv_obj_add_flag(head, LV_OBJ_FLAG_CLICKABLE);   /* dragged down, it takes the sheet */
    static const lv_event_code_t DRAG[] = { LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED,
                                            LV_EVENT_PRESS_LOST };
    for (size_t i = 0; i < sizeof(DRAG) / sizeof(DRAG[0]); i++) {
        lv_obj_add_event_cb(head, on_header_drag, DRAG[i], NULL);
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
    if (back) {
        round_button(row, CLOSE_D, LV_SYMBOL_LEFT, on_back);
    }
    lv_obj_t *words = box(row);
    lv_obj_set_height(words, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(words, 1);
    lv_obj_set_flex_flow(words, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(words, 2, 0);
    text(words, MUSE_FONT_CARD_TITLE, MUSE_COLOR_TEXT, title, true);
    if (line && line[0]) {
        text(words, MUSE_FONT_NOTE, MUSE_COLOR_DIM, line, true);
    }
    round_button(row, CLOSE_D, LV_SYMBOL_CLOSE, on_close);
}

static void browse_header(char *line, size_t cap)
{
    const muse_browse_t *b = s_browse;
    const char *site = b->count ? b->steps[b->count - 1].site : "";
    int pages = 0;
    for (int i = 0; i < b->count; i++) {
        pages += b->steps[i].url[0] != 0;
    }
    if (muse_browse_running(b)) {
        snprintf(line, cap, site[0] ? "Browsing %s..." : "Starting the browser...", site);
    } else if (b->done && b->failed) {
        snprintf(line, cap, "Stopped after %d page%s", pages, pages == 1 ? "" : "s");
    } else {
        snprintf(line, cap, "Done: %d page%s looked at", pages, pages == 1 ? "" : "s");
    }
}

static void build_column(void)
{
    s_scroll = box(s_sheet);
    lv_obj_set_size(s_scroll, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_scroll, MUSE_CARD_GAP, 0);
    if (s_view == VIEW_BROWSE) {
        add_browse(s_scroll);
        return;
    }
    if (s_detail != NONE) {
        add_detail(s_scroll, s_detail);
        return;
    }
    for (int wi = 0; wi < s_set->count; wi++) {
        const muse_widget_t *w = &s_set->w[wi];
        if (wi > 0) {
            /* A second widget: its title as a section's, a gap over it. */
            lv_obj_t *t = text(s_scroll, MUSE_FONT_NOTE, MUSE_COLOR_ACCENT, widget_title(w), true);
            lv_obj_set_style_pad_top(t, 8, 0);
        }
        switch (w->kind) {
        case MUSE_WIDGET_CARD: add_card(s_scroll, w); break;
        case MUSE_WIDGET_MAP: add_map(s_scroll, w, wi, NONE, MAP_H); break;
        case MUSE_WIDGET_FORM:
        case MUSE_WIDGET_CHECKS: add_form(s_scroll, w, wi); break;
        case MUSE_WIDGET_OPTIONS:
            if (pills_fit(w)) {
                add_pills(s_scroll, w, wi);
                break;
            }
            __attribute__((fallthrough));   /* too many, or too long: rows */
        default:
            for (int ri = 0; ri < w->count; ri++) {
                add_row(s_scroll, w, wi, ri, 0);
            }
            break;
        }
    }
}

/*
 * Builds the sheet for what it's of, its column as tall as what's on it up
 * to the room there is, then scrolling; slides it up if `slide_in`, or its
 * column in from the right (dir 1, a card) or the left (-1, back).
 */
static void open_sheet(bool slide_in, int dir)
{
    int32_t was_y = s_sheet ? lv_obj_get_style_translate_y(s_sheet, 0) : 0;
    bool keep_bottom = s_view == VIEW_BROWSE && s_scroll && lv_obj_get_scroll_bottom(s_scroll) <= 4;
    if (s_sheet) {
        lv_anim_delete(s_sheet, NULL);
        lv_obj_delete(s_sheet);   /* not async: the new one takes its place this frame */
        s_sheet = s_scroll = s_toast = s_fade[0] = s_fade[1] = NULL;
        memset(s_browse_times, 0, sizeof(s_browse_times));
    }
    s_sheet = lv_obj_create(s_face);
    lv_obj_remove_style_all(s_sheet);
    muse_style_card(s_sheet);
    lv_obj_set_style_pad_top(s_sheet, SHEET_PAD_TOP, 0);
    lv_obj_set_width(s_sheet, s_w - 2 * SHEET_SIDE);
    lv_obj_set_height(s_sheet, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_CLICKABLE);   /* taps on it stay on it, not on Muse */
    lv_obj_add_event_cb(s_sheet, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_align(s_sheet, LV_ALIGN_BOTTOM_MID, 0, -SHEET_BOTTOM);
    lv_obj_set_style_translate_y(s_sheet, slide_in ? 0 : was_y, 0);

    char line[64] = "";
    if (s_view == VIEW_BROWSE) {
        browse_header(line, sizeof(line));
        build_header("What Muse did", line, false);
    } else if (s_detail != NONE && row_at(s_detail)) {
        build_header(row_at(s_detail)->title, NULL, true);   /* the card's thing, as a page's title */
    } else {
        const muse_widget_t *w = &s_set->w[0];
        bool sub = w->text[0] && w->kind != MUSE_WIDGET_CARD;
        build_header(widget_title(w), sub ? w->text : NULL, false);
    }
    build_column();
    s_fade[0] = edge_fade(true);
    s_fade[1] = edge_fade(false);

    s_toast = box(s_sheet);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(s_toast, 8, 0);
    lv_obj_set_style_pad_hor(s_toast, 16, 0);
    lv_obj_set_style_radius(s_toast, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(MUSE_COLOR_RAISED_PRESSED), 0);
    lv_obj_set_style_opa(s_toast, LV_OPA_TRANSP, 0);
    muse_style_label(s_toast, MUSE_FONT_NOTE, MUSE_COLOR_TEXT, "");
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* Room for the column: from the highest the sheet goes, less the header. */
    lv_obj_update_layout(s_sheet);
    int room = (s_h - SHEET_BOTTOM - s_top) - lv_obj_get_style_pad_top(s_sheet, 0)
               - lv_obj_get_style_pad_bottom(s_sheet, 0) - 2 * MUSE_CARD_BORDER
               - lv_obj_get_height(lv_obj_get_child(s_sheet, 0)) - lv_obj_get_style_pad_row(s_sheet, 0);
    bool scrolls = lv_obj_get_height(s_scroll) > room;
    if (scrolls) {
        lv_obj_set_height(s_scroll, room);
        lv_obj_set_style_pad_right(s_scroll, SCROLLBAR_ROOM, 0);
        lv_obj_set_style_pad_bottom(s_scroll, 4, 0);
        muse_style_scroll_column(s_scroll, 4, 0, 4, 4);
        lv_obj_add_event_cb(s_scroll, on_scroll, LV_EVENT_SCROLL, NULL);
        lv_obj_update_layout(s_sheet);
        int x = lv_obj_get_x(s_scroll), y = lv_obj_get_y(s_scroll), w = lv_obj_get_width(s_scroll) - SCROLLBAR_ROOM;
        lv_obj_set_size(s_fade[0], w, FADE_H);
        lv_obj_set_pos(s_fade[0], x, y);
        lv_obj_set_size(s_fade[1], w, FADE_H);
        lv_obj_set_pos(s_fade[1], x, y + room - FADE_H);
        if (keep_bottom || (s_view == VIEW_BROWSE && slide_in)) {
            lv_obj_scroll_to_y(s_scroll, LV_COORD_MAX, LV_ANIM_OFF);   /* where it's up to */
        }
        on_scroll(NULL);
    }
    if (s_view == VIEW_WIDGETS) {
        s_state = ST_OPEN;
    }
    ESP_LOGI(TAG, "sheet up: %s%s", s_view == VIEW_BROWSE ? "what Muse did" : s_detail != NONE ? "a card" : s_set->w[0].name,
             scrolls ? ", scrolling" : "");
    if (slide_in) {
        lv_obj_update_layout(s_sheet);
        slide(lv_obj_get_height(s_sheet) + SHEET_BOTTOM, 0, OPEN_MS, lv_anim_path_overshoot, NULL);
    } else if (dir) {
        anim(s_scroll, translate_x, dir * 48, 0, PAGE_MS, lv_anim_path_ease_out, NULL);
        anim(s_scroll, fade_opa, LV_OPA_TRANSP, LV_OPA_COVER, PAGE_MS, lv_anim_path_ease_out, NULL);
    }
}

/* ---- Sent: folding into a chip with the answer ---- */

typedef struct {
    int32_t w0, h0, w1, h1;
} fold_t;
EXT_RAM_BSS_ATTR static fold_t s_fold;

static void fold_step(void *o, int32_t t)
{
    lv_obj_set_size(o, s_fold.w0 + (s_fold.w1 - s_fold.w0) * t / 256, s_fold.h0 + (s_fold.h1 - s_fold.h0) * t / 256);
}

static void sent_gone(lv_anim_t *a)
{
    (void)a;
    delete_sheet();
    s_state = ST_NONE;
    s_detail = NONE;
}

static void fold_done(lv_anim_t *a)
{
    (void)a;
    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, s_sheet);
    lv_anim_set_exec_cb(&b, translate_y);
    lv_anim_set_values(&b, 0, s_fold.h1 + SHEET_BOTTOM);
    lv_anim_set_delay(&b, SENT_HOLD_MS);
    lv_anim_set_duration(&b, AWAY_MS);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&b, sent_gone);
    lv_anim_start(&b);
}

/* The sheet's rows give way to a pill with a tick and the answer; it shrinks round them, then goes. */
static void fold(void *unused)
{
    (void)unused;
    if (!s_sheet || s_state != ST_SENT) {
        return;
    }
    s_fold.w0 = lv_obj_get_width(s_sheet);
    s_fold.h0 = lv_obj_get_height(s_sheet);
    lv_anim_delete(s_sheet, NULL);
    lv_obj_clean(s_sheet);
    s_scroll = s_toast = s_fade[0] = s_fade[1] = NULL;
    lv_obj_set_style_opa(s_sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_sheet, 0, 0);
    lv_obj_set_style_pad_hor(s_sheet, 20, 0);
    lv_obj_set_style_radius(s_sheet, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_translate_y(s_sheet, 0, 0);
    lv_obj_set_flex_flow(s_sheet, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_sheet, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_sheet, MUSE_ROW_GAP_X, 0);
    muse_style_label(s_sheet, MUSE_FONT_BUTTON, MUSE_COLOR_ACCENT, LV_SYMBOL_OK);
    lv_obj_t *l = muse_style_label(s_sheet, MUSE_FONT_BUTTON, MUSE_COLOR_TEXT, s_sent);
    int max_text = s_fold.w0 - 40 - 24 - MUSE_ROW_GAP_X;
    lv_obj_set_style_max_width(l, max_text, 0);
    lv_obj_set_height(l, lv_font_get_line_height(MUSE_FONT_BUTTON));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_point_t size;
    lv_text_get_size(&size, s_sent, MUSE_FONT_BUTTON, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    s_fold.w1 = LV_MIN(s_fold.w0, (size.x < max_text ? size.x : max_text) + 40 + 24 + MUSE_ROW_GAP_X);
    s_fold.h1 = MUSE_BUTTON_H;
    lv_obj_set_size(s_sheet, s_fold.w0, s_fold.h0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_sheet);
    lv_anim_set_exec_cb(&a, fold_step);
    lv_anim_set_values(&a, 0, 256);
    lv_anim_set_duration(&a, FOLD_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, fold_done);
    lv_anim_start(&a);
}

/* ---- The chip that brings it back ---- */

typedef enum { CHIP_NONE, CHIP_WIDGETS, CHIP_BROWSE } chip_for_t;
EXT_RAM_BSS_ATTR static chip_for_t s_chip_for;

/* A puff where the chip was: a ring of motes flying out and fading. */
static void puff_step(void *o, int32_t v)
{
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *d = lv_obj_get_child(o, (int32_t)i);
        float a = (float)i * 6.2832f / (float)n + 0.3f;
        float r = 6.0f + 30.0f * (float)v / 1000.0f;
        lv_obj_set_pos(d, 40 + (int32_t)(r * cosf(a)) - 4, 40 + (int32_t)(r * sinf(a) * 0.7f) - 4);
        lv_obj_set_style_opa(d, (lv_opa_t)(255 - v * 255 / 1000), 0);
        int s = 9 - (int)(v * 5 / 1000);
        lv_obj_set_size(d, s, s);
    }
}

static void puff_done(lv_anim_t *a)
{
    lv_obj_delete(a->var);
}

static void puff(int x, int y)
{
    lv_obj_t *p = box(s_face);
    lv_obj_set_size(p, 80, 80);
    lv_obj_set_pos(p, x - 40, y - 40);
    for (int i = 0; i < 8; i++) {
        lv_obj_t *d = box(p);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(i % 2 ? MUSE_COLOR_ACCENT : COLOR_CHIP_TEXT), 0);
    }
    puff_step(p, 0);
    anim(p, puff_step, 0, 1000, 380, lv_anim_path_ease_out, puff_done);
}

static void chip_hide(void)
{
    lv_obj_add_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_translate_x(s_chip, 0, 0);
    lv_obj_set_style_opa(s_chip, LV_OPA_COVER, 0);
}

/* Done with what the chip's of: it goes in a puff. */
static void chip_done(void)
{
    lv_area_t a;
    lv_obj_get_coords(s_chip, &a);
    chip_hide();
    puff((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    if (s_chip_for == CHIP_WIDGETS) {
        ESP_LOGI(TAG, "widgets done with, from the chip");
        muse_widget_clear();
        s_state = ST_NONE;
    } else {
        s_browse_dismissed = true;
    }
    s_chip_for = CHIP_NONE;
}

static void flung(lv_anim_t *a)
{
    (void)a;
    chip_done();
}

static void on_chip_x(lv_event_t *e)
{
    (void)e;
    if (!lv_obj_has_flag(s_chip, LV_OBJ_FLAG_HIDDEN)) {
        chip_done();
    }
}

/* Dragged sideways it follows; let go far enough out, it's flung away, else back it springs. A tap brings the sheet back. */
static void on_chip(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_chip_dx = 0;
        s_chip_dragged = false;
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        lv_point_t v;
        lv_indev_get_vect(lv_indev_active(), &v);
        s_chip_dx += v.x;
        s_chip_dragged = s_chip_dragged || LV_ABS(s_chip_dx) > 10;
        if (s_chip_dragged) {
            lv_obj_set_style_translate_x(s_chip, s_chip_dx, 0);
            lv_obj_set_style_opa(s_chip, (lv_opa_t)LV_MAX(80, 255 - LV_ABS(s_chip_dx) * 2), 0);
        }
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s_chip_dragged) {
            return;
        }
        if (LV_ABS(s_chip_dx) > CHIP_FLING_PX) {
            int to = s_chip_dx > 0 ? s_w : -s_w;
            anim(s_chip, translate_x, s_chip_dx, to / 2, 160, lv_anim_path_ease_in, flung);
        } else {
            anim(s_chip, translate_x, s_chip_dx, 0, 220, lv_anim_path_overshoot, NULL);
            lv_obj_set_style_opa(s_chip, LV_OPA_COVER, 0);
        }
        return;
    }
    if (code == LV_EVENT_CLICKED && !s_chip_dragged) {
        if (s_chip_for == CHIP_WIDGETS && s_state == ST_AWAY && s_set->count) {
            chip_hide();
            s_view = VIEW_WIDGETS;
            s_detail = NONE;
            open_sheet(true, 0);
        } else if (s_chip_for == CHIP_BROWSE) {
            chip_hide();
            s_view = VIEW_BROWSE;
            open_sheet(true, 0);
        }
    }
}

/* What the chip says: the options, the first widget's title, or where Muse browsed. */
static void chip_text(chip_for_t what)
{
    char t[MUSE_WIDGET_TITLE] = "";
    const char *icon = LV_SYMBOL_LIST;
    if (what == CHIP_WIDGETS) {
        const muse_widget_t *w = &s_set->w[0];
        if (w->kind == MUSE_WIDGET_OPTIONS) {
            for (int i = 0; i < w->count && strlen(t) + 3 < sizeof(t); i++) {
                strlcat(t, i ? ", " : "", sizeof(t));
                strlcat(t, w->rows[i].title, sizeof(t));
            }
        } else {
            strlcpy(t, widget_title(w), sizeof(t));
        }
        icon = kind_symbol(w->kind);
    } else {
        const muse_browse_t *b = s_browse;
        const char *site = "";
        for (int i = b->count - 1; i >= 0 && !site[0]; i--) {
            site = b->steps[i].site;
        }
        snprintf(t, sizeof(t), site[0] ? "Browsed %s" : "What Muse did", site);
        icon = LV_SYMBOL_EYE_OPEN;
    }
    lv_label_set_text(s_chip_icon, icon);
    lv_label_set_text(s_chip_lbl, t);
    lv_point_t size;
    lv_text_get_size(&size, t, &lv_font_unscii_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_obj_set_width(s_chip_lbl, LV_MIN(size.x, CHIP_TEXT_W));
}

static void chip_tick(muse_mode_t mode, float now, bool may_open)
{
    char c[4] = "";
    if (muse_state_caption(c, sizeof(c), &s_caption_ver) && c[0]) {
        s_caption_at = now;
    }
    bool calm = mode == MUSE_MODE_IDLE && may_open && !s_sheet && !muse_widget_keys_up()
                && (!s_caption_at || now - s_caption_at > CHIP_CAPTION_S);
    chip_for_t want = CHIP_NONE;
    if (calm && s_state == ST_AWAY && s_set->count) {
        want = CHIP_WIDGETS;
    } else if (calm && s_state == ST_NONE && s_browse->count && !s_browse_dismissed) {
        want = CHIP_BROWSE;
    }
    bool shown = !lv_obj_has_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
    if (want == s_chip_for && shown == (want != CHIP_NONE)) {
        return;
    }
    if (lv_anim_get(s_chip, translate_x)) {
        return;   /* being flung */
    }
    s_chip_for = want;
    if (want == CHIP_NONE) {
        chip_hide();
        return;
    }
    chip_text(want);
    lv_obj_remove_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_translate_x(s_chip, 0, 0);
    lv_obj_move_foreground(s_chip);
    anim(s_chip, fade_opa, LV_OPA_TRANSP, LV_OPA_COVER, 250, lv_anim_path_ease_out, NULL);
}

/* ---- The browser's history, kept up ---- */

static void browse_tick(void)
{
    muse_browse_bench_tick();
    uint32_t seq = muse_browse_seq();
    if (seq != s_browse_seq && muse_browse_get(s_browse)) {
        s_browse_seq = seq;
        s_browse_heard_ms = now_ms();
        if (s_browse->turn != s_browse_turn) {
            s_browse_turn = s_browse->turn;
            s_browse_dismissed = false;   /* a new turn's */
        }
        if (s_sheet && s_view == VIEW_BROWSE) {
            open_sheet(false, 0);   /* the new step on it */
        }
    }
    if (s_sheet && s_view == VIEW_BROWSE && now_ms() - s_times_at > 1000) {
        s_times_at = now_ms();
        for (int i = 0; i < s_browse_shown && i < MUSE_BROWSE_STEPS; i++) {
            char t[12];
            if (s_browse_times[i]) {
                ago(t, sizeof(t), s_browse->steps[i].at_ms);
                lv_label_set_text(s_browse_times[i], t);
            }
        }
    }
}

/* Product pictures come in: a card showing a placeholder for one has it. */
static void pic_tick(void)
{
    if (!s_scroll || s_detail == NONE) {
        return;
    }
    lv_obj_t *frame = lv_obj_get_child(s_scroll, 0);
    if (!frame || !lv_obj_has_flag(frame, LV_OBJ_FLAG_USER_1)) {
        return;
    }
    const lv_image_dsc_t *pic = muse_widget_pic(lv_obj_get_user_data(frame));
    if (!pic) {
        return;
    }
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_USER_1);
    lv_obj_clean(frame);
    lv_obj_t *img = lv_image_create(frame);
    lv_image_set_src(img, pic);
    lv_obj_center(img);
    anim(img, fade_opa, LV_OPA_TRANSP, LV_OPA_COVER, 250, lv_anim_path_ease_out, NULL);
}

/* ---- The face's ---- */

void muse_widget_ui_build(lv_obj_t *face, int w, int h, int top)
{
    s_face = face;
    s_w = w;
    s_h = h;
    s_top = top;
    s_detail = s_typing = NONE;
    s_set = heap_caps_calloc(1, sizeof(*s_set), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_browse = heap_caps_calloc(1, sizeof(*s_browse), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    static const lv_style_prop_t PRESS[] = { LV_STYLE_TRANSFORM_WIDTH, LV_STYLE_TRANSFORM_HEIGHT, 0 };
    lv_style_transition_dsc_init(&s_press_tr, PRESS, lv_anim_path_ease_out, 90, 0, NULL);

    s_chip = lv_obj_create(face);
    lv_obj_remove_style_all(s_chip);
    lv_obj_set_size(s_chip, LV_SIZE_CONTENT, CHIP_H);
    lv_obj_set_style_pad_left(s_chip, 16, 0);
    lv_obj_set_style_pad_right(s_chip, 6, 0);
    lv_obj_set_style_pad_column(s_chip, 8, 0);
    lv_obj_set_style_radius(s_chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_chip, lv_color_hex(COLOR_CHIP_BG), 0);
    lv_obj_set_style_bg_color(s_chip, lv_color_hex(MUSE_COLOR_CARD_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_chip, 1, 0);
    lv_obj_set_style_border_color(s_chip, lv_color_hex(COLOR_CHIP_EDGE), 0);
    lv_obj_set_ext_click_area(s_chip, 8);
    lv_obj_set_flex_flow(s_chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_chip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_chip, LV_OBJ_FLAG_CLICKABLE);
    press_look(s_chip);
    s_chip_icon = muse_style_label(s_chip, MUSE_FONT_NOTE, MUSE_COLOR_ACCENT, "");
    s_chip_lbl = muse_style_label(s_chip, &lv_font_unscii_16, COLOR_CHIP_TEXT, "");
    lv_obj_set_style_max_width(s_chip_lbl, CHIP_TEXT_W, 0);
    lv_obj_set_height(s_chip_lbl, lv_font_get_line_height(&lv_font_unscii_16));
    lv_label_set_long_mode(s_chip_lbl, LV_LABEL_LONG_MODE_DOTS);
    s_chip_x = round_button(s_chip, 28, LV_SYMBOL_CLOSE, on_chip_x);   /* done with it */
    lv_obj_set_style_bg_color(s_chip_x, lv_color_hex(MUSE_COLOR_CARD_PRESSED), 0);
    lv_obj_align(s_chip, LV_ALIGN_BOTTOM_MID, 0, -CHIP_BOTTOM + (CHIP_H - 36) / 2);
    lv_obj_add_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
    static const lv_event_code_t CHIP[] = { LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED,
                                            LV_EVENT_PRESS_LOST, LV_EVENT_CLICKED };
    for (size_t i = 0; i < sizeof(CHIP) / sizeof(CHIP[0]); i++) {
        lv_obj_add_event_cb(s_chip, on_chip, CHIP[i], NULL);
    }
}

void muse_widget_ui_tick(muse_mode_t mode, float now, bool may_open)
{
    if (!s_set || !s_browse) {
        return;
    }
    if (mode == MUSE_MODE_LISTENING && (s_state == ST_WAITING || s_state == ST_OPEN || s_state == ST_AWAY)) {
        ESP_LOGI(TAG, "a new turn: the widgets go");   /* answered by voice, or passed over */
        muse_widget_keys_close();
        if (s_view == VIEW_WIDGETS) {
            delete_sheet();
        }
        s_state = ST_NONE;
        s_detail = NONE;
    }
    if (mode == MUSE_MODE_LISTENING && s_sheet && s_view == VIEW_BROWSE) {
        delete_sheet();
    }
    uint32_t seq = muse_widget_seq();
    if (seq != s_seq && s_state != ST_SENT && muse_widget_get(s_set)) {
        bool same = s_set->seq == s_seq;
        s_seq = s_set->seq;
        if (!s_set->count) {
            if (s_state == ST_OPEN && s_view == VIEW_WIDGETS) {
                put_away(false);
            }
            s_state = ST_NONE;
            memset(s_checked, 0, sizeof(s_checked));
            memset(s_values, 0, sizeof(s_values));
        } else if (s_state == ST_OPEN && s_view == VIEW_WIDGETS && !same) {
            if (!muse_widget_keys_up()) {
                open_sheet(false, 0);   /* another came: the sheet again, with it */
            }
        } else if (s_state != ST_OPEN) {
            s_state = ST_WAITING;
            s_since = now;
        }
    }
    browse_tick();
    if (s_state == ST_WAITING && may_open && mode != MUSE_MODE_LISTENING && !(s_sheet && s_view == VIEW_BROWSE)
        && (mode == MUSE_MODE_SPEAKING || mode == MUSE_MODE_IDLE || now - s_since > THINKING_OPEN_S)) {
        s_view = VIEW_WIDGETS;
        s_detail = NONE;
        open_sheet(true, 0);
    }
    muse_widget_map_tick();
    pic_tick();
    chip_tick(mode, now, may_open);
}

bool muse_widget_ui_sheet_up(void)
{
    return s_sheet != NULL || muse_widget_keys_up();
}

bool muse_widget_ui_chip_up(void)
{
    return s_chip && !lv_obj_has_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
}

bool muse_widget_ui_browsing(uint32_t *site, float *done)
{
    if (!s_browse || !s_browse->task[0]) {
        return false;
    }
    const muse_browse_t *b = s_browse;
    *site = 0;
    for (int i = b->count - 1; i >= 0 && !*site; i--) {
        *site = b->steps[i].site[0] ? muse_browse_color(b->steps[i].site) : 0;
    }
    if (muse_browse_running(b)) {
        *done = -1.0f;
        return now_ms() - s_browse_heard_ms < BROWSE_QUIET_MS;
    }
    float t = (float)(now_ms() - b->done_ms) / 1000.0f;
    *done = t / BROWSE_DONE_S;
    return t < BROWSE_DONE_S + 0.3f;
}

bool muse_widget_ui_browse_open(void)
{
    if (!s_browse || (!s_browse->count && !muse_browse_running(s_browse)) || s_state == ST_SENT) {
        return false;
    }
    if (s_sheet && s_view == VIEW_BROWSE) {
        return true;
    }
    if (s_state == ST_OPEN) {
        s_state = ST_AWAY;   /* the widgets wait in the chip meanwhile */
    }
    chip_hide();
    s_chip_for = CHIP_NONE;
    s_view = VIEW_BROWSE;
    s_browse_dismissed = false;
    open_sheet(true, 0);
    return true;
}

#endif
