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
 * Inside the card (16 px padding, the dialogs'): a grabber, the header (the
 * title in the card title's type, its line dim under it, a close button on
 * the right), then the column. Rows are muse_style_row's on a card, at
 * least MUSE_BUTTON_H tall, the text wrapping rather than cut: a leading
 * icon in the accent colour, the title over a dim line or two, then on the
 * right a price or a site, and a button where the row answers by one.
 */
#include "muse_widget_ui.h"

#if MUSE_WIDGET_UI

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "muse_chat.h"
#include "muse_style.h"
#include "muse_widget.h"

static const char *TAG = "muse_widget";

#define SHEET_SIDE 12           /* in from the screen's sides */
#define SHEET_BOTTOM 26         /* up from its bottom: the page dots show under it */
#define SHEET_PAD_TOP 8         /* the grabber's room */
#define GRABBER_W 40
#define GRABBER_H 5
#define CLOSE_D 36              /* the close button, 52 px to hit */
#define ROW_PAD_V 10
#define ICON_W 28               /* a row's leading icon's column */
#define PILL_PAD 18             /* an option pill's text from its ends */
#define PILLS_MAX 3
#define SCROLLBAR_ROOM 10       /* the column's right, while it scrolls */
#define FADE_H 24               /* the column's edges, while there's more that way */
#define DRAG_AWAY_PX 60         /* the header dragged down this far puts the sheet away */
#define DOT_GAP 8.0f            /* "dotted_lines": dots round an option pill */
#define DOT_PX 3
#define OPEN_MS 320
#define AWAY_MS 220
#define FOLD_MS 260
#define SENT_HOLD_MS 1300       /* the "sent" chip, before it goes */
#define TOAST_HOLD_MS 1600
#define THINKING_OPEN_S 4.0f    /* widgets in, the speech held up (an image): opened anyway */
#define CHIP_CAPTION_S 3.0f     /* a new caption keeps the chip out of its way this long */
#define CHIP_BOTTOM 34          /* "up next"'s place (muse_home_extras.c) */
#define CHIP_TEXT_W 300
#define COLOR_CHIP_BG 0x1d1733  /* "up next"'s look */
#define COLOR_CHIP_EDGE 0x5b3fa0
#define COLOR_CHIP_TEXT 0xe4defa
#define FONT_SMALL (&lv_font_montserrat_14)

typedef enum {
    ST_NONE,      /* no widgets, or done with them */
    ST_WAITING,   /* in, the reply not yet being said */
    ST_OPEN,      /* the sheet's up */
    ST_SENT,      /* answered: folding into the "sent" chip, then gone */
    ST_AWAY,      /* put away: the chip brings it back */
} state_t;

EXT_RAM_BSS_ATTR static muse_widget_set_t *s_set;   /* the face's copy, PSRAM */
EXT_RAM_BSS_ATTR static uint32_t s_seq;
EXT_RAM_BSS_ATTR static state_t s_state;
EXT_RAM_BSS_ATTR static float s_since;               /* when the set came */
EXT_RAM_BSS_ATTR static int s_w, s_h, s_top;
EXT_RAM_BSS_ATTR static lv_obj_t *s_face, *s_sheet, *s_scroll, *s_fade[2], *s_toast;
EXT_RAM_BSS_ATTR static lv_obj_t *s_chip, *s_chip_icon, *s_chip_lbl;
EXT_RAM_BSS_ATTR static int32_t s_drag;               /* the header dragged down this far */
EXT_RAM_BSS_ATTR static char s_sent[MUSE_WIDGET_ROW_TITLE];
EXT_RAM_BSS_ATTR static lv_style_transition_dsc_t s_press_tr;
EXT_RAM_BSS_ATTR static uint32_t s_caption_ver;      /* the caption the chip last saw, */
EXT_RAM_BSS_ATTR static float s_caption_at;          /* and when it was new (0: never) */

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

/* ---- Icons: Montserrat's symbols, and the two it lacks, drawn ---- */

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

static void pin_icon(lv_obj_t *col)
{
    lv_obj_t *p = box(col);
    lv_obj_set_size(p, 18, 18);
    lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(MUSE_COLOR_ACCENT), 0);
    lv_obj_t *dot = box(p);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(MUSE_COLOR_CARD), 0);
    lv_obj_center(dot);
    lv_obj_center(p);
}

/* An option's ring, filled for "center_aligned_filled" (or once chosen). */
static void ring_icon(lv_obj_t *col, bool filled)
{
    lv_obj_t *r = box(col);
    lv_obj_set_size(r, 20, 20);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(r, 2, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(MUSE_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(r, filled ? LV_OPA_40 : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(MUSE_COLOR_ACCENT), 0);
    lv_obj_center(r);
}

static const char *kind_symbol(uint8_t kind)
{
    switch (kind) {
    case MUSE_WIDGET_OPTIONS: return LV_SYMBOL_NEW_LINE;   /* an answer to give */
    case MUSE_WIDGET_MAP: return LV_SYMBOL_GPS;
    case MUSE_WIDGET_SHOPPING: return "$";
    case MUSE_WIDGET_CARD: return LV_SYMBOL_FILE;
    default: return LV_SYMBOL_LIST;
    }
}

/* A row's leading icon, in a column of its own so every row's text lines up. */
static void leading(lv_obj_t *row, uint8_t type, bool filled)
{
    lv_obj_t *col = box(row);
    lv_obj_set_size(col, ICON_W, 24);
    const char *sym = NULL;
    switch (type) {
    case MUSE_WIDGET_ROW_OPTION: ring_icon(col, filled); return;
    case MUSE_WIDGET_ROW_CALENDAR: calendar_icon(col); return;
    case MUSE_WIDGET_ROW_PLACE: pin_icon(col); return;
    case MUSE_WIDGET_ROW_SEND: sym = LV_SYMBOL_NEW_LINE; break;
    case MUSE_WIDGET_ROW_LINK: sym = LV_SYMBOL_UPLOAD; break;
    case MUSE_WIDGET_ROW_EMAIL: sym = LV_SYMBOL_ENVELOPE; break;
    case MUSE_WIDGET_ROW_FLIGHT: sym = LV_SYMBOL_GPS; break;
    case MUSE_WIDGET_ROW_PRODUCT: sym = "$"; break;
    default: sym = LV_SYMBOL_BULLET; break;
    }
    lv_obj_center(muse_style_label(col, MUSE_FONT_BUTTON, MUSE_COLOR_ACCENT, sym));
}

/* ---- Answering ---- */

static void fold(void *unused);

static const muse_widget_row_t *row_at(intptr_t at)
{
    int wi = (int)(at >> 8), ri = (int)(at & 0xff);
    if (!s_set || wi >= s_set->count || ri >= s_set->w[wi].count) {
        return NULL;
    }
    return &s_set->w[wi].rows[ri];
}

/* The row's words go to the widget's chat as the user's message; the sheet folds into a chip saying them. */
static void answer(const muse_widget_row_t *r)
{
    if (s_state != ST_OPEN || !r || !r->send[0]) {
        return;
    }
    if (!muse_hatch_reply_text(s_set->sid, r->send)) {
        show_toast("Muse can't be reached right now");
        return;
    }
    ESP_LOGI(TAG, "answered: \"%s\"", r->send);
    if (r->type == MUSE_WIDGET_ROW_OPTION) {
        strlcpy(s_sent, r->title, sizeof(s_sent));   /* shown as it was */
    } else {
        muse_widget_text(s_sent, sizeof(s_sent), r->send, false);
    }
    s_state = ST_SENT;
    lv_async_call(fold, NULL);   /* out of the tapped row's event before its sheet's emptied */
}

static void on_row(lv_event_t *e)
{
    const muse_widget_row_t *r = row_at((intptr_t)lv_event_get_user_data(e));
    if (!r || s_state != ST_OPEN) {
        return;
    }
    if (r->type == MUSE_WIDGET_ROW_LINK && !r->send[0]) {
        show_toast("Open it in the Muse app on your phone");
        return;
    }
    answer(r);
}

/* ---- Rows ---- */

/* The title over its dim lines, taking the row's width that's left. */
static void row_words(lv_obj_t *row, const muse_widget_row_t *r)
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
    text(col, MUSE_FONT_BUTTON, MUSE_COLOR_TEXT, t, true);
    if (r->sub[0]) {
        text(col, MUSE_FONT_NOTE, MUSE_COLOR_DIM, r->sub, true);
    }
    if (r->extra[0] && r->type != MUSE_WIDGET_ROW_PRODUCT) {
        text(col, MUSE_FONT_NOTE, MUSE_COLOR_DIM, r->extra, true);
    }
}

/* On the right: a price (a product's old one struck under it) or a site. */
static void row_meta(lv_obj_t *row, const muse_widget_row_t *r)
{
    if (!r->meta[0]) {
        return;
    }
    lv_obj_t *col = box(row);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    bool site = r->type == MUSE_WIDGET_ROW_LINK;
    const lv_font_t *font = site ? MUSE_FONT_NOTE : MUSE_FONT_VALUE;
    lv_obj_t *m = muse_style_label(col, font, site ? MUSE_COLOR_DIM : MUSE_COLOR_ACCENT, r->meta);
    lv_obj_set_style_max_width(m, 150, 0);
    lv_obj_set_height(m, lv_font_get_line_height(font));   /* one line, ending in dots */
    lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_DOTS);
    if (r->type == MUSE_WIDGET_ROW_PRODUCT && r->extra[0]) {
        lv_obj_t *was = muse_style_label(col, FONT_SMALL, MUSE_COLOR_DIM, r->extra);
        lv_obj_set_style_text_decor(was, LV_TEXT_DECOR_STRIKETHROUGH, 0);
    }
}

/* A small filled button on the row's right (a flight's "Book", a product's "Add"), 48 px to hit. */
static void row_button(lv_obj_t *row, const muse_widget_row_t *r, intptr_t at)
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
    lv_obj_center(muse_style_label(b, MUSE_FONT_NOTE, MUSE_COLOR_CARD, r->button));
    lv_obj_add_event_cb(b, on_row, LV_EVENT_CLICKED, (void *)at);
}

/* What ends a row that answers by a tap: a send arrow, or a chevron to a place. */
static void row_trailing(lv_obj_t *row, const muse_widget_row_t *r)
{
    if (r->type == MUSE_WIDGET_ROW_SEND) {
        lv_obj_t *c = box(row);
        lv_obj_set_size(c, 32, 32);
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(MUSE_COLOR_ACCENT), 0);
        lv_obj_center(muse_style_label(c, MUSE_FONT_NOTE, MUSE_COLOR_CARD, LV_SYMBOL_UP));
    } else if (r->type == MUSE_WIDGET_ROW_PLACE) {
        muse_style_label(row, MUSE_FONT_NOTE, MUSE_COLOR_DIM, LV_SYMBOL_RIGHT);
    }
}

static void add_row(lv_obj_t *col, const muse_widget_t *w, int wi, int ri)
{
    const muse_widget_row_t *r = &w->rows[ri];
    intptr_t at = (intptr_t)(wi << 8 | ri);
    bool tap = (r->send[0] && !r->button[0]) || r->type == MUSE_WIDGET_ROW_LINK;
    lv_obj_t *row = muse_style_row(col, tap, true);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, MUSE_BUTTON_H, 0);
    lv_obj_set_style_pad_ver(row, ROW_PAD_V, 0);
    if (tap) {
        press_look(row);
        lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, (void *)at);
    }
    leading(row, r->type, w->filled);
    row_words(row, r);
    row_meta(row, r);
    if (r->button[0] && r->send[0]) {
        row_button(row, r, at);
    } else if (tap) {
        row_trailing(row, r);
    }
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
    int pill = (inner_w() - SCROLLBAR_ROOM - (w->count - 1) * MUSE_CARD_GAP) / w->count;   /* room either way */
    for (int i = 0; i < w->count; i++) {
        lv_point_t size;
        lv_text_get_size(&size, w->rows[i].title, MUSE_FONT_BUTTON, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x + 2 * PILL_PAD > pill) {
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
    lv_obj_t *note = text(col, MUSE_FONT_NOTE, MUSE_COLOR_DIM, "Open in the Muse app to see this", true);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
}

/* ---- The sheet ---- */

static void delete_sheet(void)
{
    if (s_sheet) {
        lv_anim_delete(s_sheet, NULL);
        lv_obj_delete_async(s_sheet);   /* we may be in one of its own events */
        s_sheet = s_scroll = s_toast = s_fade[0] = s_fade[1] = NULL;
    }
}

static void translate_y(void *o, int32_t v)
{
    lv_obj_set_style_translate_y(o, v, 0);
}

static void slide(int32_t from, int32_t to, uint32_t ms, lv_anim_path_cb_t path, lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_sheet);
    lv_anim_set_exec_cb(&a, translate_y);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, path);
    lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

static void gone(lv_anim_t *a)
{
    (void)a;
    delete_sheet();
}

/* Down and off the screen; then the chip brings it back (ST_AWAY), or nothing does. */
static void put_away(state_t then)
{
    if (!s_sheet) {
        s_state = then;
        return;
    }
    s_state = then;
    ESP_LOGI(TAG, "sheet put away");
    slide(lv_obj_get_style_translate_y(s_sheet, 0), lv_obj_get_height(s_sheet) + SHEET_BOTTOM, AWAY_MS,
          lv_anim_path_ease_in, gone);
}

static void on_close(lv_event_t *e)
{
    (void)e;
    if (s_state == ST_OPEN) {
        put_away(ST_AWAY);
    }
}

/* Swiped down where nothing scrolls. */
static void on_gesture(lv_event_t *e)
{
    (void)e;
    if (s_state == ST_OPEN && lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_BOTTOM) {
        put_away(ST_AWAY);
    }
}

/* The header follows a finger down; let go far enough down, the sheet goes, else it springs back. */
static void on_header_drag(lv_event_t *e)
{
    if (s_state != ST_OPEN) {
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
        put_away(ST_AWAY);
    } else if (s_drag > 0) {
        slide(s_drag, 0, 200, lv_anim_path_ease_out, NULL);
    }
}

/* The column's edges fade while there's more that way. */
static void on_scroll(lv_event_t *e)
{
    (void)e;
    if (!s_scroll) {
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

/* Grabber, then the title and its line beside the close button. */
static void build_header(const muse_widget_t *w)
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

    lv_obj_t *line = box(head);
    lv_obj_set_size(line, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(line, MUSE_ROW_GAP_X, 0);
    lv_obj_t *words = box(line);
    lv_obj_set_height(words, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(words, 1);
    lv_obj_set_flex_flow(words, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(words, 2, 0);
    const char *title = w->title[0] ? w->title : w->kind == MUSE_WIDGET_OPTIONS ? "Choose one" : "From Muse";
    text(words, MUSE_FONT_CARD_TITLE, MUSE_COLOR_TEXT, title, true);
    if (w->text[0] && w->kind != MUSE_WIDGET_CARD) {
        text(words, MUSE_FONT_NOTE, MUSE_COLOR_DIM, w->text, true);
    }

    lv_obj_t *x = lv_button_create(line);
    lv_obj_remove_style_all(x);
    lv_obj_set_size(x, CLOSE_D, CLOSE_D);
    lv_obj_set_ext_click_area(x, (52 - CLOSE_D) / 2);
    lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(x, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(x, lv_color_hex(MUSE_COLOR_CARD_PRESSED), 0);
    lv_obj_set_style_bg_color(x, lv_color_hex(MUSE_COLOR_RAISED_PRESSED), LV_STATE_PRESSED);
    press_look(x);
    lv_obj_center(muse_style_label(x, MUSE_FONT_NOTE, MUSE_COLOR_DIM, LV_SYMBOL_CLOSE));
    lv_obj_add_event_cb(x, on_close, LV_EVENT_CLICKED, NULL);
}

static void build_column(void)
{
    s_scroll = box(s_sheet);
    lv_obj_set_size(s_scroll, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_scroll, MUSE_CARD_GAP, 0);
    for (int wi = 0; wi < s_set->count; wi++) {
        const muse_widget_t *w = &s_set->w[wi];
        if (wi > 0) {
            /* A second widget: its title as a section's, a gap over it. */
            lv_obj_t *t = text(s_scroll, MUSE_FONT_NOTE, MUSE_COLOR_ACCENT,
                               w->title[0] ? w->title : w->kind == MUSE_WIDGET_OPTIONS ? "Choose one" : "From Muse", true);
            lv_obj_set_style_pad_top(t, 8, 0);
        }
        if (w->kind == MUSE_WIDGET_CARD) {
            add_card(s_scroll, w);
        } else if (w->kind == MUSE_WIDGET_OPTIONS && pills_fit(w)) {
            add_pills(s_scroll, w, wi);
        } else {
            for (int ri = 0; ri < w->count; ri++) {
                add_row(s_scroll, w, wi, ri);
            }
        }
    }
}

/*
 * Builds the sheet for the set, its column as tall as what's on it up to
 * the room there is, then scrolling; slides it up if `slide_in`.
 */
static void open_sheet(bool slide_in)
{
    delete_sheet();
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

    build_header(&s_set->w[0]);
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
        on_scroll(NULL);
    }
    s_state = ST_OPEN;
    ESP_LOGI(TAG, "sheet up: %d widget%s, %s%s", s_set->count, s_set->count == 1 ? "" : "s", s_set->w[0].name,
             scrolls ? ", scrolling" : "");
    if (slide_in) {
        lv_obj_update_layout(s_sheet);
        slide(lv_obj_get_height(s_sheet) + SHEET_BOTTOM, 0, OPEN_MS, lv_anim_path_overshoot, NULL);
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

static void on_chip(lv_event_t *e)
{
    (void)e;
    if (s_state == ST_AWAY && s_set->count) {
        lv_obj_add_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
        open_sheet(true);
    }
}

/* What the chip says: the options, or the first widget's title. */
static void chip_text(void)
{
    const muse_widget_t *w = &s_set->w[0];
    char t[MUSE_WIDGET_TITLE] = "";
    if (w->kind == MUSE_WIDGET_OPTIONS) {
        for (int i = 0; i < w->count && strlen(t) + 3 < sizeof(t); i++) {
            strlcat(t, i ? ", " : "", sizeof(t));
            strlcat(t, w->rows[i].title, sizeof(t));
        }
    } else {
        strlcpy(t, w->title[0] ? w->title : "From Muse", sizeof(t));
    }
    lv_label_set_text(s_chip_icon, kind_symbol(w->kind));
    lv_label_set_text(s_chip_lbl, t);
}

static void chip_tick(muse_mode_t mode, float now, bool may_open)
{
    char c[4] = "";
    if (muse_state_caption(c, sizeof(c), &s_caption_ver) && c[0]) {
        s_caption_at = now;
    }
    bool show = s_state == ST_AWAY && mode == MUSE_MODE_IDLE && may_open
                && (!s_caption_at || now - s_caption_at > CHIP_CAPTION_S);
    if (show == !lv_obj_has_flag(s_chip, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    lv_obj_set_flag(s_chip, LV_OBJ_FLAG_HIDDEN, !show);
    if (show) {
        lv_obj_move_foreground(s_chip);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_chip);
        lv_anim_set_exec_cb(&a, fade_opa);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&a, 250);
        lv_anim_start(&a);
    }
}

/* ---- The face's ---- */

void muse_widget_ui_build(lv_obj_t *face, int w, int h, int top)
{
    s_face = face;
    s_w = w;
    s_h = h;
    s_top = top;
    s_set = heap_caps_calloc(1, sizeof(*s_set), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    static const lv_style_prop_t PRESS[] = { LV_STYLE_TRANSFORM_WIDTH, LV_STYLE_TRANSFORM_HEIGHT, 0 };
    lv_style_transition_dsc_init(&s_press_tr, PRESS, lv_anim_path_ease_out, 90, 0, NULL);

    s_chip = lv_button_create(face);
    lv_obj_remove_style_all(s_chip);
    lv_obj_set_size(s_chip, LV_SIZE_CONTENT, 36);
    lv_obj_set_style_pad_hor(s_chip, 16, 0);
    lv_obj_set_style_pad_column(s_chip, 8, 0);
    lv_obj_set_style_radius(s_chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_chip, lv_color_hex(COLOR_CHIP_BG), 0);
    lv_obj_set_style_bg_color(s_chip, lv_color_hex(MUSE_COLOR_CARD_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_chip, 1, 0);
    lv_obj_set_style_border_color(s_chip, lv_color_hex(COLOR_CHIP_EDGE), 0);
    lv_obj_set_ext_click_area(s_chip, 10);
    lv_obj_set_flex_flow(s_chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_chip, LV_OBJ_FLAG_SCROLLABLE);
    press_look(s_chip);
    s_chip_icon = muse_style_label(s_chip, MUSE_FONT_NOTE, MUSE_COLOR_ACCENT, "");
    s_chip_lbl = muse_style_label(s_chip, &lv_font_unscii_16, COLOR_CHIP_TEXT, "");
    lv_obj_set_style_max_width(s_chip_lbl, CHIP_TEXT_W, 0);
    lv_obj_set_height(s_chip_lbl, lv_font_get_line_height(&lv_font_unscii_16));
    lv_label_set_long_mode(s_chip_lbl, LV_LABEL_LONG_MODE_DOTS);
    muse_style_label(s_chip, FONT_SMALL, MUSE_COLOR_DIM, LV_SYMBOL_UP);   /* it comes back up */
    lv_obj_align(s_chip, LV_ALIGN_BOTTOM_MID, 0, -CHIP_BOTTOM);
    lv_obj_add_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_chip, on_chip, LV_EVENT_CLICKED, NULL);
}

void muse_widget_ui_tick(muse_mode_t mode, float now, bool may_open)
{
    if (!s_set) {
        return;
    }
    if (mode == MUSE_MODE_LISTENING && (s_state == ST_WAITING || s_state == ST_OPEN || s_state == ST_AWAY)) {
        ESP_LOGI(TAG, "a new turn: the widgets go");   /* answered by voice, or passed over */
        delete_sheet();
        s_state = ST_NONE;
    }
    uint32_t seq = muse_widget_seq();
    if (seq != s_seq && s_state != ST_SENT && muse_widget_get(s_set)) {
        s_seq = s_set->seq;
        if (!s_set->count) {
            if (s_state == ST_OPEN) {
                put_away(ST_NONE);
            } else {
                s_state = ST_NONE;
            }
        } else if (s_state == ST_OPEN) {
            open_sheet(false);   /* another came: the sheet again, with it */
        } else {
            s_state = ST_WAITING;
            s_since = now;
        }
        if (s_set->count) {
            chip_text();
        }
    }
    if (s_state == ST_WAITING && may_open && mode != MUSE_MODE_LISTENING
        && (mode == MUSE_MODE_SPEAKING || mode == MUSE_MODE_IDLE || now - s_since > THINKING_OPEN_S)) {
        open_sheet(true);
    }
    chip_tick(mode, now, may_open);
}

bool muse_widget_ui_sheet_up(void)
{
    return s_sheet != NULL;
}

bool muse_widget_ui_chip_up(void)
{
    return s_chip && !lv_obj_has_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
}

#endif
