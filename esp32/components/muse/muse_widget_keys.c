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
 * Typing an answer (muse_widget_keys.h), on the 2.16's 480 px square:
 *
 *   y  16..64   Cancel, and the answer's button, a pill
 *   y  76..     the question (wrapping), then the field under it
 *   y 248..474  the keys: four rows 52 px tall, ten keys across a row
 *               (44 px each), shift and delete wider, the space bar widest
 *
 * The keys are one button matrix, laid out like a phone's: letters (a
 * capital first, then small), and numbers and symbols. The return key is
 * drawn in the accent colour (a draw task's colours, as a matrix can't style
 * one key), dim until there's something to send. A letter or symbol pressed
 * rises over the finger, bigger, as a phone's does (the matrix's popover),
 * and goes in when it's let go; every key lights and clicks as it's touched
 * (muse_style_pressable_keys).
 */
#include "muse_widget_keys.h"

#include <ctype.h>
#include <string.h>

#include "esp_attr.h"

#include "muse_style.h"

#define PAD 20
#define TOP_H 48
#define KEYS_H 226
#define KEYS_BOTTOM 6
#define KEY_GAP 6
#define TEXT_MAX 200
#define OPEN_MS 260
#define CLOSE_MS 200
#define COLOR_KEY 0x2e2552        /* MUSE_COLOR_CARD_PRESSED: a key */
#define COLOR_KEY_SPECIAL 0x221c3e   /* shift, delete, 123: a shade darker */

#if LV_FONT_MONTSERRAT_28
#define FONT_POPOVER (&lv_font_montserrat_28)   /* a pressed key's letter, over it */
#else
#define FONT_POPOVER (&lv_font_montserrat_20)
#endif
#define RET_W_MAX 96   /* the return key's name, bigger, fits it */

#define SHIFT LV_SYMBOL_UP
#define DEL LV_SYMBOL_BACKSPACE

EXT_RAM_BSS_ATTR static char s_ret[16];   /* the return key's name ("Send") */

static const char *const LOWER[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    " ", "a", "s", "d", "f", "g", "h", "j", "k", "l", " ", "\n",
    SHIFT, "z", "x", "c", "v", "b", "n", "m", DEL, "\n",
    "123", ",", " ", ".", s_ret, "",
};
static const char *const UPPER[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    " ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
    SHIFT, "Z", "X", "C", "V", "B", "N", "M", DEL, "\n",
    "123", ",", " ", ".", s_ret, "",
};
static const char *const NUMBERS[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "-", "/", ":", ";", "(", ")", "$", "&", "@", "\"", "\n",
    "#", ".", ",", "?", "!", "'", "%", "*", DEL, "\n",
    "ABC", "=", " ", "+", s_ret, "",
};

#define K(w) (LV_BUTTONMATRIX_CTRL_WIDTH_##w | LV_BUTTONMATRIX_CTRL_NO_REPEAT)
#define C(w) (K(w) | LV_BUTTONMATRIX_CTRL_POPOVER)   /* a character: it pops up */
#define SPECIAL(w) (K(w) | LV_BUTTONMATRIX_CTRL_CHECKED)
#define RET(w) (SPECIAL(w) | LV_BUTTONMATRIX_CTRL_CUSTOM_1)   /* the primary click */
#define GAP (LV_BUTTONMATRIX_CTRL_WIDTH_1 | LV_BUTTONMATRIX_CTRL_HIDDEN)
static const lv_buttonmatrix_ctrl_t LETTERS_CTRL[] = {
    C(2), C(2), C(2), C(2), C(2), C(2), C(2), C(2), C(2), C(2),
    GAP, C(2), C(2), C(2), C(2), C(2), C(2), C(2), C(2), C(2), GAP,
    SPECIAL(3), C(2), C(2), C(2), C(2), C(2), C(2), C(2), SPECIAL(3) & ~LV_BUTTONMATRIX_CTRL_NO_REPEAT,
    SPECIAL(3), C(2), K(8), C(2), RET(5),
};
static const lv_buttonmatrix_ctrl_t NUMBERS_CTRL[] = {
    C(2), C(2), C(2), C(2), C(2), C(2), C(2), C(2), C(2), C(2),
    C(2), C(2), C(2), C(2), C(2), C(2), C(2), C(2), C(2), C(2),
    C(2), C(2), C(2), C(2), C(2), C(2), C(2), C(2), SPECIAL(4) & ~LV_BUTTONMATRIX_CTRL_NO_REPEAT,
    SPECIAL(3), C(2), K(8), C(2), RET(5),
};
#define RET_ID (k.layout == KEYS_NUMBERS ? 33 : 34)   /* the return key's index */

typedef enum { KEYS_LOWER, KEYS_UPPER, KEYS_NUMBERS } layout_t;

EXT_RAM_BSS_ATTR static struct {
    lv_obj_t *root, *ta, *keys, *send, *send_lbl;
    layout_t layout;
    bool shift_once;         /* the next letter only, then small again */
    bool can_send;
    bool closing;
    muse_keys_done_t done;
    void *ctx;
} k;

static void set_layout(layout_t l)
{
    k.layout = l;
    lv_buttonmatrix_set_map(k.keys, l == KEYS_LOWER ? LOWER : l == KEYS_UPPER ? UPPER : NUMBERS);
    lv_buttonmatrix_set_ctrl_map(k.keys, l == KEYS_NUMBERS ? NUMBERS_CTRL : LETTERS_CTRL);
}

/* What's typed, trimmed, into out; whether there's any. */
static bool answer(char *out, size_t cap)
{
    const char *t = lv_textarea_get_text(k.ta);
    while (*t && isspace((unsigned char)*t)) {
        t++;
    }
    size_t n = strlen(t);
    while (n && isspace((unsigned char)t[n - 1])) {
        n--;
    }
    n = n < cap - 1 ? n : cap - 1;
    memcpy(out, t, n);
    out[n] = '\0';
    return n > 0;
}

static void refresh_send(void)
{
    char t[TEXT_MAX + 1];
    bool can = answer(t, sizeof(t));
    if (can == k.can_send) {
        return;
    }
    k.can_send = can;
    lv_obj_set_state(k.send, LV_STATE_DISABLED, !can);
    lv_obj_invalidate(k.keys);   /* the return key's colour */
}

static void fade_opa(void *o, int32_t v)
{
    lv_obj_set_style_opa(o, (lv_opa_t)v, 0);
}

static void slide_y(void *o, int32_t v)
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

static void gone(lv_anim_t *a)
{
    (void)a;
    if (k.root) {
        lv_obj_delete(k.root);
    }
    k.root = k.ta = k.keys = k.send = NULL;
    k.closing = false;
}

/* Slides down and goes; then the answer (or NULL) to the caller. */
static void finish(bool send)
{
    if (!k.root || k.closing) {
        return;
    }
    char t[TEXT_MAX + 1];
    if (send && !answer(t, sizeof(t))) {
        return;
    }
    k.closing = true;
    muse_keys_done_t done = k.done;
    k.done = NULL;
    anim(k.keys, slide_y, 0, KEYS_H + KEYS_BOTTOM, CLOSE_MS, lv_anim_path_ease_in, NULL);
    anim(k.root, fade_opa, LV_OPA_COVER, LV_OPA_TRANSP, CLOSE_MS, lv_anim_path_ease_in, gone);
    if (done) {
        done(send ? t : NULL, k.ctx);
    }
}

static void on_key(lv_event_t *e)
{
    (void)e;
    uint32_t id = lv_buttonmatrix_get_selected_button(k.keys);
    const char *t = id == LV_BUTTONMATRIX_BUTTON_NONE ? NULL : lv_buttonmatrix_get_button_text(k.keys, id);
    if (!t || k.closing) {
        return;
    }
    if (id == RET_ID) {
        finish(true);
        return;
    }
    if (!strcmp(t, SHIFT)) {
        set_layout(k.layout == KEYS_UPPER ? KEYS_LOWER : KEYS_UPPER);
        k.shift_once = k.layout == KEYS_UPPER;
        return;
    }
    if (!strcmp(t, "123")) {
        set_layout(KEYS_NUMBERS);
        return;
    }
    if (!strcmp(t, "ABC")) {
        set_layout(KEYS_LOWER);
        return;
    }
    if (!strcmp(t, DEL)) {
        lv_textarea_delete_char(k.ta);
        if (!lv_textarea_get_text(k.ta)[0] && k.layout == KEYS_LOWER) {
            set_layout(KEYS_UPPER);   /* empty again: a capital first */
            k.shift_once = true;
        }
    } else {
        lv_textarea_add_text(k.ta, t);
        if (k.layout == KEYS_UPPER && k.shift_once) {
            set_layout(KEYS_LOWER);
            k.shift_once = false;
        } else if (k.layout == KEYS_NUMBERS && !strcmp(t, " ")) {
            set_layout(KEYS_LOWER);   /* a word's numbers done */
        }
    }
    refresh_send();
}

/* Keys' colours: special ones darker, the return key the accent's (dim until it can send). */
static void on_key_draw(lv_event_t *e)
{
    lv_draw_task_t *t = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(t);
    if (!base || base->part != LV_PART_ITEMS || base->id1 != RET_ID) {
        return;
    }
    if (lv_draw_task_get_type(t) == LV_DRAW_TASK_TYPE_FILL) {
        lv_draw_fill_dsc_t *f = lv_draw_task_get_fill_dsc(t);
        f->color = lv_color_hex(k.can_send ? MUSE_COLOR_ACCENT : COLOR_KEY_SPECIAL);
    } else if (lv_draw_task_get_type(t) == LV_DRAW_TASK_TYPE_LABEL) {
        lv_draw_label_dsc_t *l = lv_draw_task_get_label_dsc(t);
        l->color = lv_color_hex(k.can_send ? MUSE_COLOR_CARD : MUSE_COLOR_DIM);
    }
}

static void on_cancel(lv_event_t *e)
{
    (void)e;
    finish(false);
}

static void on_send(lv_event_t *e)
{
    (void)e;
    finish(true);
}

static lv_obj_t *build_keys(lv_obj_t *root)
{
    lv_obj_t *kb = lv_buttonmatrix_create(root);
    lv_obj_remove_style_all(kb);
    lv_obj_set_size(kb, lv_pct(100), KEYS_H);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -KEYS_BOTTOM);
    lv_obj_set_style_pad_hor(kb, 8, 0);   /* the corner keys clear of the screen's round corners */
    lv_obj_set_style_pad_row(kb, KEY_GAP + 2, 0);
    lv_obj_set_style_pad_column(kb, KEY_GAP, 0);
    lv_obj_set_style_radius(kb, 10, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(COLOR_KEY), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(COLOR_KEY_SPECIAL), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, lv_color_hex(MUSE_COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_20, LV_PART_ITEMS);
    /* Pressed, a key's words are bigger: a letter's in its popover, but a return key's name too. */
    lv_point_t ret;
    lv_text_get_size(&ret, s_ret, FONT_POPOVER, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (ret.x <= RET_W_MAX) {
        lv_obj_set_style_text_font(kb, FONT_POPOVER, LV_PART_ITEMS | LV_STATE_PRESSED);
    }
    muse_style_pressable_keys(kb);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_remove_flag(kb, LV_OBJ_FLAG_GESTURE_BUBBLE);   /* a sloppy swipe mustn't lose the words */
    lv_obj_add_event_cb(kb, on_key, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(kb, on_key_draw, LV_EVENT_DRAW_TASK_ADDED, NULL);
    return kb;
}

void muse_widget_keys_open(lv_obj_t *parent, const char *prompt, const char *placeholder, const char *initial,
                           const char *send, muse_keys_done_t done, void *ctx)
{
    if (k.root) {
        lv_anim_delete(k.root, NULL);
        lv_anim_delete(k.keys, NULL);
        lv_obj_delete(k.root);
    }
    memset(&k, 0, sizeof(k));
    k.done = done;
    k.ctx = ctx;
    strlcpy(s_ret, send && send[0] ? send : "Send", sizeof(s_ret));

    k.root = lv_obj_create(parent);
    lv_obj_remove_style_all(k.root);
    lv_obj_set_size(k.root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(k.root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(k.root, lv_color_black(), 0);
    lv_obj_add_flag(k.root, LV_OBJ_FLAG_CLICKABLE);   /* nothing under it takes a tap */
    lv_obj_remove_flag(k.root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* Cancel, and the pill that sends. */
    lv_obj_t *cancel = lv_button_create(k.root);
    lv_obj_remove_style_all(cancel);
    lv_obj_set_size(cancel, LV_SIZE_CONTENT, TOP_H);
    lv_obj_set_style_pad_hor(cancel, 8, 0);
    lv_obj_set_ext_click_area(cancel, 8);
    lv_obj_center(muse_style_label(cancel, MUSE_FONT_BUTTON, MUSE_COLOR_ACCENT, "Cancel"));
    lv_obj_set_style_text_opa(cancel, LV_OPA_60, LV_STATE_PRESSED);
    muse_style_pressable(cancel, MUSE_PRESS_BUTTON, false);
    lv_obj_align(cancel, LV_ALIGN_TOP_LEFT, PAD - 8, 16);
    lv_obj_add_event_cb(cancel, on_cancel, LV_EVENT_CLICKED, NULL);

    k.send = lv_button_create(k.root);
    lv_obj_remove_style_all(k.send);
    lv_obj_set_size(k.send, LV_SIZE_CONTENT, 40);
    lv_obj_set_style_pad_hor(k.send, 20, 0);
    lv_obj_set_style_radius(k.send, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(k.send, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(k.send, lv_color_hex(MUSE_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_color(k.send, lv_color_hex(MUSE_COLOR_ACCENT_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(k.send, lv_color_hex(MUSE_COLOR_CARD_PRESSED), LV_STATE_DISABLED);
    lv_obj_set_ext_click_area(k.send, 6);
    k.send_lbl = muse_style_label(k.send, MUSE_FONT_NOTE, MUSE_COLOR_CARD, s_ret);
    lv_obj_set_style_text_color(k.send_lbl, lv_color_hex(MUSE_COLOR_DIM), LV_STATE_DISABLED);
    lv_obj_center(k.send_lbl);
    lv_obj_align(k.send, LV_ALIGN_TOP_RIGHT, -PAD, 20);
    lv_obj_add_state(k.send, LV_STATE_DISABLED);
    muse_style_pressable(k.send, MUSE_PRESS_BUTTON, true);
    lv_obj_add_event_cb(k.send, on_send, LV_EVENT_CLICKED, NULL);

    /* The question over the field, as tall as they need. */
    lv_obj_t *col = lv_obj_create(k.root);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(col, PAD, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 12, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(col, LV_ALIGN_TOP_LEFT, 0, 16 + TOP_H + 10);
    if (prompt && prompt[0]) {
        lv_obj_t *q = muse_style_label(col, MUSE_FONT_CARD_TITLE, MUSE_COLOR_TEXT, prompt);
        lv_obj_set_width(q, lv_pct(100));
        lv_label_set_long_mode(q, LV_LABEL_LONG_MODE_WRAP);
    }
    k.ta = lv_textarea_create(col);
    lv_obj_set_width(k.ta, lv_pct(100));
    lv_obj_set_height(k.ta, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(k.ta, 3 * lv_font_get_line_height(&lv_font_montserrat_20) + 28, 0);
    lv_textarea_set_max_length(k.ta, TEXT_MAX);
    lv_obj_set_style_text_font(k.ta, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(k.ta, lv_color_hex(MUSE_COLOR_TEXT), 0);
    lv_obj_set_style_bg_color(k.ta, lv_color_hex(MUSE_COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(k.ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(k.ta, lv_color_hex(MUSE_COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(k.ta, 2, 0);
    lv_obj_set_style_radius(k.ta, 16, 0);
    lv_obj_set_style_pad_all(k.ta, 12, 0);
    lv_obj_set_style_pad_hor(k.ta, 14, 0);
    lv_obj_set_style_bg_color(k.ta, lv_color_hex(MUSE_COLOR_ACCENT), LV_PART_CURSOR);
    lv_obj_set_style_border_color(k.ta, lv_color_hex(MUSE_COLOR_ACCENT), LV_PART_CURSOR);
    lv_obj_set_style_text_color(k.ta, lv_color_hex(MUSE_COLOR_DIM), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_textarea_set_placeholder_text(k.ta, placeholder && placeholder[0] ? placeholder : "Type here");
    lv_textarea_set_text(k.ta, initial ? initial : "");
    lv_obj_add_state(k.ta, LV_STATE_FOCUSED);   /* the cursor blinking in it */
    lv_obj_remove_flag(k.ta, LV_OBJ_FLAG_GESTURE_BUBBLE);

    k.keys = build_keys(k.root);
    bool empty = !lv_textarea_get_text(k.ta)[0];
    set_layout(empty ? KEYS_UPPER : KEYS_LOWER);
    k.shift_once = empty;
    k.can_send = true;
    refresh_send();

    lv_obj_move_foreground(k.root);
    anim(k.root, fade_opa, LV_OPA_TRANSP, LV_OPA_COVER, OPEN_MS * 2 / 3, lv_anim_path_ease_out, NULL);
    anim(k.keys, slide_y, KEYS_H + KEYS_BOTTOM, 0, OPEN_MS, lv_anim_path_ease_out, NULL);
}

bool muse_widget_keys_up(void)
{
    return k.root != NULL;
}

void muse_widget_keys_close(void)
{
    if (k.root) {
        lv_anim_delete(k.root, NULL);
        lv_anim_delete(k.keys, NULL);
        lv_obj_delete(k.root);
    }
    k.root = k.ta = k.keys = k.send = NULL;
    k.done = NULL;
    k.closing = false;
}
