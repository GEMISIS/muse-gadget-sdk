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
 * The dialog card (muse_dialog.h), styled as the settings pages are
 * (muse_settings_ui.c): a card on a dimmed backdrop, at most 3/4 of the
 * screen tall, scrolling only when what's on it doesn't fit.
 */
#include "muse_dialog.h"

#include <stdint.h>
#include <string.h>

#include "muse_board.h"

#define CARD_W 360
#define BUTTON_H 52
#define HELP_D 36       /* the "?" circle */
#define TITLE_MAX 48

#define COLOR_TEXT 0xf2efff
#define COLOR_DIM 0x8b84a8
#define COLOR_CARD 0x1a1530
#define COLOR_CARD_PRESSED 0x2e2552
#define COLOR_NEUTRAL_PRESSED 0x3d3270
#define COLOR_ACCENT 0xa77dff
#define COLOR_ACCENT_PRESSED 0xc8adff
#define COLOR_DANGER 0xff5c5c
#define COLOR_DANGER_PRESSED 0xff8a8a

typedef struct {
    muse_dialog_cb_t cb;
    void *user;
} action_t;

static lv_obj_t *s_backdrop;   /* the dialog's, over its parent */
static lv_obj_t *s_help;       /* the "?" card's, over that */
static lv_obj_t *s_parent;     /* what it was opened over: NULL for the top layer */
static action_t s_actions[MUSE_DIALOG_BUTTONS_MAX + MUSE_DIALOG_OPTIONS_MAX];
static const char *s_help_text;
static char s_title[TITLE_MAX];

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

/* Full width, centred, wrapped. */
static lv_obj_t *para(lv_obj_t *card, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = label(card, font, color, text);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(l, 2, 0);
    return l;
}

static void drop(lv_obj_t **o)
{
    if (*o) {
        lv_obj_delete_async(*o);   /* we may be in one of its own events */
        *o = NULL;
    }
}

void muse_dialog_close(void)
{
    drop(&s_help);
    drop(&s_backdrop);
    s_help_text = NULL;
}

void muse_dialog_close_on(lv_obj_t *parent)
{
    if (s_backdrop && s_parent == parent) {
        muse_dialog_close();
    }
}

bool muse_dialog_is_open(void)
{
    return s_backdrop != NULL;
}

/* A tap on the dim backdrop closes its card; one on the card itself doesn't. */
static void on_backdrop(lv_event_t *e)
{
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) {
        return;
    }
    if (lv_event_get_current_target(e) == s_help) {
        drop(&s_help);
    } else {
        muse_dialog_close();
    }
}

/*
 * Scrolls only when its content doesn't fit, with no bounce when it does,
 * and a scrollbar while it doesn't: dim, in its padding, clear of its
 * rounded corners.
 */
static void scroll_column(lv_obj_t *o)
{
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(o, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(o, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(o, lv_color_hex(COLOR_DIM), LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(o, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_ver(o, 20, LV_PART_SCROLLBAR);
}

/* The whole parent dimmed, and a card in the middle of it. The backdrop
 * doesn't pass drags on to the screens either side. */
static lv_obj_t *backdrop(lv_obj_t *parent, lv_obj_t **card_out)
{
    lv_obj_t *b = lv_obj_create(parent ? parent : lv_layer_top());
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, on_backdrop, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(b);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_MIN(CARD_W, muse_board->width - 24), LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, muse_board->height * 3 / 4, 0);   /* the dimmed screen shows round it */
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);   /* taps on it stay on it */
    scroll_column(card);
    *card_out = card;
    return b;
}

static void on_action(lv_event_t *e)
{
    action_t a = s_actions[(int)(intptr_t)lv_event_get_user_data(e)];
    muse_dialog_close();
    if (a.cb) {
        a.cb(a.user);   /* may open another */
    }
}

/* A full-width button on a card. */
static lv_obj_t *card_button(lv_obj_t *card, const char *text, muse_dialog_style_t style)
{
    uint32_t bg = COLOR_CARD_PRESSED, pressed = COLOR_NEUTRAL_PRESSED, color = COLOR_TEXT;
    if (style == MUSE_DIALOG_ACCENT) {
        bg = COLOR_ACCENT;
        pressed = COLOR_ACCENT_PRESSED;
        color = COLOR_CARD;
    } else if (style == MUSE_DIALOG_DANGER) {
        bg = COLOR_DANGER;
        pressed = COLOR_DANGER_PRESSED;
        color = COLOR_CARD;
    }
    lv_obj_t *b = lv_button_create(card);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, lv_pct(100), BUTTON_H);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(pressed), LV_STATE_PRESSED);
    lv_obj_t *l = label(b, &lv_font_montserrat_20, color, text);
    lv_obj_set_width(l, lv_pct(90));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_center(l);
    return b;
}

/* A row to pick: icon, text, and a tick on the right if it's the current one. */
static lv_obj_t *option(lv_obj_t *card, const muse_dialog_option_t *o)
{
    lv_obj_t *r = lv_button_create(card);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), BUTTON_H);
    lv_obj_set_style_radius(r, 16, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(COLOR_CARD_PRESSED), 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(COLOR_NEUTRAL_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(r, 14, 0);
    lv_obj_set_style_pad_column(r, 12, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    if (o->icon) {
        label(r, &lv_font_montserrat_20, COLOR_ACCENT, o->icon);
    }
    lv_obj_t *t = label(r, &lv_font_montserrat_20, o->ticked ? COLOR_ACCENT : COLOR_TEXT, o->label);
    lv_obj_set_flex_grow(t, 1);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *tick = label(r, &lv_font_montserrat_20, COLOR_ACCENT, o->ticked ? LV_SYMBOL_OK : "");
    lv_obj_set_width(tick, 24);   /* kept either way, so the text keeps its width */
    lv_obj_set_style_text_align(tick, LV_TEXT_ALIGN_RIGHT, 0);
    return r;
}

static void on_help_done(lv_event_t *e)
{
    (void)e;
    drop(&s_help);   /* back to the dialog */
}

static void on_help(lv_event_t *e)
{
    (void)e;
    if (s_help || !s_help_text) {
        return;
    }
    lv_obj_t *card;
    s_help = backdrop(s_parent, &card);
    para(card, &lv_font_montserrat_20, COLOR_TEXT, s_title);
    lv_obj_t *t = para(card, &lv_font_montserrat_16, COLOR_TEXT, s_help_text);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_LEFT, 0);   /* a few lines, read down the left */
    lv_obj_set_style_pad_bottom(t, 4, 0);
    lv_obj_add_event_cb(card_button(card, "Got it", MUSE_DIALOG_ACCENT), on_help_done, LV_EVENT_CLICKED, NULL);
}

/* "?" in the card's top right corner, out of the column and its scrolling. */
static void help_button(lv_obj_t *card)
{
    lv_obj_t *b = lv_button_create(card);
    lv_obj_remove_style_all(b);
    lv_obj_add_flag(b, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(b, HELP_D, HELP_D);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, 6, -6);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_CARD_PRESSED), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_NEUTRAL_PRESSED), LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, on_help, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label(b, &lv_font_montserrat_20, COLOR_ACCENT, "?"));
}

lv_obj_t *muse_dialog_open(lv_obj_t *parent, const muse_dialog_t *d)
{
    if (s_backdrop) {
        return NULL;
    }
    lv_obj_t *card;
    s_parent = parent;
    s_backdrop = backdrop(parent, &card);
    strlcpy(s_title, d->title ? d->title : "", sizeof(s_title));
    s_help_text = d->help;

    lv_obj_t *t = para(card, &lv_font_montserrat_20, COLOR_TEXT, s_title);
    if (d->help) {
        lv_obj_set_style_pad_hor(t, HELP_D, 0);   /* centred, clear of the "?" */
        help_button(card);
    }
    if (d->subtitle) {
        lv_obj_t *s = para(card, &lv_font_montserrat_20, COLOR_ACCENT, d->subtitle);
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
    }
    if (d->text) {
        lv_obj_set_style_pad_bottom(para(card, &lv_font_montserrat_16, COLOR_DIM, d->text), 4, 0);
    }
    lv_obj_t *body = NULL;
    if (d->body) {
        body = lv_obj_create(card);
        lv_obj_remove_style_all(body);
        lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(body, 8, 0);
        lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }
    int n = 0;
    for (int i = 0; i < d->option_count && i < MUSE_DIALOG_OPTIONS_MAX; i++, n++) {
        s_actions[n] = (action_t){ d->options[i].cb, d->options[i].user };
        lv_obj_add_event_cb(option(card, &d->options[i]), on_action, LV_EVENT_CLICKED, (void *)(intptr_t)n);
    }
    for (int i = 0; i < d->button_count && i < MUSE_DIALOG_BUTTONS_MAX; i++, n++) {
        s_actions[n] = (action_t){ d->buttons[i].cb, d->buttons[i].user };
        lv_obj_t *b = card_button(card, d->buttons[i].label, d->buttons[i].style);
        lv_obj_add_event_cb(b, on_action, LV_EVENT_CLICKED, (void *)(intptr_t)n);
    }
    return body ? body : card;
}
