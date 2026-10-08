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
 * The Chats screen (muse_chats_ui.h): the main chat, the gadget's own, and
 * the ones made here (muse_settings_chat_items), the one in use ticked.
 * Tapping one picks it and goes back to Muse; holding a made one arms
 * forgetting it, and a tap then forgets it. "New chat" is ticked when picked;
 * the chat joins the list once the first thing asked in it has the Muse
 * title it (muse_settings_chat_pick_new, muse_settings_chat_retitle). Styled as the settings pages are
 * (muse_settings_ui.c). The "?" in the top left corner opens a card that says all this.
 */
#include "muse_chats_ui.h"

#include <string.h>

#include "esp_timer.h"

#include "muse_board.h"
#include "muse_settings.h"
#include "muse_ui.h"

#define LIST_W 330
#define LIST_TOP 84
#define ROW_H 58
#define GUTTER 12   /* either side of the rows; the scrollbar runs down the right one */
#define HELP_W 360
#define FORGET_ARMED_US 4000000   /* how long a held chat waits for the tap that forgets it */

#define COLOR_TEXT 0xf2efff
#define COLOR_DIM 0x8b84a8
#define COLOR_CARD 0x1a1530
#define COLOR_CARD_PRESSED 0x2e2552
#define COLOR_ACCENT 0xa77dff
#define COLOR_ACCENT_PRESSED 0xc8adff
#define COLOR_DANGER 0xff5c5c

static lv_obj_t *s_list;
static lv_obj_t *s_note;
static muse_chat_item_t s_items[MUSE_CHAT_ITEMS_MAX];
static lv_obj_t *s_values[MUSE_CHAT_ITEMS_MAX];
static int s_count;
static int s_current = -1;
static uint32_t s_gen = UINT32_MAX;
static int s_armed = -1;       /* the chat a hold armed to forget */
static int64_t s_armed_us;
static lv_obj_t *s_tile;
static lv_obj_t *s_help;       /* the "?" button's card, over the list while it's open */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static void set_text(lv_obj_t *l, const char *text)
{
    if (l && strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

/*
 * Scrolls only when its content doesn't fit, with no bounce when it does,
 * and a scrollbar while it doesn't: dim on black, by the right edge, its
 * bottom end `bottom` px up (clear of a curve or a rounded corner).
 */
static void scroll_column(lv_obj_t *o, int bottom)
{
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(o, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(o, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(o, lv_color_hex(COLOR_DIM), LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(o, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_top(o, 8, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_bottom(o, bottom, LV_PART_SCROLLBAR);
}

/* Tappable row: an icon, the text over a smaller dim line (sub, if not NULL), and a value on the right. */
static lv_obj_t *row(const char *icon, const char *text, const char *sub, lv_event_cb_t cb, void *user)
{
    lv_obj_t *c = lv_button_create(s_list);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, lv_pct(100), ROW_H);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(COLOR_CARD_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(c, 16, 0);
    lv_obj_set_style_pad_column(c, 12, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    label(c, &lv_font_montserrat_20, COLOR_ACCENT, icon);
    lv_obj_t *col = lv_obj_create(c);
    lv_obj_remove_style_all(col);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);   /* taps go to the row */
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *t = label(col, &lv_font_montserrat_20, COLOR_TEXT, text);
    lv_obj_set_width(t, lv_pct(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    if (sub) {
        label(col, &lv_font_montserrat_14, COLOR_DIM, sub);
    }
    lv_obj_add_event_cb(c, cb, LV_EVENT_ALL, user);
    return label(c, &lv_font_montserrat_16, COLOR_ACCENT, "");
}

static void on_chat(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= s_count) {
        return;
    }
    if (code == LV_EVENT_LONG_PRESSED && s_items[i].kind == MUSE_CHAT_NAMED) {
        s_armed = i;
        s_armed_us = esp_timer_get_time();
        return;
    }
    if (code != LV_EVENT_SHORT_CLICKED) {   /* not after a hold: that press only arms */
        return;
    }
    if (i == s_armed) {
        s_armed = -1;
        muse_settings_chat_forget(s_items[i].sid);   /* the list rebuilds on the next tick */
        set_text(s_note, "Forgotten here; the chat itself stays in the Muse app.");
        return;
    }
    s_armed = -1;
    muse_settings_set_chat_sid(s_items[i].kind == MUSE_CHAT_MAIN ? "" : s_items[i].sid);
    muse_ui_show_face();   /* back to Muse, to talk in it */
}

static void on_new(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) {
        return;
    }
    s_armed = -1;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == s_current) {
        muse_ui_show_face();   /* picked already: nothing said in it yet */
        return;
    }
    if (muse_settings_chat_pick_new() == ESP_ERR_NO_MEM) {
        set_text(s_note, "Eight chats is the most; hold one to forget it first.");
        return;
    }
    muse_ui_show_face();   /* to ask the first thing, which the Muse names it by */
}

/* Rows for the chats as they are now: after one is made, picked, forgotten or told a mode. */
static void rebuild(void)
{
    /* The gadget mode each chat last heard (muse_gadget_mode_t order, as muse_gadget_mode_name). */
    static const char *const TOLD[] = { "Desk mode", "Night mode", "On-the-go mode" };
    s_count = muse_settings_chat_items(s_items, MUSE_CHAT_ITEMS_MAX, &s_current);
    s_armed = -1;
    lv_obj_clean(s_list);
    /* Main chat first, New chat right under it, then the rest as listed. */
    int order[MUSE_CHAT_ITEMS_MAX], n = 0;
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < s_count; i++) {
            muse_chat_kind_t k = s_items[i].kind;
            if ((pass == 0 && k == MUSE_CHAT_MAIN) || (pass == 1 && k == MUSE_CHAT_NEW)
                || (pass == 2 && k != MUSE_CHAT_MAIN && k != MUSE_CHAT_NEW)) {
                order[n++] = i;
            }
        }
    }
    for (int o = 0; o < n; o++) {
        int i = order[o];
        const muse_chat_item_t *c = &s_items[i];
        if (c->kind == MUSE_CHAT_NEW) {
            s_values[i] = row(LV_SYMBOL_PLUS, "New chat", NULL, on_new, (void *)(intptr_t)i);
            continue;
        }
        const char *icon = c->kind == MUSE_CHAT_MAIN ? LV_SYMBOL_HOME
                         : c->kind == MUSE_CHAT_GADGET ? LV_SYMBOL_AUDIO : LV_SYMBOL_LIST;
        const char *name = c->kind == MUSE_CHAT_MAIN ? "Main chat"
                         : c->kind == MUSE_CHAT_GADGET ? "Gadget chat"
                         : c->name[0] ? c->name : c->sid;
        const char *told = c->told_mode >= 0 && c->told_mode < (int)(sizeof(TOLD) / sizeof(TOLD[0]))
                         ? TOLD[c->told_mode] : NULL;
        s_values[i] = row(icon, name, told, on_chat, (void *)(intptr_t)i);
    }
    /* Empty but for a short word on what a tap just did; the "?" explains the rest. */
    s_note = label(s_list, &lv_font_montserrat_16, COLOR_DIM, "");
    lv_obj_set_width(s_note, lv_pct(100));
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_MODE_WRAP);
}

/* ---------- help: the "?" in the top left corner ---------- */

static void close_help(void)
{
    if (s_help) {
        lv_obj_delete_async(s_help);   /* we may be in one of its own events */
        s_help = NULL;
    }
}

static void on_help_close(lv_event_t *e)
{
    /* A tap on the dim backdrop closes it; one on the card itself doesn't. */
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) {
        close_help();
    }
}

/* One tip: the icon it's about, beside a line or two. */
static void help_tip(lv_obj_t *card, const char *icon, const char *text)
{
    lv_obj_t *r = lv_obj_create(card);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r, 12, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *i = label(r, &lv_font_montserrat_16, COLOR_ACCENT, icon);
    lv_obj_set_width(i, 20);
    lv_obj_t *t = label(r, &lv_font_montserrat_16, COLOR_TEXT, text);
    lv_obj_set_flex_grow(t, 1);
    lv_obj_set_style_text_line_space(t, 2, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
}

/*
 * A card over the whole screen, the rest dimmed. It's on the tile, so it goes
 * with it; the backdrop doesn't pass drags on to the screens either side.
 */
static void open_help(void)
{
    if (s_help) {
        return;
    }
    s_help = lv_obj_create(s_tile);
    lv_obj_remove_style_all(s_help);
    lv_obj_set_size(s_help, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_help, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_help, LV_OPA_80, 0);
    lv_obj_remove_flag(s_help, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_help, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_help, on_help_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_help);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, HELP_W, LV_SIZE_CONTENT);
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
    scroll_column(card, 20);   /* in its padding, clear of its rounded corners */
    lv_obj_set_style_pad_top(card, 20, LV_PART_SCROLLBAR);

    label(card, &lv_font_montserrat_20, COLOR_TEXT, "How chats work");
    help_tip(card, LV_SYMBOL_LIST, "Tap a chat to talk in it.");
    help_tip(card, LV_SYMBOL_PLUS, "New chat: Muse names it from your first question.");
    help_tip(card, LV_SYMBOL_CLOSE, "Hold, then tap, to remove a chat here. It stays in the Muse app.");
    help_tip(card, LV_SYMBOL_HOME, "A restart begins on the main chat.");
    help_tip(card, LV_SYMBOL_SHUFFLE, "The word by a chat is the mode it last heard.");

    lv_obj_t *ok = lv_button_create(card);
    lv_obj_remove_style_all(ok);
    lv_obj_set_size(ok, lv_pct(100), 44);
    lv_obj_set_style_radius(ok, 16, 0);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(COLOR_ACCENT_PRESSED), LV_STATE_PRESSED);
    lv_obj_add_event_cb(ok, on_help_close, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label(ok, &lv_font_montserrat_20, COLOR_CARD, "Got it"));
}

static void on_help(lv_event_t *e)
{
    (void)e;
    s_armed = -1;
    open_help();
}

/* "?" in the top left corner, level with the title: 48 px to tap. */
static void build_help_button(lv_obj_t *tile)
{
    s_tile = tile;
    lv_obj_t *b = lv_button_create(tile);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 48, 48);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 20, 28);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_CARD_PRESSED), LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, on_help, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label(b, &lv_font_montserrat_20, COLOR_ACCENT, "?"));
}

void muse_chats_ui_build(lv_obj_t *tile)
{
    lv_obj_set_style_bg_color(tile, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = label(tile, &lv_font_unscii_16, COLOR_ACCENT, "CHATS");
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 44);

    s_list = lv_obj_create(tile);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, LIST_W + 2 * GUTTER, muse_board->height - LIST_TOP);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, LIST_TOP);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 10, 0);
    lv_obj_set_style_pad_hor(s_list, GUTTER, 0);
    /* Clear of the page dots, or a round screen's bottom curve; no more, or
     * a list that fits would scroll. */
    lv_obj_set_style_pad_bottom(s_list, muse_board->round ? 110 : 40, 0);
    scroll_column(s_list, muse_board->round ? 90 : 16);
    build_help_button(tile);
}

void muse_chats_ui_tick(bool visible)
{
    if (!s_list) {
        return;
    }
    if (!visible) {
        close_help();   /* swiped or sent away: not still open on the way back */
    }
    uint32_t gen = muse_settings_chats_gen();
    if (gen != s_gen) {
        s_gen = gen;
        rebuild();
    }
    if (!visible) {
        s_armed = -1;
    }
    if (s_armed >= 0 && esp_timer_get_time() - s_armed_us >= FORGET_ARMED_US) {
        s_armed = -1;
    }
    for (int i = 0; i < s_count; i++) {
        lv_obj_set_style_text_color(s_values[i], lv_color_hex(i == s_armed ? COLOR_DANGER : COLOR_ACCENT), 0);
        set_text(s_values[i], i == s_armed ? "Tap to forget" : i == s_current ? LV_SYMBOL_OK : "");
    }
}

bool muse_chats_ui_typing(void)
{
    return s_help != NULL;   /* the help card is up: no page dots, no swiping away */
}
