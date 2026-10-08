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
 * The Chats screen (muse_chats_ui.h): a list of the chats the conversation
 * can go to, the current one ticked. Tapping one picks it and goes back to
 * Muse. Styled as the settings pages are (muse_settings_ui.c).
 */
#include "muse_chats_ui.h"

#include <string.h>

#include "muse_board.h"
#include "muse_settings.h"
#include "muse_ui.h"

#define LIST_W 330
#define LIST_TOP 84
#define ROW_H 58

#define COLOR_TEXT 0xf2efff
#define COLOR_DIM 0x8b84a8
#define COLOR_CARD 0x1a1530
#define COLOR_CARD_PRESSED 0x2e2552
#define COLOR_ACCENT 0xa77dff

enum { CHAT_MAIN, CHAT_GADGET, CHAT_BUILTIN };

static lv_obj_t *s_list;
static lv_obj_t *s_checks[CHAT_BUILTIN];
static lv_obj_t *s_note;

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

/* Tappable row: the chat's name, and a tick when it's the one in use. */
static lv_obj_t *row(const char *text, lv_event_cb_t cb, void *user)
{
    lv_obj_t *c = lv_button_create(s_list);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, lv_pct(100), ROW_H);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(COLOR_CARD_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(c, 16, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = label(c, &lv_font_montserrat_20, COLOR_TEXT, text);
    lv_obj_set_flex_grow(t, 1);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, user);
    return label(c, &lv_font_montserrat_20, COLOR_ACCENT, "");
}

static void on_builtin(lv_event_t *e)
{
    char sid[MUSE_CHAT_SID_MAX + 1] = "";
    if ((intptr_t)lv_event_get_user_data(e) == CHAT_GADGET) {
        muse_settings_gadget_chat_sid(sid);
    }
    muse_settings_set_chat_sid(sid);
    muse_ui_show_face();   /* back to Muse, to talk in it */
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
    lv_obj_set_style_pad_bottom(s_list, 110, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    s_checks[CHAT_MAIN] = row("Main chat", on_builtin, (void *)(intptr_t)CHAT_MAIN);
    s_checks[CHAT_GADGET] = row("Gadget chat", on_builtin, (void *)(intptr_t)CHAT_GADGET);
    s_note = label(s_list, &lv_font_montserrat_16, COLOR_DIM,
                   "Where you talk to Muse from here on. It starts on the main chat after a restart.");
    lv_obj_set_width(s_note, lv_pct(100));
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_MODE_WRAP);
}

void muse_chats_ui_tick(bool visible)
{
    if (!visible || !s_list) {
        return;
    }
    char sid[MUSE_CHAT_SID_MAX + 1], gadget[MUSE_CHAT_SID_MAX + 1];
    muse_settings_chat_sid(sid);
    muse_settings_gadget_chat_sid(gadget);
    set_text(s_checks[CHAT_MAIN], sid[0] ? "" : LV_SYMBOL_OK);
    set_text(s_checks[CHAT_GADGET], sid[0] && !strcmp(sid, gadget) ? LV_SYMBOL_OK : "");
}

bool muse_chats_ui_typing(void)
{
    return false;
}
