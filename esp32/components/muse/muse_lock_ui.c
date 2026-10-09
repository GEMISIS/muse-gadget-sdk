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
 * The passcode's keypad (muse_lock_ui.h). On the 2.16's 480 px square:
 *
 *   y  14..38   the title, or what went wrong
 *   y  50..64   six dots, or (locked out) why
 *   y  74..476  the keys: 96 px round, 24 px apart across and 6 down
 *
 * The keys are the widget sheet's round buttons, bigger; the titles and
 * notes the dialogs' type; it slides as the widget keyboard does. Smaller
 * screens get it scaled down (and a round one a little more).
 */
#include "muse_lock_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"

#include "muse_board.h"
#include "muse_dialog.h"
#include "muse_lock.h"
#include "muse_state.h"
#include "muse_style.h"

static const char *TAG = "muse_lock_ui";

#define KEY_D 96
#define KEY_GAP_X 24
#define KEY_GAP_Y 6
#define KEYS_BOTTOM 4
#define TITLE_Y 14
#define DOTS_Y 50
#define DOT_D 14
#define DOT_GAP 20
#define AVATAR_PX 128           /* Muse in the corner: two screen px a cell */
#define OPEN_MS 260
#define CLOSE_MS 200
#define SHAKE_MS 420
#define SHAKE_PX 14
#define CHECK_DELAY_MS 60       /* the last dot drawn before the hash holds the task up */
#define IDLE_CLOSE_S 15.0f
#define MISS_S 1.4f             /* Muse dizzy after a wrong one */
#define ERASE_DELAY_MS 1500     /* the erasing screen read before it starts */
#if LV_FONT_MONTSERRAT_48
#define FONT_DIGIT (&lv_font_montserrat_48)
#else
#define FONT_DIGIT (&lv_font_montserrat_28)
#endif

enum { KEY_SLOT = 10, KEY_DEL = 11, KEY_COUNT = 12 };
typedef enum { DO_UNLOCK, DO_SET, DO_CHANGE, DO_OFF } purpose_t;
typedef enum { STEP_CURRENT, STEP_CHOOSE, STEP_CONFIRM } step_t;
typedef enum { VIEW_KEYS, VIEW_LOCKOUT, VIEW_ERASE } view_t;

EXT_RAM_BSS_ATTR static struct {
    lv_obj_t *sheet, *title, *dots_row, *dots[MUSE_LOCK_PIN_LEN], *note, *keys[KEY_COUNT], *avatar, *cancel;
    int w, h, scale;            /* scale: per mille of the 480 px layout */
    bool up;
    purpose_t purpose;
    step_t step;
    view_t view;
    char typed[MUSE_LOCK_PIN_LEN + 1];
    char first[MUSE_LOCK_PIN_LEN + 1];   /* the new one, till it's typed again */
    int n;
    bool checking;              /* the hash is due or running */
    bool bench;                 /* the attempt is ">pin=": print its result */
    char hint[64];
    uint32_t hint_color;
    float now;                  /* the frame's time (muse_lock_ui_tick) */
    float touched_at, missed_at;
    bool warned;                /* the final round's warning, shown */
    bool erasing;
    uint32_t shown_left_s;
    lv_style_transition_dsc_t press_tr;
    muse_lock_ui_done_t done;
} u;

static int px(int v)
{
    return v * u.scale / 1000;
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

/* ---- What's shown ---- */

static void show_dots(void)
{
    for (int i = 0; i < MUSE_LOCK_PIN_LEN; i++) {
        lv_obj_set_style_bg_opa(u.dots[i], i < u.n ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    lv_obj_set_style_text_opa(lv_obj_get_child(u.keys[KEY_DEL], 0), u.n ? LV_OPA_COVER : LV_OPA_40, 0);
}

static const char *title_text(void)
{
    if (u.purpose == DO_UNLOCK || u.step == STEP_CURRENT) {
        return u.purpose == DO_UNLOCK ? "Enter passcode" : "Enter your passcode";
    }
    return u.step == STEP_CHOOSE ? "Choose a 6-digit passcode" : "Enter it again";
}

static void set_hint(const char *text, uint32_t color)
{
    strlcpy(u.hint, text ? text : "", sizeof(u.hint));
    u.hint_color = color;
}

static void show_title(void)
{
    lv_label_set_text(u.title, u.hint[0] ? u.hint : title_text());
    lv_obj_set_style_text_color(u.title, lv_color_hex(u.hint[0] ? u.hint_color : MUSE_COLOR_TEXT), 0);
}

/* The keys, or the lockout's countdown over them dimmed, or the erasing. */
static void show_view(view_t v)
{
    u.view = v;
    bool keys = v == VIEW_KEYS;
    lv_obj_set_flag(u.dots_row, LV_OBJ_FLAG_HIDDEN, !keys);
    lv_obj_set_flag(u.note, LV_OBJ_FLAG_HIDDEN, keys);
    lv_obj_t *box = lv_obj_get_parent(u.keys[0]);
    lv_obj_set_flag(box, LV_OBJ_FLAG_HIDDEN, v == VIEW_ERASE);
    lv_obj_set_style_opa(box, keys ? LV_OPA_COVER : LV_OPA_30, 0);
    for (int i = 0; i < KEY_COUNT; i++) {
        lv_obj_set_state(u.keys[i], LV_STATE_DISABLED, !keys);
    }
    bool unlocking = u.purpose == DO_UNLOCK;
    lv_obj_set_flag(u.avatar, LV_OBJ_FLAG_HIDDEN, !unlocking);
    lv_obj_set_flag(u.cancel, LV_OBJ_FLAG_HIDDEN, unlocking);
    if (v == VIEW_ERASE) {
        lv_obj_align(u.note, LV_ALIGN_CENTER, 0, 0);
        lv_obj_align(u.title, LV_ALIGN_CENTER, 0, -px(70));
        lv_label_set_text(u.title, "Erasing this device");
        lv_obj_set_style_text_color(u.title, lv_color_hex(MUSE_COLOR_DANGER), 0);
        lv_label_set_text(u.note, "Too many wrong passcodes. Everything on it is being erased: "
                                  "it restarts in a moment, ready to set up again in the Muse app.");
        return;
    }
    lv_obj_align(u.title, LV_ALIGN_TOP_MID, 0, px(TITLE_Y));
    lv_obj_align(u.note, LV_ALIGN_TOP_MID, 0, px(DOTS_Y) - 3);
    if (v == VIEW_LOCKOUT) {
        lv_label_set_text(u.note, "Too many wrong passcodes");
        u.shown_left_s = UINT32_MAX;   /* the countdown, next tick */
    } else {
        show_title();
    }
}

static void lockout_title(void)
{
    muse_lock_policy_t p;
    muse_lock_state(&p);
    if (p.lockout_s == u.shown_left_s) {
        return;
    }
    u.shown_left_s = p.lockout_s;
    char t[40];
    snprintf(t, sizeof(t), "Try again in %u:%02u", (unsigned)(p.lockout_s / 60), (unsigned)(p.lockout_s % 60));
    lv_label_set_text(u.title, t);
    lv_obj_set_style_text_color(u.title, lv_color_hex(MUSE_COLOR_TEXT), 0);
}

static void clear_typed(void)
{
    memset(u.typed, 0, sizeof(u.typed));
    u.n = 0;
    show_dots();
}

static void shake_done(lv_anim_t *a)
{
    (void)a;
    clear_typed();
}

/* Side to side, dying away: no. */
static void shake_x(void *o, int32_t t)
{
    float f = t / 1000.0f;
    translate_x(o, (int32_t)(px(SHAKE_PX) * sinf(f * 6.0f * (float)M_PI) * (1.0f - f)));
}

static void shake(void)
{
    anim(u.dots_row, shake_x, 0, 1000, SHAKE_MS, lv_anim_path_linear, shake_done);
}

/* ---- Opening and closing ---- */

static void hidden(lv_anim_t *a)
{
    (void)a;
    if (!u.up) {
        lv_obj_add_flag(u.sheet, LV_OBJ_FLAG_HIDDEN);
    }
}

static void slide_in(bool animate)
{
    lv_anim_delete(u.sheet, translate_y);
    lv_obj_remove_flag(u.sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(u.sheet);
    u.up = true;
    u.touched_at = 0;   /* from the next tick */
    if (animate) {
        anim(u.sheet, translate_y, u.h, 0, OPEN_MS, lv_anim_path_ease_out, NULL);
    } else {
        translate_y(u.sheet, 0);
    }
}

static void slide_out(bool animate)
{
    u.up = false;
    lv_anim_delete(u.dots_row, NULL);
    translate_x(u.dots_row, 0);
    clear_typed();
    memset(u.first, 0, sizeof(u.first));
    if (animate) {
        anim(u.sheet, translate_y, lv_obj_get_style_translate_y(u.sheet, 0), u.h, CLOSE_MS, lv_anim_path_ease_in,
             hidden);
    } else {
        lv_anim_delete(u.sheet, translate_y);
        lv_obj_add_flag(u.sheet, LV_OBJ_FLAG_HIDDEN);
    }
}

/* A Settings flow ends: `ok`, done; else cancelled. */
static void finish(bool ok)
{
    muse_lock_ui_done_t done = u.done;
    u.done = NULL;
    u.purpose = DO_UNLOCK;
    slide_out(true);
    if (done) {
        done(ok);
    }
}

static void begin(purpose_t purpose, step_t step, muse_lock_ui_done_t done)
{
    u.purpose = purpose;
    u.step = step;
    u.done = done;
    u.checking = u.bench = false;
    set_hint(NULL, 0);
    clear_typed();
    show_view(VIEW_KEYS);
}

/* The final round's warning, once a lockout's over. */
static void warn_final(void)
{
    static const muse_dialog_button_t ok = { "I understand", MUSE_DIALOG_ACCENT, NULL, NULL };
    char text[160];
    snprintf(text, sizeof(text),
             "%d more wrong attempts will erase this device: its Wi-Fi, pairing, chats and settings.",
             MUSE_LOCK_TRIES);
    const muse_dialog_t d = {
        .title = "Careful",
        .text = text,
        .buttons = &ok,
        .button_count = 1,
    };
    u.warned = muse_dialog_open(NULL, &d) != NULL;
}

/* ---- Checking ---- */

static void bench_print(const char *result, int left)
{
    if (u.bench) {
        printf("@pin {\"result\":\"%s\",\"left\":%d}\n", result, left);
        fflush(stdout);
        u.bench = false;
    }
}

static void erase(lv_timer_t *t)
{
    (void)t;
    muse_lock_wipe();
}

static void start_erase(void)
{
    if (u.erasing) {
        return;
    }
    u.erasing = true;
    muse_dialog_close();
    if (!u.up) {
        slide_in(false);
    }
    show_view(VIEW_ERASE);
    lv_timer_t *t = lv_timer_create(erase, ERASE_DELAY_MS, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void missed(const char *hint, uint32_t color)
{
    set_hint(hint, color);
    show_title();
    u.missed_at = u.now;
    shake();
}

/* An attempt at the current passcode, to unlock or before a change. */
static void check_current(void)
{
    muse_lock_result_t r = muse_lock_try(u.typed);
    memset(u.typed, 0, sizeof(u.typed));
    muse_lock_policy_t p;
    muse_lock_state(&p);
    int left = muse_lock_policy_left(&p);
    switch (r) {
    case MUSE_LOCK_RIGHT:
        bench_print("right", MUSE_LOCK_TRIES);
        set_hint(NULL, 0);
        if (u.purpose == DO_UNLOCK) {
            slide_out(true);
            muse_state_make_happy();   /* a hop, on the face as the keypad goes */
        } else if (u.purpose == DO_OFF) {
            muse_lock_clear_pin();
            finish(true);
        } else {
            u.step = STEP_CHOOSE;
            clear_typed();
            show_title();
        }
        break;
    case MUSE_LOCK_WRONG:
        bench_print("wrong", left);
        char h[64];
        if (muse_lock_policy_final(&p)) {
            snprintf(h, sizeof(h), left == 1 ? "Wrong passcode: 1 more erases it" : "Wrong passcode: %d more erase it",
                     left);
            missed(h, MUSE_COLOR_DANGER);
        } else {
            snprintf(h, sizeof(h), left == 1 ? "Wrong passcode, 1 try left" : "Wrong passcode, %d tries left", left);
            missed(h, MUSE_COLOR_WARN);
        }
        break;
    case MUSE_LOCK_LOCKOUT:
    case MUSE_LOCK_REFUSED:
        bench_print(r == MUSE_LOCK_LOCKOUT ? "lockout" : "refused", 0);
        if (u.done) {
            muse_lock_ui_done_t done = u.done;
            u.done = NULL;
            done(false);
        }
        u.purpose = DO_UNLOCK;   /* it's locked now */
        set_hint(NULL, 0);
        u.missed_at = u.now;
        u.warned = false;
        clear_typed();
        show_view(VIEW_LOCKOUT);
        break;
    case MUSE_LOCK_WIPE:
        bench_print("wipe", 0);
        start_erase();
        break;
    }
}

static void save_new(void)
{
    bool ok = muse_lock_set_pin(u.first);
    memset(u.first, 0, sizeof(u.first));
    clear_typed();
    if (ok) {
        finish(true);
    } else {
        u.step = STEP_CHOOSE;
        set_hint("Couldn't save it. Try again", MUSE_COLOR_WARN);
        show_title();
    }
}

static void check(lv_timer_t *t)
{
    (void)t;
    if (!u.checking) {
        return;
    }
    if (u.purpose == DO_UNLOCK || u.step == STEP_CURRENT) {
        check_current();
    } else if (u.step == STEP_CHOOSE) {
        memcpy(u.first, u.typed, sizeof(u.first));
        u.step = STEP_CONFIRM;
        set_hint(NULL, 0);
        clear_typed();
        show_title();
    } else if (!strcmp(u.first, u.typed)) {
        save_new();
    } else {
        memset(u.first, 0, sizeof(u.first));
        u.step = STEP_CHOOSE;
        set_hint("They didn't match. Choose again", MUSE_COLOR_WARN);
        show_title();
        shake();
    }
    u.checking = false;
}

/* Six digits in: checked a frame later, with the last dot drawn. */
static void submit(void)
{
    u.checking = true;
    lv_timer_t *t = lv_timer_create(check, CHECK_DELAY_MS, NULL);
    lv_timer_set_repeat_count(t, 1);
}

/* ---- Keys ---- */

static void on_key(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    u.touched_at = u.now;
    muse_state_poke();
    if (u.checking || u.view != VIEW_KEYS || lv_anim_get(u.dots_row, NULL)) {
        return;
    }
    if (k == KEY_DEL) {
        if (u.n) {
            u.typed[--u.n] = '\0';
            show_dots();
        }
        return;
    }
    if (k == KEY_SLOT) {
        if (u.purpose != DO_UNLOCK) {
            finish(false);   /* Cancel */
        }
        return;
    }
    if (u.n >= MUSE_LOCK_PIN_LEN) {
        return;
    }
    if (!u.n && u.hint[0] && u.hint_color != MUSE_COLOR_DANGER) {
        set_hint(NULL, 0);   /* typing again: the plain title back */
        show_title();
    }
    u.typed[u.n++] = (char)('0' + k);
    show_dots();
    if (u.n == MUSE_LOCK_PIN_LEN) {
        submit();
    }
}

static void on_gesture(lv_event_t *e)
{
    (void)e;
    if (lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_BOTTOM || u.checking || u.erasing) {
        return;
    }
    lv_indev_wait_release(lv_indev_active());
    if (u.purpose == DO_UNLOCK) {
        slide_out(true);
    } else {
        finish(false);
    }
}

/* A round key, the widget sheet's round button's look; pressed, it lights and gives a little. */
static lv_obj_t *key(lv_obj_t *box, int k, int col, int row)
{
    lv_obj_t *b = lv_button_create(box);
    lv_obj_remove_style_all(b);
    int d = px(KEY_D);
    lv_obj_set_size(b, d, d);
    lv_obj_set_pos(b, col * (d + px(KEY_GAP_X)), row * (d + px(KEY_GAP_Y)));
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, on_key, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    lv_obj_set_style_transform_width(b, -px(5), LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(b, -px(5), LV_STATE_PRESSED);
    lv_obj_set_style_transition(b, &u.press_tr, 0);
    lv_obj_set_style_transition(b, &u.press_tr, LV_STATE_PRESSED);
    if (k <= 9) {
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(MUSE_COLOR_CARD_PRESSED), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(MUSE_COLOR_RAISED_PRESSED), LV_STATE_PRESSED);
        char t[2] = { (char)('0' + k), 0 };
        lv_obj_center(muse_style_label(b, u.scale >= 900 ? FONT_DIGIT : &lv_font_montserrat_28, MUSE_COLOR_TEXT, t));
    }
    return b;
}

static void build_keys(lv_obj_t *sheet)
{
    int d = px(KEY_D);
    lv_obj_t *box = lv_obj_create(sheet);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 3 * d + 2 * px(KEY_GAP_X), 4 * d + 3 * px(KEY_GAP_Y));
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -px(KEYS_BOTTOM));
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    for (int k = 1; k <= 9; k++) {
        u.keys[k] = key(box, k, (k - 1) % 3, (k - 1) / 3);
    }
    u.keys[0] = key(box, 0, 1, 3);
    u.keys[KEY_SLOT] = key(box, KEY_SLOT, 0, 3);
    u.keys[KEY_DEL] = key(box, KEY_DEL, 2, 3);
    lv_obj_set_style_text_color(u.keys[KEY_DEL], lv_color_hex(MUSE_COLOR_ACCENT_PRESSED), LV_STATE_PRESSED);
    lv_obj_center(muse_style_label(u.keys[KEY_DEL], MUSE_FONT_ROW, MUSE_COLOR_TEXT, LV_SYMBOL_BACKSPACE));
    lv_obj_set_style_text_color(lv_obj_get_child(u.keys[KEY_DEL], 0), lv_color_hex(MUSE_COLOR_ACCENT_PRESSED),
                                LV_STATE_PRESSED);
    /* The bottom left: Cancel in Settings, Muse when unlocking. */
    u.cancel = muse_style_label(u.keys[KEY_SLOT], MUSE_FONT_BUTTON, MUSE_COLOR_TEXT, "Cancel");
    lv_obj_center(u.cancel);
    u.avatar = lv_image_create(box);
    int a = px(AVATAR_PX);
    lv_obj_set_size(u.avatar, a, a);
    /* His feet on the keys' bottom line: the grid's last rows are blank. */
    lv_obj_align_to(u.avatar, u.keys[KEY_SLOT], LV_ALIGN_BOTTOM_MID, 0, a / 16);
    lv_obj_remove_flag(u.avatar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_to_index(u.avatar, 0);   /* under the keys: his square's black corners don't cover 7 */
}

void muse_lock_ui_build(lv_obj_t *layer, int w, int h)
{
    u.w = w;
    u.h = h;
    int side = w < h ? w : h;
    u.scale = side * 1000 / 480;
    if (muse_board->round) {
        u.scale = u.scale * 82 / 100;   /* the corners' keys inside the circle */
    }
    u.scale = u.scale > 1000 ? 1000 : u.scale;
    static const lv_style_prop_t PROPS[] = { LV_STYLE_TRANSFORM_WIDTH, LV_STYLE_TRANSFORM_HEIGHT, LV_STYLE_BG_COLOR, 0 };
    lv_style_transition_dsc_init(&u.press_tr, PROPS, lv_anim_path_ease_out, 120, 0, NULL);

    u.sheet = lv_obj_create(layer);
    lv_obj_remove_style_all(u.sheet);
    lv_obj_set_size(u.sheet, w, h);
    lv_obj_set_style_bg_color(u.sheet, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(u.sheet, LV_OPA_COVER, 0);
    lv_obj_remove_flag(u.sheet, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(u.sheet, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);   /* taps stop here */
    lv_obj_add_event_cb(u.sheet, on_gesture, LV_EVENT_GESTURE, NULL);

    u.title = muse_style_label(u.sheet, MUSE_FONT_CARD_TITLE, MUSE_COLOR_TEXT, "");
    lv_obj_set_width(u.title, w - 40);
    lv_obj_set_style_text_align(u.title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(u.title, LV_LABEL_LONG_MODE_DOTS);

    u.dots_row = lv_obj_create(u.sheet);
    lv_obj_remove_style_all(u.dots_row);
    int dd = px(DOT_D) < 10 ? 10 : px(DOT_D);
    lv_obj_set_size(u.dots_row, MUSE_LOCK_PIN_LEN * dd + (MUSE_LOCK_PIN_LEN - 1) * px(DOT_GAP), dd);
    lv_obj_align(u.dots_row, LV_ALIGN_TOP_MID, 0, px(DOTS_Y));
    lv_obj_set_flex_flow(u.dots_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(u.dots_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(u.dots_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < MUSE_LOCK_PIN_LEN; i++) {
        lv_obj_t *d = lv_obj_create(u.dots_row);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, dd, dd);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(d, MUSE_CARD_BORDER, 0);
        lv_obj_set_style_border_color(d, lv_color_hex(MUSE_COLOR_ACCENT), 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(MUSE_COLOR_ACCENT), 0);
        u.dots[i] = d;
    }

    u.note = muse_style_label(u.sheet, MUSE_FONT_NOTE, MUSE_COLOR_DIM, "");
    lv_obj_set_width(u.note, w - 80);
    lv_obj_set_style_text_align(u.note, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(u.note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_line_space(u.note, 2, 0);

    build_keys(u.sheet);
    begin(DO_UNLOCK, STEP_CURRENT, NULL);
}

/* ---- Public ---- */

void muse_lock_ui_open(bool animate)
{
    if (!u.sheet || (u.up && u.purpose == DO_UNLOCK)) {
        return;
    }
    if (u.up && u.done) {
        finish(false);   /* a Settings flow, overtaken by the lock */
    }
    begin(DO_UNLOCK, STEP_CURRENT, NULL);
    muse_lock_policy_t p;
    muse_lock_state(&p);
    if (!muse_lock_policy_can_try(&p)) {
        show_view(VIEW_LOCKOUT);
    }
    slide_in(animate);
}

void muse_lock_ui_close(void)
{
    if (u.up && !u.erasing && !u.checking) {
        if (u.purpose == DO_UNLOCK) {
            slide_out(false);
        } else {
            finish(false);
        }
    }
}

bool muse_lock_ui_up(void)
{
    return u.sheet && !lv_obj_has_flag(u.sheet, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *muse_lock_ui_avatar(void)
{
    return muse_lock_ui_up() && u.purpose == DO_UNLOCK && u.view != VIEW_ERASE ? u.avatar : NULL;
}

void muse_lock_ui_mood(float now, float *dizzy, float *tired)
{
    float since = now - u.missed_at;
    *dizzy = u.missed_at > 0 && since >= 0 && since < MISS_S ? 1.0f - since / MISS_S : 0.0f;
    *tired = u.view == VIEW_LOCKOUT ? 0.5f : u.n ? 0.85f : 0.0f;
}

static void flow(purpose_t purpose, step_t step, muse_lock_ui_done_t done)
{
    if (!u.sheet || muse_lock_locked()) {
        if (done) {
            done(false);
        }
        return;
    }
    begin(purpose, step, done);
    slide_in(true);
}

void muse_lock_ui_set_pin(muse_lock_ui_done_t done)
{
    flow(DO_SET, STEP_CHOOSE, done);
}

void muse_lock_ui_change_pin(muse_lock_ui_done_t done)
{
    flow(DO_CHANGE, STEP_CURRENT, done);
}

void muse_lock_ui_turn_off(muse_lock_ui_done_t done)
{
    flow(DO_OFF, STEP_CURRENT, done);
}

/* ">pin=": as if typed, through the same check and counting. */
static void bench(void)
{
    char pin[MUSE_LOCK_PIN_LEN + 1];
    if (!muse_lock_bench_take(pin)) {
        return;
    }
    muse_lock_policy_t p;
    muse_lock_state(&p);
    const char *no = !muse_lock_locked() ? "not_locked" : u.checking || u.erasing ? "busy"
                     : !muse_lock_policy_can_try(&p) ? "refused" : NULL;
    if (no) {
        printf("@pin {\"result\":\"%s\",\"left\":%d}\n", no, muse_lock_policy_left(&p));
        fflush(stdout);
        memset(pin, 0, sizeof(pin));
        return;
    }
    if (!u.up || u.purpose != DO_UNLOCK) {
        muse_lock_ui_open(false);
    }
    memcpy(u.typed, pin, sizeof(u.typed));
    memset(pin, 0, sizeof(pin));
    u.n = (int)strlen(u.typed);
    show_dots();
    u.bench = true;
    submit();
}

void muse_lock_ui_tick(float now)
{
    if (!u.sheet) {
        return;
    }
    u.now = now;
    if (u.touched_at <= 0) {
        u.touched_at = now;
    }
    muse_lock_tick();
    bench();
    if (muse_lock_wipe_due() && !u.erasing) {
        start_erase();   /* at boot, after a power cut as it started */
    }
    if (u.erasing) {
        return;
    }
    bool locked = muse_lock_locked();
    if (u.up && u.done && locked) {
        ESP_LOGI(TAG, "locked during a Settings flow: cancelled");
        finish(false);
    }
    if (!u.up) {
        return;
    }
    if (muse_state_asleep() || (u.purpose == DO_UNLOCK && !locked && !u.checking)) {
        slide_out(false);   /* dark: the locked face to wake to; unlocked: nothing to do */
        return;
    }
    if (u.purpose != DO_UNLOCK) {
        return;
    }
    muse_lock_policy_t p;
    muse_lock_state(&p);
    if (u.view == VIEW_LOCKOUT) {
        if (p.lockout_s) {
            lockout_title();
        } else {
            set_hint(NULL, 0);
            show_view(VIEW_KEYS);
        }
    } else if (!muse_lock_policy_can_try(&p) && !u.checking) {
        show_view(VIEW_LOCKOUT);
    }
    if (u.view == VIEW_KEYS && muse_lock_policy_final(&p) && !p.failed && !u.warned && !muse_dialog_is_open()) {
        warn_final();
    }
    if (!u.checking && !muse_dialog_is_open() && now - u.touched_at > IDLE_CLOSE_S) {
        slide_out(true);   /* left alone: back to the locked face */
    }
}
