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
 * The dialog card (muse_dialog.h), in the shared look (muse_style.h): a card
 * on a dimmed backdrop, at most 3/4 of the screen tall, scrolling only when
 * what's on it doesn't fit. Its buttons are the settings pages' buttons, its
 * options their rows, and its "?" the Chats screen's.
 */
#include "muse_dialog.h"

#include <stdint.h>
#include <string.h>

#include "muse_board.h"

#define CARD_W 360
#define HELP_INSET 10   /* the "?" in from the card's top left corner */
#define TITLE_MAX 48

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

/* Full width, centred, wrapped. */
static lv_obj_t *para(lv_obj_t *card, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = muse_style_label(card, font, color, text);
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

/* The whole parent dimmed, and a card in the middle of it, its scrollbar in
 * its padding, clear of its rounded corners. */
static lv_obj_t *backdrop(lv_obj_t *parent, lv_obj_t **card_out)
{
    lv_obj_t *card;
    lv_obj_t *b = muse_style_backdrop(parent, CARD_W, &card);
    lv_obj_add_event_cb(b, on_backdrop, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_max_height(card, muse_board->height * 3 / 4, 0);   /* the dimmed screen shows round it */
    muse_style_scroll_column(card, 6, 3, 20, 20);
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

static lv_obj_t *card_button(lv_obj_t *card, const char *text, muse_dialog_style_t style)
{
    return muse_style_button(card, text, style, MUSE_FONT_BUTTON, MUSE_BUTTON_H);
}

/* A row to pick, as on the settings pages: icon, text, and a tick on the
 * right if it's the current one. */
static lv_obj_t *option(lv_obj_t *card, const muse_dialog_option_t *o)
{
    lv_obj_t *r = muse_style_row(card, true, true);
    lv_obj_set_height(r, MUSE_BUTTON_H);
    if (o->icon) {
        muse_style_label(r, MUSE_FONT_BUTTON, MUSE_COLOR_ACCENT, o->icon);
    }
    lv_obj_t *t = muse_style_label(r, MUSE_FONT_BUTTON, o->ticked ? MUSE_COLOR_ACCENT : MUSE_COLOR_TEXT, o->label);
    lv_obj_set_flex_grow(t, 1);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *tick = muse_style_label(r, MUSE_FONT_BUTTON, MUSE_COLOR_ACCENT, o->ticked ? LV_SYMBOL_OK : "");
    lv_obj_set_width(tick, MUSE_TICK_W);   /* kept either way, so the text keeps its width */
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
    para(card, MUSE_FONT_CARD_TITLE, MUSE_COLOR_TEXT, s_title);
    lv_obj_t *t = para(card, MUSE_FONT_NOTE, MUSE_COLOR_TEXT, s_help_text);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_LEFT, 0);   /* a few lines, read down the left */
    lv_obj_set_style_pad_bottom(t, 4, 0);
    lv_obj_add_event_cb(card_button(card, "Got it", MUSE_DIALOG_ACCENT), on_help_done, LV_EVENT_CLICKED, NULL);
}

/*
 * The Chats screen's "?" (muse_style_help_button), in the card's top left
 * corner, out of the column and its scrolling; the title beside it is
 * centred on it and kept clear of it either side, so it stays centred.
 */
static void help_button(lv_obj_t *card, lv_obj_t *title)
{
    const int off = HELP_INSET - MUSE_CARD_PAD - MUSE_CARD_BORDER;   /* from the content's corner */
    lv_obj_t *b = muse_style_help_button(card);
    lv_obj_add_flag(b, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, off, off);
    lv_obj_add_event_cb(b, on_help, LV_EVENT_CLICKED, NULL);

    int top = off + MUSE_HELP_D / 2 - lv_font_get_line_height(MUSE_FONT_CARD_TITLE) / 2;
    lv_obj_set_style_pad_top(title, top > 0 ? top : 0, 0);
    lv_obj_set_style_min_height(title, off + MUSE_HELP_D, 0);   /* what's under it starts below the "?" */
    lv_obj_set_style_pad_hor(title, off + MUSE_HELP_D + 4, 0);
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

    lv_obj_t *t = para(card, MUSE_FONT_CARD_TITLE, MUSE_COLOR_TEXT, s_title);
    if (d->help) {
        help_button(card, t);
    }
    if (d->subtitle) {
        lv_obj_t *s = para(card, MUSE_FONT_CARD_TITLE, MUSE_COLOR_ACCENT, d->subtitle);
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
    }
    if (d->text) {
        lv_obj_set_style_pad_bottom(para(card, MUSE_FONT_NOTE, MUSE_COLOR_DIM, d->text), 4, 0);
    }
    lv_obj_t *body = NULL;
    if (d->body) {
        body = lv_obj_create(card);
        lv_obj_remove_style_all(body);
        lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(body, MUSE_CARD_GAP, 0);
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
