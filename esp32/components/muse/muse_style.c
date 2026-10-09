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

/* The shared look (muse_style.h). */
#include "muse_style.h"

#include "muse_board.h"

#define BACKDROP_MARGIN 24   /* a card's least room from the screen's sides, together */

lv_obj_t *muse_style_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *muse_style_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *t = muse_style_label(parent, MUSE_FONT_TITLE, MUSE_COLOR_ACCENT, text);
    lv_obj_set_style_text_letter_space(t, MUSE_TITLE_LETTER_SPACE, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, MUSE_TITLE_Y);
    return t;
}

lv_obj_t *muse_style_row(lv_obj_t *parent, bool clickable, bool on_card)
{
    lv_obj_t *c = clickable ? lv_button_create(parent) : lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, lv_pct(100), MUSE_ROW_H);
    lv_obj_set_style_radius(c, MUSE_ROW_RADIUS, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(on_card ? MUSE_COLOR_CARD_PRESSED : MUSE_COLOR_CARD), 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(on_card ? MUSE_COLOR_RAISED_PRESSED : MUSE_COLOR_CARD_PRESSED),
                              LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(c, MUSE_ROW_PAD, 0);
    lv_obj_set_style_pad_column(c, MUSE_ROW_GAP_X, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/* The way out (Back, Cancel, "?") is outlined, on a card or off one; a
 * choice is filled. */
static void outline(lv_obj_t *b)
{
    lv_obj_set_style_border_width(b, MUSE_CARD_BORDER, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(MUSE_COLOR_ACCENT), 0);
    lv_obj_set_style_border_opa(b, LV_OPA_60, 0);
}

lv_obj_t *muse_style_button(lv_obj_t *parent, const char *text, muse_button_kind_t kind, const lv_font_t *font,
                            int h)
{
    uint32_t bg = MUSE_COLOR_CARD, pressed = MUSE_COLOR_CARD_PRESSED, color = MUSE_COLOR_TEXT;
    if (kind == MUSE_BUTTON_ACCENT) {
        bg = MUSE_COLOR_ACCENT;
        pressed = MUSE_COLOR_ACCENT_PRESSED;
        color = MUSE_COLOR_CARD;
    } else if (kind == MUSE_BUTTON_DANGER) {
        bg = MUSE_COLOR_DANGER;
        pressed = MUSE_COLOR_DANGER_PRESSED;
        color = MUSE_COLOR_CARD;
    }
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, lv_pct(100), h);
    lv_obj_set_style_radius(b, MUSE_ROW_RADIUS, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(pressed), LV_STATE_PRESSED);
    if (kind == MUSE_BUTTON_NEUTRAL) {
        outline(b);
    }
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = muse_style_label(b, font, color, text);
    lv_obj_set_style_max_width(l, lv_pct(90), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_center(l);
    return b;
}

lv_obj_t *muse_style_help_button(lv_obj_t *parent)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, MUSE_HELP_D, MUSE_HELP_D);
    lv_obj_set_ext_click_area(b, (48 - MUSE_HELP_D) / 2 + 4);   /* 48 px to hit or more, either size */
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(MUSE_COLOR_CARD), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(MUSE_COLOR_CARD_PRESSED), LV_STATE_PRESSED);
    outline(b);
    lv_obj_center(muse_style_label(b, MUSE_FONT_BUTTON, MUSE_COLOR_ACCENT, "?"));
    return b;
}

lv_obj_t *muse_style_padlock(lv_obj_t *parent, int size, uint32_t color)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, size, size);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    int line = size >= 20 ? 3 : 2;
    lv_obj_t *shackle = lv_obj_create(box);
    lv_obj_remove_style_all(shackle);
    lv_obj_set_size(shackle, size * 9 / 16, size * 5 / 8);
    lv_obj_align(shackle, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(shackle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(shackle, line, 0);
    lv_obj_set_style_border_color(shackle, lv_color_hex(color), 0);
    lv_obj_t *body = lv_obj_create(box);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, size * 13 / 16, size * 9 / 16);
    lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(body, size / 6, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(body, lv_color_hex(color), 0);
    return box;
}

void muse_style_card(lv_obj_t *card)
{
    lv_obj_set_style_radius(card, MUSE_CARD_RADIUS, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(MUSE_COLOR_CARD), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(MUSE_COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(card, MUSE_CARD_BORDER, 0);
    lv_obj_set_style_pad_all(card, MUSE_CARD_PAD, 0);
    lv_obj_set_style_pad_row(card, MUSE_CARD_GAP, 0);
}

lv_obj_t *muse_style_backdrop(lv_obj_t *parent, int w, lv_obj_t **card_out)
{
    lv_obj_t *b = lv_obj_create(parent ? parent : lv_layer_top());
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(b, MUSE_BACKDROP_OPA, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *card = lv_obj_create(b);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_MIN(w, muse_board->width - BACKDROP_MARGIN), LV_SIZE_CONTENT);
    lv_obj_center(card);
    muse_style_card(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);   /* taps on it stay on it */
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    *card_out = card;
    return b;
}

void muse_style_scroll_column(lv_obj_t *o, int width, int inset, int top, int bottom)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(o, width, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(o, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(o, lv_color_hex(MUSE_COLOR_DIM), LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(o, inset, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_top(o, top, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_bottom(o, bottom, LV_PART_SCROLLBAR);
}
