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
 * forgetting it, and a tap then forgets it. "New chat" asks for a name on
 * a keyboard of its own. Styled as the settings pages are
 * (muse_settings_ui.c).
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
#define FORGET_ARMED_US 4000000   /* how long a held chat waits for the tap that forgets it */

#define COLOR_TEXT 0xf2efff
#define COLOR_DIM 0x8b84a8
#define COLOR_CARD 0x1a1530
#define COLOR_CARD_PRESSED 0x2e2552
#define COLOR_ACCENT 0xa77dff
#define COLOR_DANGER 0xff5c5c

static lv_obj_t *s_list;
static lv_obj_t *s_note;
static lv_obj_t *s_entry;      /* the name being typed and its keyboard, over the list */
static lv_obj_t *s_entry_ta;
static muse_chat_item_t s_items[MUSE_CHAT_ITEMS_MAX];
static lv_obj_t *s_values[MUSE_CHAT_ITEMS_MAX];
static int s_count;
static int s_current = -1;
static uint32_t s_gen = UINT32_MAX;
static int s_armed = -1;       /* the chat a hold armed to forget */
static int64_t s_armed_us;

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

/* Tappable row: an icon, the text, and a value on the right. */
static lv_obj_t *row(const char *icon, const char *text, lv_event_cb_t cb, void *user)
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
    lv_obj_t *t = label(c, &lv_font_montserrat_20, COLOR_TEXT, text);
    lv_obj_set_flex_grow(t, 1);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
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

static void entry_close(void)
{
    lv_textarea_set_text(s_entry_ta, "");
    lv_obj_add_flag(s_entry, LV_OBJ_FLAG_HIDDEN);
}

static void on_entry_key(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CANCEL) {
        entry_close();
        return;
    }
    if (code != LV_EVENT_READY) {
        return;
    }
    char name[MUSE_CHAT_NAME_MAX + 1];
    strlcpy(name, lv_textarea_get_text(s_entry_ta), sizeof(name));
    if (!name[0]) {
        entry_close();
        return;
    }
    char sid[MUSE_CHAT_SID_MAX + 1];
    esp_err_t err = muse_settings_chat_new(name, sid);
    entry_close();
    if (err == ESP_ERR_NO_MEM) {
        set_text(s_note, "Eight chats is the most; hold one to forget it first.");
    } else if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        muse_ui_show_face();   /* made (or already there) and picked */
    } else {
        set_text(s_note, "That name won't do; try another.");
    }
}

static void on_new(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) {
        return;
    }
    s_armed = -1;
    lv_obj_remove_flag(s_entry, LV_OBJ_FLAG_HIDDEN);
}

static void build_entry(lv_obj_t *tile)
{
    s_entry = lv_obj_create(tile);
    lv_obj_remove_style_all(s_entry);
    lv_obj_set_size(s_entry, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_entry, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_entry, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_entry, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_entry, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *t = label(s_entry, &lv_font_unscii_16, COLOR_ACCENT, "NEW CHAT");
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 44);

    s_entry_ta = lv_textarea_create(s_entry);
    lv_textarea_set_one_line(s_entry_ta, true);
    lv_textarea_set_max_length(s_entry_ta, MUSE_CHAT_NAME_MAX);
    lv_textarea_set_placeholder_text(s_entry_ta, "Its name");
    lv_obj_set_width(s_entry_ta, LIST_W);
    lv_obj_set_style_text_font(s_entry_ta, &lv_font_montserrat_20, 0);
    lv_obj_align(s_entry_ta, LV_ALIGN_TOP_MID, 0, LIST_TOP);

    lv_obj_t *kb = lv_keyboard_create(s_entry);
    lv_obj_set_size(kb, muse_board->width, muse_board->height / 2);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_entry_ta);
    lv_obj_add_event_cb(kb, on_entry_key, LV_EVENT_ALL, NULL);
}

/* Rows for the chats as they are now: after one is made, picked or forgotten. */
static void rebuild(void)
{
    s_count = muse_settings_chat_items(s_items, MUSE_CHAT_ITEMS_MAX, &s_current);
    s_armed = -1;
    lv_obj_clean(s_list);
    for (int i = 0; i < s_count; i++) {
        const muse_chat_item_t *c = &s_items[i];
        const char *icon = c->kind == MUSE_CHAT_MAIN ? LV_SYMBOL_HOME
                         : c->kind == MUSE_CHAT_GADGET ? LV_SYMBOL_AUDIO : LV_SYMBOL_LIST;
        const char *name = c->kind == MUSE_CHAT_MAIN ? "Main chat"
                         : c->kind == MUSE_CHAT_GADGET ? "Gadget chat"
                         : c->name[0] ? c->name : c->sid;
        s_values[i] = row(icon, name, on_chat, (void *)(intptr_t)i);
    }
    row(LV_SYMBOL_PLUS, "New chat", on_new, NULL);
    s_note = label(s_list, &lv_font_montserrat_16, COLOR_DIM,
                   "Where you talk to Muse from here on; a restart goes back to the main chat. "
                   "A new chat shows up in the Muse app with its first message. Hold one to forget it.");
    lv_obj_set_width(s_note, lv_pct(100));
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_MODE_WRAP);
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
    lv_obj_set_size(s_list, LIST_W, muse_board->height - LIST_TOP);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, LIST_TOP);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 10, 0);
    lv_obj_set_style_pad_bottom(s_list, 110, 0);   /* clear of the page dots */
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    build_entry(tile);
}

void muse_chats_ui_tick(bool visible)
{
    if (!s_list) {
        return;
    }
    uint32_t gen = muse_settings_chats_gen();
    if (gen != s_gen) {
        s_gen = gen;
        rebuild();
    }
    if (!visible) {
        if (!lv_obj_has_flag(s_entry, LV_OBJ_FLAG_HIDDEN)) {
            entry_close();
        }
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
    return s_entry && !lv_obj_has_flag(s_entry, LV_OBJ_FLAG_HIDDEN);
}
