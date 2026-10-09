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

#include "muse_settings_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"

#include "muse_audio.h"
#include "muse_battery.h"
#include "muse_ble.h"
#include "muse_board.h"
#include "muse_chat.h"
#include "muse_dialog.h"
#include "muse_gadget_mode.h"
#include "muse_home_extras.h"
#include "muse_input.h"
#include "muse_keypad.h"
#include "muse_link.h"
#include "muse_lock.h"
#include "muse_lock_ui.h"
#if CONFIG_MUSE_GADGET_ALARM
#include "muse_rtc.h"
#endif
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_style.h"
#include "muse_text.h"
#include "muse_ui.h"
#include "muse_voice.h"
#include "muse_wifi.h"

#define MAX_APS 12

typedef void (*text_done_cb_t)(const char *text);

/* The screen size the text page is scaled to (see text_px). */
static int s_text_scale = 466;
static lv_obj_t *s_tile;
static lv_obj_t *s_current;
static lv_obj_t *s_home, *s_wifi, *s_hatch, *s_ble, *s_sound, *s_display, *s_battery, *s_power, *s_text;
static lv_obj_t *s_mode;
static lv_obj_t *s_advanced;   /* Muse, Bluetooth and Battery, under home */
static lv_obj_t *s_general;    /* Wi-Fi, Display, Sound and Passcode, under home */
EXT_RAM_BSS_ATTR static lv_obj_t *s_passcode;   /* under General */
static lv_obj_t *s_reset;      /* Reset device's two warnings, under Advanced */
static lv_obj_t *s_about;      /* what this device is and how it's doing, under Advanced */
static lv_obj_t *s_reset_note, *s_reset_go_lbl;
static int s_reset_step;       /* warnings agreed to so far */

/*
 * Only home is kept. A sub-page is built when it opens and deleted on the way
 * back, so only the one on screen holds RAM: all of them at once cost ~50 KB,
 * which a board without PSRAM needs for Link's session.
 */
typedef struct {
    lv_obj_t **obj;
    void (*build)(lv_obj_t *tile);
} page_t;

/* Home values. */
static lv_obj_t *s_home_wifi, *s_home_sound, *s_home_display;
static lv_obj_t *s_home_mode;
EXT_RAM_BSS_ATTR static lv_obj_t *s_home_passcode;

/* Passcode page. */
EXT_RAM_BSS_ATTR static lv_obj_t *s_pass_sw, *s_pass_more, *s_pass_after[2];
EXT_RAM_BSS_ATTR static int s_pass_shown;   /* has a PIN, as shown; -1 to show again */

/* Advanced values. */
static lv_obj_t *s_adv_hatch, *s_adv_ble, *s_adv_battery;

/* About page. */
static lv_obj_t *s_about_ip, *s_about_wifi, *s_about_signal, *s_about_uptime, *s_about_mem;

/* Wi-Fi page. */
static lv_obj_t *s_wifi_sw, *s_wifi_status, *s_wifi_saved, *s_wifi_scan_btn, *s_wifi_scan_lbl, *s_wifi_list;
static uint32_t s_shown_scan_gen = UINT32_MAX;
static muse_wifi_ap_t s_aps[MAX_APS];
static muse_wifi_saved_t s_saved[MUSE_WIFI_SAVED_MAX];
static lv_obj_t *s_saved_vals[MUSE_WIFI_SAVED_MAX];
static int s_saved_n = -1;   /* as listed; -1 before the first fill */
static int s_forget_armed = -1;
static int64_t s_forget_armed_us;
static char s_join_ssid[MUSE_SSID_MAX + 1];

/* Hatch page. */
static lv_obj_t *s_hatch_status, *s_hatch_host, *s_hatch_vm, *s_hatch_token;
static lv_obj_t *s_link_status, *s_link_reset_lbl;
static int64_t s_link_reset_armed_us;

/* Bluetooth page. */
static lv_obj_t *s_ble_sw, *s_ble_status;

/* Sound page. */
static lv_obj_t *s_spk_sw, *s_vol_val, *s_vol_sl, *s_gain_val, *s_gain_sl, *s_mic_bar, *s_mic_val;

/* Display page: brightness, and when the screen sleeps. */
static lv_obj_t *s_bright_val, *s_bright_sl;
static const int SLEEP_CHOICES[] = { 0, 30, 60, 120, 300, 600 };
static const char *const SLEEP_NAMES[] = { "Never", "30 seconds", "1 minute", "2 minutes", "5 minutes", "10 minutes" };
#define SLEEP_COUNT (int)(sizeof(SLEEP_CHOICES) / sizeof(SLEEP_CHOICES[0]))
static lv_obj_t *s_sleep_checks[SLEEP_COUNT];

/* Mode */
static lv_obj_t *s_mode_home, *s_mode_away;
static muse_wifi_saved_t s_mode_nets[MUSE_WIFI_SAVED_MAX];   /* the Wi-Fi dialog's, while it's open */

/* Battery page. */
static lv_obj_t *s_batt_status, *s_batt_level, *s_batt_drain, *s_batt_full, *s_batt_off, *s_batt_slept, *s_batt_wakes,
    *s_batt_busy, *s_batt_awake;
static int64_t s_batt_shown_us;

/* Text entry page. */
static lv_obj_t *s_text_title, *s_text_ta;
static lv_obj_t *s_text_kp;                 /* a screen under 2" */
static lv_obj_t *s_text_kb, *s_text_show;   /* a bigger one */
static text_done_cb_t s_text_done;
static lv_obj_t *s_text_back;

/* ---------- building blocks ---------- */

/* Network and phone names can have characters the fonts lack (muse_text.h). */
#define SHOWN_MAX 96

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    char shown[SHOWN_MAX];
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, muse_text_showable(text, shown, sizeof(shown)));
    return l;
}

static void set_text(lv_obj_t *l, const char *text)
{
    char shown[SHOWN_MAX];
    text = muse_text_showable(text, shown, sizeof(shown));
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

static lv_obj_t *note(lv_obj_t *list, const char *text)
{
    lv_obj_t *l = label(list, MUSE_FONT_NOTE, MUSE_COLOR_DIM, text);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

static void go_back(void);

static void on_gesture(lv_event_t *e)
{
    (void)e;
    /* Dragging a slider right isn't a "back" swipe. */
    lv_obj_t *pressed = lv_indev_get_active_obj();
    if (pressed && lv_obj_check_type(pressed, &lv_slider_class)) {
        return;
    }
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
        lv_indev_wait_release(lv_indev_active());
        go_back();
    }
}

/* Swipe right on a page to go back. By default a swipe bubbles up past the
 * page to the screen, so the page has to stop it. */
static void catch_swipes(lv_obj_t *p)
{
    lv_obj_remove_flag(p, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(p, on_gesture, LV_EVENT_GESTURE, NULL);
}

static void on_back(lv_event_t *e)
{
    (void)e;
    go_back();
}

/* The text page's arrow, by its title: the keys take the bottom, where the
 * other pages have back_row(). */
static lv_obj_t *back_button(lv_obj_t *p)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 56, 48);
    lv_obj_align(b, LV_ALIGN_TOP_MID, -112, 28);
    lv_obj_add_event_cb(b, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *arrow = label(b, &lv_font_montserrat_20, MUSE_COLOR_ACCENT, LV_SYMBOL_LEFT);
    lv_obj_center(arrow);
    return b;
}

/* A page: title and a vertically scrolling column; a sub-page ends it with back_row(). */
static lv_obj_t *page(lv_obj_t *tile, const char *title, lv_obj_t **list_out)
{
    /* Flat short panels, and ones too narrow for MUSE_LIST_W, get tighter rows. */
    const bool compact = !muse_board->round && (muse_board->height <= 240 || muse_board->width < MUSE_LIST_W);
    const int list_top = compact ? 36 : MUSE_LIST_TOP;
    /* Room either side of the rows; the scrollbar runs down the right one. */
    const int gutter = compact ? 4 : MUSE_LIST_GUTTER;
    lv_obj_t *p = lv_obj_create(tile);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    catch_swipes(p);

    if (compact) {
        lv_obj_align(label(p, &lv_font_montserrat_16, MUSE_COLOR_ACCENT, title), LV_ALIGN_TOP_MID, 0, 8);
    } else {
        muse_style_title(p, title);
    }

    lv_obj_t *list = lv_obj_create(p);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, (compact ? muse_board->width - 16 : MUSE_LIST_W) + 2 * gutter, muse_board->height - list_top);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, list_top);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, compact ? 10 : MUSE_ROW_GAP, 0);
    lv_obj_set_style_pad_hor(list, gutter, 0);
    /* Clear the page dots (8 px, 14 px up) on flat panels, or the bottom
     * curve on round ones; no more, or a page that fits would scroll. */
    lv_obj_set_style_pad_bottom(list, muse_board->round ? 110 : compact ? 32 : MUSE_LIST_PAD_BOTTOM, 0);
    /* Its scrollbar in the right gutter, short of the list's ends (and of a
     * round screen's bottom curve), as on the Chats screen. */
    muse_style_scroll_column(list, compact ? 4 : 6, compact ? 0 : 3, 8, muse_board->round ? 90 : 16);
    *list_out = list;
    return p;
}

/* A row's card (muse_style_row), on the page's black. */
static lv_obj_t *card(lv_obj_t *list, bool clickable)
{
    return muse_style_row(list, clickable, false);
}

/* Tappable row: icon, text, right-aligned value. */
static lv_obj_t *row(lv_obj_t *list, const char *icon, const char *text, lv_obj_t **value_out,
                     lv_event_cb_t cb, void *user)
{
    lv_obj_t *c = card(list, true);
    if (icon) {
        label(c, MUSE_FONT_ROW, MUSE_COLOR_ACCENT, icon);
    }
    lv_obj_t *t = label(c, MUSE_FONT_ROW, MUSE_COLOR_TEXT, text);
    lv_obj_set_flex_grow(t, 1);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    if (value_out) {
        lv_obj_t *v = label(c, MUSE_FONT_VALUE, MUSE_COLOR_DIM, "");
        lv_obj_set_style_max_width(v, 130, 0);
        lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
        *value_out = v;
    }
    lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, user);
    return c;
}

static lv_obj_t *switch_row(lv_obj_t *list, const char *text, bool on, lv_event_cb_t cb)
{
    lv_obj_t *c = card(list, false);
    lv_obj_t *t = label(c, MUSE_FONT_ROW, MUSE_COLOR_TEXT, text);
    lv_obj_set_flex_grow(t, 1);
    lv_obj_t *sw = lv_switch_create(c);
    lv_obj_set_size(sw, 60, 32);
    lv_obj_set_ext_click_area(sw, 12);   /* 56 px to hit, inside the row */
    lv_obj_set_style_bg_color(sw, lv_color_hex(MUSE_COLOR_OFF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(MUSE_COLOR_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

/* A row-sized button (muse_style_button): filled for an action, outlined for
 * the way back. */
static lv_obj_t *button(lv_obj_t *list, const char *text, muse_button_kind_t kind, lv_event_cb_t cb,
                        lv_obj_t **label_out)
{
    lv_obj_t *b = muse_style_button(list, text, kind, MUSE_FONT_ROW, MUSE_ROW_H);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    if (label_out) {
        *label_out = lv_obj_get_child(b, 0);
    }
    return b;
}

/* The way back, last on every sub-page and the same on each: full width and
 * taller than a row, so it's easy to find and hit on a small screen. */
static lv_obj_t *back_row(lv_obj_t *list, const char *text)
{
    char buf[32];
    snprintf(buf, sizeof(buf), LV_SYMBOL_LEFT "  %s", text);
    lv_obj_t *b = button(list, buf, MUSE_BUTTON_NEUTRAL, on_back, NULL);
    lv_obj_set_height(b, MUSE_ROW_H + 6);
    return b;
}

/* Label + value on one line, slider below, in line with the rows' text. */
static lv_obj_t *slider(lv_obj_t *list, const char *text, int lo, int hi, int value, lv_obj_t **value_out,
                        lv_event_cb_t cb)
{
    lv_obj_t *c = lv_obj_create(list);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(c, MUSE_ROW_PAD, 0);
    lv_obj_set_style_pad_ver(c, 6, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    label(c, MUSE_FONT_ROW, MUSE_COLOR_TEXT, text);
    lv_obj_t *v = label(c, &lv_font_montserrat_20, MUSE_COLOR_ACCENT, "");
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, 0);
    *value_out = v;

    lv_obj_t *s = lv_slider_create(c);
    lv_obj_set_width(s, lv_pct(94));
    lv_obj_set_height(s, 12);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 40);
    lv_slider_set_range(s, lo, hi);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, lv_color_hex(MUSE_COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(MUSE_COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_hex(MUSE_COLOR_TEXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 18);   /* 48 px to hit */
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *spacer = lv_obj_create(c);   /* room below the slider */
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_align(spacer, LV_ALIGN_TOP_LEFT, 0, 64);
    return s;
}

/* A column of rows inside the page's list, filled in later. */
static lv_obj_t *column(lv_obj_t *list)
{
    lv_obj_t *l = lv_obj_create(list);
    lv_obj_remove_style_all(l);
    lv_obj_set_size(l, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(l, MUSE_ROW_GAP, 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    return l;
}

/* Label and value, not tappable. */
static lv_obj_t *info_row(lv_obj_t *list, const char *text)
{
    lv_obj_t *c = card(list, false);
    lv_obj_set_height(c, MUSE_INFO_H);
    lv_obj_t *t = label(c, MUSE_FONT_VALUE, MUSE_COLOR_TEXT, text);
    lv_obj_set_flex_grow(t, 1);
    return label(c, MUSE_FONT_VALUE, MUSE_COLOR_ACCENT, "");
}

/* ---------- navigation ---------- */

static void drop(lv_obj_t *p)
{
    lv_obj_t **const pages[] = { &s_wifi, &s_hatch, &s_ble, &s_sound, &s_display, &s_battery, &s_power, &s_text,
                                 &s_mode, &s_advanced, &s_general, &s_reset, &s_about, &s_passcode };
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        if (*pages[i] == p) {
            *pages[i] = NULL;
        }
    }
    lv_obj_delete_async(p);   /* we may be in one of its own events */
}

/* The page a page's Back goes to: the text page's opener, General's
 * or Advanced's own pages', or home. */
static lv_obj_t *parent(lv_obj_t *p)
{
    if (p == s_text) {
        return s_text_back;
    }
    if (p && (p == s_hatch || p == s_ble || p == s_battery || p == s_reset || p == s_about)) {
        return s_advanced;
    }
    if (p && (p == s_wifi || p == s_sound || p == s_display || p == s_passcode)) {
        return s_general;
    }
    return s_home;
}

static void show(lv_obj_t *p)
{
    lv_obj_t *prev = s_current;
    if (prev == p) {
        return;
    }
    if (prev) {
        lv_obj_add_flag(prev, LV_OBJ_FLAG_HIDDEN);
    }
    muse_dialog_close_on(s_tile);   /* it was about the page left */
    if (prev == s_sound) {
        muse_voice_set_monitor(false);
    }
    lv_obj_remove_flag(p, LV_OBJ_FLAG_HIDDEN);
    s_current = p;
    if (p == s_sound) {
        muse_voice_set_monitor(true);
    }
    muse_ui_set_swipe_enabled(p == s_home);
    /* Only the way down to the page on screen is kept: going into a page
     * keeps the one it opened from (Advanced, or what the text page is for),
     * and going back drops the one left. Home is always kept. */
    if (prev && prev != s_home && parent(p) != prev) {
        drop(prev);
    }
}

static void close_text(void);

static void go_back(void)
{
    if (s_current == s_text) {
        close_text();
    } else if (s_current != s_home) {
        show(parent(s_current));
    }
}

static void on_nav(lv_event_t *e)
{
    const page_t *page = lv_event_get_user_data(e);
    if (!*page->obj) {
        page->build(s_tile);
    }
    show(*page->obj);
    muse_settings_ui_tick(true);   /* fill it in now, not at the next tick */
}

/* ---------- text entry ---------- */

/* Laid out for a 466 px screen; a smaller round one scales it about its centre. */
static int text_px(int v)
{
    return v * s_text_scale / 466;
}

static int text_y(int y)
{
    return muse_board->height / 2 + text_px(y - 233);
}

static void close_text(void)
{
    lv_textarea_set_text(s_text_ta, "");   /* don't leave secrets in the widget */
    show(s_text_back);
}

static void on_text_ready(lv_event_t *e)
{
    (void)e;
    text_done_cb_t done = s_text_done;
    char *text = strdup(lv_textarea_get_text(s_text_ta));
    close_text();
    if (done && text) {
        done(text);
    }
    if (text) {
        memset(text, 0, strlen(text));
        free(text);
    }
}

static void on_text_cancel(lv_event_t *e)
{
    (void)e;
    close_text();
}

static void on_text_show(lv_event_t *e)
{
    (void)e;
    bool pw = !lv_textarea_get_password_mode(s_text_ta);
    lv_textarea_set_password_mode(s_text_ta, pw);
    lv_label_set_text(lv_obj_get_child(s_text_show, 0), pw ? "Show" : "Hide");
}

/* Beside the keyboard's field, a button to show a password. */
static void fit_show_button(bool password)
{
    int w = text_px(300), show_w = 70, gap = 8;
    lv_obj_set_width(s_text_ta, password ? w - show_w - gap : w);
    lv_obj_align(s_text_ta, LV_ALIGN_TOP_MID, password ? -(show_w + gap) / 2 : 0, text_y(76));
    lv_obj_align(s_text_show, LV_ALIGN_TOP_MID, (w - show_w) / 2, text_y(76));
    lv_label_set_text(lv_obj_get_child(s_text_show, 0), "Show");
    if (password) {
        lv_obj_remove_flag(s_text_show, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_text_show, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_keyboard(void)
{
    s_text_show = lv_button_create(s_text);
    lv_obj_remove_style_all(s_text_show);
    lv_obj_set_size(s_text_show, 70, text_px(48));
    lv_obj_set_style_radius(s_text_show, 14, 0);
    lv_obj_set_style_bg_opa(s_text_show, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_text_show, lv_color_hex(MUSE_COLOR_CARD), 0);
    lv_obj_add_event_cb(s_text_show, on_text_show, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label(s_text_show, &lv_font_montserrat_16, MUSE_COLOR_TEXT, "Show"));

    /* Inside the circle, or across the rest of the screen. */
    s_text_kb = lv_keyboard_create(s_text);
    int y = text_y(136);
    if (muse_board->round) {
        lv_obj_set_size(s_text_kb, text_px(384), text_px(206));
    } else {
        int w = muse_board->width - 16;
        int h = muse_board->height - y - 8;
        lv_obj_set_size(s_text_kb, w, LV_MIN(h, w * 206 / 384));
    }
    lv_obj_align(s_text_kb, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_opa(s_text_kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_text_kb, 2, 0);
    lv_obj_set_style_pad_gap(s_text_kb, 4, 0);
    lv_obj_set_style_text_font(s_text_kb, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_text_kb, lv_color_hex(MUSE_COLOR_CARD), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_text_kb, lv_color_hex(MUSE_COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_radius(s_text_kb, 8, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_text_kb, 0, LV_PART_ITEMS);
    lv_obj_remove_flag(s_text_kb, LV_OBJ_FLAG_GESTURE_BUBBLE);   /* a sloppy swipe mustn't lose the text */
    lv_keyboard_set_textarea(s_text_kb, s_text_ta);
    lv_obj_add_event_cb(s_text_kb, on_text_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_text_kb, on_text_cancel, LV_EVENT_CANCEL, NULL);
}

static void build_text_page(lv_obj_t *tile)
{
    s_text = lv_obj_create(tile);
    lv_obj_remove_style_all(s_text);
    lv_obj_set_size(s_text, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(s_text, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_text, LV_OBJ_FLAG_HIDDEN);
    catch_swipes(s_text);
    lv_obj_t *back = back_button(s_text);

    /* Between the back arrow and its mirror image. */
    s_text_title = label(s_text, &lv_font_montserrat_20, MUSE_COLOR_ACCENT, "");
    lv_obj_set_width(s_text_title, 150);
    lv_obj_set_style_text_align(s_text_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_text_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_text_title, LV_ALIGN_TOP_MID, 0, 40);

    const lv_font_t *font = &lv_font_montserrat_20;
    int h = text_px(48), border = 2;
    int pad = (h - 2 * border - lv_font_get_line_height(font)) / 2;
    s_text_ta = lv_textarea_create(s_text);
    lv_textarea_set_one_line(s_text_ta, true);
    lv_obj_set_size(s_text_ta, text_px(300), h);
    lv_obj_set_style_text_font(s_text_ta, font, 0);
    lv_obj_set_style_pad_ver(s_text_ta, pad > 0 ? pad : 0, 0);
    lv_obj_set_style_pad_hor(s_text_ta, 14, 0);
    lv_obj_set_style_bg_color(s_text_ta, lv_color_hex(MUSE_COLOR_CARD), 0);
    lv_obj_set_style_text_color(s_text_ta, lv_color_hex(MUSE_COLOR_TEXT), 0);
    lv_obj_set_style_border_color(s_text_ta, lv_color_hex(MUSE_COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(s_text_ta, border, 0);
    lv_obj_set_style_radius(s_text_ta, 14, 0);
    lv_obj_set_style_text_font(s_text_ta, &lv_font_montserrat_16, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_color(s_text_ta, lv_color_hex(MUSE_COLOR_DIM), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_state(s_text_ta, LV_STATE_FOCUSED);
    lv_obj_align(s_text_ta, LV_ALIGN_TOP_MID, 0, text_y(76));

    /* A full keyboard's keys are too small to hit on a screen under 2". */
    if (muse_board->diagonal_in >= 2.0f) {
        build_keyboard();
        return;
    }

    /* The keys out to the screen's edges, and the rest squeezed in above them:
     * the back arrow beside the field, the title over both. */
    lv_obj_align(s_text_title, LV_ALIGN_TOP_MID, 0, text_y(16));
    lv_obj_set_width(s_text_title, text_px(180));
    lv_obj_set_width(s_text_ta, text_px(224));
    lv_obj_align(s_text_ta, LV_ALIGN_TOP_MID, 0, text_y(44));
    lv_obj_set_size(back, text_px(48), h);
    lv_obj_align(back, LV_ALIGN_TOP_MID, -text_px(140), text_y(44));
    s_text_kp = muse_keypad_create(s_text, s_text_ta, muse_board->round);
    int top = text_y(98);
    lv_obj_set_size(s_text_kp, muse_board->width, muse_board->height - top);
    lv_obj_align(s_text_kp, LV_ALIGN_TOP_MID, 0, top);
    if (muse_board->round) {
        /* Lifts the bottom row's labels inside the circle. The button matrix
         * stretches edge keys' taps over padding up to LV_DPI_DEF / 10, and not
         * at all over more, so this much keeps them reaching the edge. */
        lv_obj_set_style_pad_bottom(s_text_kp, LV_DPI_DEF / 10, 0);
    }
    lv_obj_add_event_cb(s_text_kp, on_text_ready, LV_EVENT_READY, NULL);
}

/* The hint shows in the empty field, so keep it short. */
static void open_text(const char *title, const char *initial, bool password, int max_len, const char *hint,
                      text_done_cb_t done, lv_obj_t *back)
{
    if (!s_text) {
        build_text_page(s_tile);
    }
    set_text(s_text_title, title);
    lv_textarea_set_max_length(s_text_ta, max_len);
    lv_textarea_set_password_mode(s_text_ta, password);
    lv_textarea_set_text(s_text_ta, initial ? initial : "");
    lv_textarea_set_placeholder_text(s_text_ta, hint ? hint : "");
    if (s_text_kp) {
        muse_keypad_reset(s_text_kp, password);
    } else {
        lv_keyboard_set_mode(s_text_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        fit_show_button(password);
    }
    s_text_done = done;
    s_text_back = back;
    show(s_text);
}

/* ---------- Wi-Fi ---------- */

static void on_wifi_sw(lv_event_t *e)
{
    muse_settings_set_wifi_on(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void on_wifi_scan(lv_event_t *e)
{
    (void)e;
    muse_wifi_scan();
}

static void on_wifi_pass(const char *pass)
{
    muse_settings_set_wifi(s_join_ssid, pass);
}

static bool is_saved(const char *ssid)
{
    for (int i = 0; i < s_saved_n; i++) {
        if (!strcmp(s_saved[i].ssid, ssid)) {
            return true;
        }
    }
    return false;
}

/* A saved network asks again too, in case its password changed. */
static void on_wifi_ap(lv_event_t *e)
{
    const muse_wifi_ap_t *ap = &s_aps[(int)(intptr_t)lv_event_get_user_data(e)];
    strlcpy(s_join_ssid, ap->ssid, sizeof(s_join_ssid));
    if (ap->secure) {
        open_text(ap->ssid, "", true, MUSE_PASS_MAX, "Password", on_wifi_pass, s_wifi);
    } else {
        muse_settings_set_wifi(s_join_ssid, "");
    }
}

static void on_other_ssid(const char *ssid)
{
    if (!ssid[0]) {
        return;
    }
    strlcpy(s_join_ssid, ssid, sizeof(s_join_ssid));
    open_text(ssid, "", true, MUSE_PASS_MAX, "Empty if it's open", on_wifi_pass, s_wifi);
}

static void on_wifi_other(lv_event_t *e)
{
    (void)e;
    open_text("Other network", "", false, MUSE_SSID_MAX, "Network name", on_other_ssid, s_wifi);
}

/* Two taps within a few seconds forget a saved network. */
static void on_wifi_saved(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int64_t now = esp_timer_get_time();
    if (s_forget_armed == i && now - s_forget_armed_us < 4000000) {
        muse_wifi_forget(s_saved[i].ssid);
        s_forget_armed = -1;
        return;
    }
    s_forget_armed = i;
    s_forget_armed_us = now;
}

static bool same_saved(const muse_wifi_saved_t *a, int n)
{
    if (n != s_saved_n) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        if (strcmp(a[i].ssid, s_saved[i].ssid) || a[i].hidden != s_saved[i].hidden) {
            return false;
        }
    }
    return true;
}

static void rebuild_saved_list(void)
{
    muse_wifi_saved_t saved[MUSE_WIFI_SAVED_MAX];
    int n = muse_wifi_saved(saved, MUSE_WIFI_SAVED_MAX);
    if (same_saved(saved, n)) {
        return;
    }
    memcpy(s_saved, saved, n * sizeof(saved[0]));
    s_saved_n = n;
    s_forget_armed = -1;
    s_shown_scan_gen = UINT32_MAX;   /* re-mark the saved ones in the scan list */
    lv_obj_clean(s_wifi_saved);
    if (n) {
        note(s_wifi_saved, "Saved networks");
    }
    for (int i = 0; i < n; i++) {
        row(s_wifi_saved, LV_SYMBOL_WIFI, saved[i].ssid, &s_saved_vals[i], on_wifi_saved, (void *)(intptr_t)i);
    }
}

static void tick_saved_list(const muse_wifi_status_t *w)
{
    if (s_forget_armed >= 0 && esp_timer_get_time() - s_forget_armed_us >= 4000000) {
        s_forget_armed = -1;
    }
    for (int i = 0; i < s_saved_n; i++) {
        const char *text = "";
        uint32_t color = MUSE_COLOR_DIM;
        if (i == s_forget_armed) {
            text = "Tap to forget";
            color = MUSE_COLOR_DANGER;
        } else if (w->state == MUSE_WIFI_CONNECTED && !strcmp(w->ssid, s_saved[i].ssid)) {
            text = "Connected";
            color = MUSE_COLOR_OK;
        } else if (s_saved[i].hidden) {
            text = "Hidden";
        }
        set_text(s_saved_vals[i], text);
        lv_obj_set_style_text_color(s_saved_vals[i], lv_color_hex(color), 0);
    }
}

/* Returns true when fresh results include a saved network. */
static bool rebuild_scan_list(void)
{
    uint32_t gen;
    int n = muse_wifi_scan_results(s_aps, MAX_APS, &gen);
    if (gen == s_shown_scan_gen) {
        return false;
    }
    bool fresh = s_shown_scan_gen != UINT32_MAX, saved_seen = false;
    s_shown_scan_gen = gen;
    lv_obj_clean(s_wifi_list);
    for (int i = 0; i < n; i++) {
        lv_obj_t *v;
        row(s_wifi_list, NULL, s_aps[i].ssid, &v, on_wifi_ap, (void *)(intptr_t)i);
        bool saved = is_saved(s_aps[i].ssid);
        saved_seen |= saved;
        char buf[24];
        snprintf(buf, sizeof(buf), "%s%d dBm", saved ? "Saved  " : (s_aps[i].secure ? "" : "Open  "), s_aps[i].rssi);
        lv_label_set_text(v, buf);
    }
    if (gen && !n) {
        note(s_wifi_list, "No networks found");
    }
    return fresh && saved_seen;
}

static void build_wifi_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_wifi = page(tile, "WI-FI", &list);
    s_shown_scan_gen = UINT32_MAX;   /* the lists start empty */
    s_saved_n = -1;
    s_forget_armed = -1;
    s_wifi_sw = switch_row(list, "Wi-Fi", muse_settings_wifi_on(), on_wifi_sw);
    s_wifi_status = note(list, "");

    s_wifi_saved = column(list);
    s_wifi_scan_btn = button(list, LV_SYMBOL_REFRESH "  Scan for networks", MUSE_BUTTON_ACCENT, on_wifi_scan, &s_wifi_scan_lbl);
    s_wifi_list = column(list);

    row(list, LV_SYMBOL_EDIT, "Other network...", NULL, on_wifi_other, NULL);
    lv_obj_t *mac = info_row(list, "MAC address");
    uint8_t m[6];
    if (esp_read_mac(m, ESP_MAC_WIFI_STA) == ESP_OK) {
        char buf[18];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
        lv_label_set_text(mac, buf);
    }
    note(list, "Tap a saved network twice to forget it.");
    back_row(list, "Back");
}

static void tick_wifi(void)
{
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    char buf[128];
    switch (w.state) {
    case MUSE_WIFI_OFF:
        strlcpy(buf, "Wi-Fi is off", sizeof(buf));
        break;
    case MUSE_WIFI_NO_NETWORK:
        strlcpy(buf, "No saved networks. Scan and pick one.", sizeof(buf));
        break;
    case MUSE_WIFI_CONNECTING:
        snprintf(buf, sizeof(buf), "Joining %s\n%s", w.ssid, w.detail);
        break;
    case MUSE_WIFI_CONNECTED:
        snprintf(buf, sizeof(buf), "Connected to %s\n%s  -  %d dBm", w.ssid, w.ip, w.rssi);
        break;
    case MUSE_WIFI_NOT_NEARBY:
        strlcpy(buf, "No saved network nearby\nLooking again within a minute", sizeof(buf));
        break;
    case MUSE_WIFI_FAILED:
    default:
        snprintf(buf, sizeof(buf), "Couldn't join %s\n%s", w.ssid, w.detail);
        break;
    }
    set_text(s_wifi_status, buf);
    lv_obj_set_style_text_color(s_wifi_status, lv_color_hex(w.state == MUSE_WIFI_CONNECTED ? MUSE_COLOR_OK :
                                                            w.state == MUSE_WIFI_FAILED ? MUSE_COLOR_WARN : MUSE_COLOR_DIM), 0);

    bool on = w.state != MUSE_WIFI_OFF;
    if (on != lv_obj_has_state(s_wifi_sw, LV_STATE_CHECKED)) {
        lv_obj_set_state(s_wifi_sw, LV_STATE_CHECKED, on);
    }
    lv_obj_set_flag(s_wifi_scan_btn, LV_OBJ_FLAG_HIDDEN, !on);
    lv_obj_set_flag(s_wifi_list, LV_OBJ_FLAG_HIDDEN, !on);
    set_text(s_wifi_scan_lbl, muse_wifi_scanning() ? "Scanning..." : LV_SYMBOL_REFRESH "  Scan for networks");
    rebuild_saved_list();
    tick_saved_list(&w);
    /* The scan found one: join it now rather than at the next look. */
    if (rebuild_scan_list() && w.state == MUSE_WIFI_NOT_NEARBY) {
        muse_wifi_apply();
    }
}

/* ---------- Hatch ---------- */

static void on_hatch_host_done(const char *text) { muse_settings_set_hatch_host(text); }
static void on_hatch_vm_done(const char *text) { muse_settings_set_hatch_vm(text); }

static void on_hatch_token_done(const char *text)
{
    if (text[0]) {
        muse_settings_set_hatch_token(text, false);
    }
}

static void on_hatch_host(lv_event_t *e)
{
    (void)e;
    char host[MUSE_HOST_MAX + 1];
    muse_settings_hatch_host(host);
    open_text("Muse server", host, false, MUSE_HOST_MAX, "Empty for the default", on_hatch_host_done, s_hatch);
}

static void on_hatch_vm(lv_event_t *e)
{
    (void)e;
    char vm[MUSE_VM_MAX + 1];
    muse_settings_hatch_vm(vm);
    open_text("VM ID", vm, false, MUSE_VM_MAX, "Optional", on_hatch_vm_done, s_hatch);
}

static void on_hatch_token(lv_event_t *e)
{
    (void)e;
    open_text("Device token", "", true, MUSE_TOKEN_MAX, "Empty keeps the current one", on_hatch_token_done, s_hatch);
}

static void on_hatch_test(lv_event_t *e)
{
    (void)e;
    muse_hatch_test();
}

/* Two taps within a few seconds: this wipes Wi-Fi and the Muse app pairing. */
static void on_link_reset(lv_event_t *e)
{
    (void)e;
    int64_t now = esp_timer_get_time();
    if (s_link_reset_armed_us && now - s_link_reset_armed_us < 5000000) {
        set_text(s_link_reset_lbl, "Resetting...");
        muse_link_reset_setup();
        return;
    }
    s_link_reset_armed_us = now;
    set_text(s_link_reset_lbl, "Tap again to reset");
}

static void build_hatch_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_hatch = page(tile, "MUSE", &list);
    s_link_reset_armed_us = 0;
    s_link_status = note(list, "");
    button(list, "Reset pairing", MUSE_BUTTON_DANGER, on_link_reset, &s_link_reset_lbl);
    s_hatch_status = note(list, "");
    row(list, NULL, "Server", &s_hatch_host, on_hatch_host, NULL);
    row(list, NULL, "VM ID", &s_hatch_vm, on_hatch_vm, NULL);
    row(list, NULL, "Device token", &s_hatch_token, on_hatch_token, NULL);
    button(list, "Test connection", MUSE_BUTTON_ACCENT, on_hatch_test, NULL);
    note(list, "Reset pairing forgets Wi-Fi and the app pairing, then restarts.");
    back_row(list, "Back");
}

static void tick_hatch(void)
{
    char link[64];
    snprintf(link, sizeof(link), "Muse app: %s\n%s", muse_link_hatch_linked() ? "paired" : "not paired",
             muse_link_state_name(muse_link_state()));
    set_text(s_link_status, link);
    if (s_link_reset_armed_us && esp_timer_get_time() - s_link_reset_armed_us >= 5000000) {
        s_link_reset_armed_us = 0;
        set_text(s_link_reset_lbl, "Reset pairing");
    }

    muse_hatch_status_t h;
    muse_hatch_status(&h);
    char buf[96];
    snprintf(buf, sizeof(buf), "%s\n%s", muse_hatch_state_name(h.state), h.detail);
    set_text(s_hatch_status, buf);
    lv_obj_set_style_text_color(s_hatch_status, lv_color_hex(h.state == MUSE_HATCH_REACHABLE ? MUSE_COLOR_OK :
                                                             h.state == MUSE_HATCH_UNREACHABLE ? MUSE_COLOR_WARN : MUSE_COLOR_DIM), 0);

    char host[MUSE_HOST_MAX + 1], vm[MUSE_VM_MAX + 1];
    muse_settings_hatch_host(host);
    muse_settings_hatch_vm(vm);
    set_text(s_hatch_host, host);
    set_text(s_hatch_vm, vm[0] ? vm : "Not set");
    size_t n = muse_settings_hatch_token_len();
    snprintf(buf, sizeof(buf), n ? "Set (%u chars)" : "Not set", (unsigned)n);
    set_text(s_hatch_token, buf);
}

/* ---------- Bluetooth ---------- */

static void on_ble_sw(lv_event_t *e)
{
    muse_settings_set_ble_on(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void on_ble_forget(lv_event_t *e)
{
    (void)e;
    muse_ble_forget_all();
}

static void build_ble_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_ble = page(tile, "BLUETOOTH", &list);
    s_ble_sw = switch_row(list, "Phone setup", muse_settings_ble_on(), on_ble_sw);
    s_ble_status = note(list, "");
    button(list, "Forget paired phones", MUSE_BUTTON_DANGER, on_ble_forget, NULL);
    note(list, "To pair, open tools/ble_setup.html in Chrome, connect, and enter the code Muse shows.");
    back_row(list, "Back");
}

static void tick_ble(void)
{
    muse_ble_status_t b;
    muse_ble_status(&b);
    char buf[96];
    switch (b.state) {
    case MUSE_BLE_OFF:
        strlcpy(buf, "Off", sizeof(buf));
        break;
    case MUSE_BLE_ADVERTISING:
        snprintf(buf, sizeof(buf), "Visible as %s", b.name);
        break;
    case MUSE_BLE_CONNECTED:
    default:
        snprintf(buf, sizeof(buf), "Phone connected\n%s", b.secure ? "Paired" : "Waiting for pairing");
        break;
    }
    set_text(s_ble_status, buf);
    lv_obj_set_style_text_color(s_ble_status, lv_color_hex(b.state == MUSE_BLE_CONNECTED && b.secure ? MUSE_COLOR_OK : MUSE_COLOR_DIM), 0);
    bool on = muse_settings_ble_on();
    if (on != lv_obj_has_state(s_ble_sw, LV_STATE_CHECKED)) {
        lv_obj_set_state(s_ble_sw, LV_STATE_CHECKED, on);
    }
}

/* ---------- Sound ---------- */

static void set_val(lv_obj_t *l, const char *fmt, int v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), fmt, v);
    set_text(l, buf);
}

static void on_speaker_sw(lv_event_t *e)
{
    muse_settings_set_speaker_on(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void on_volume(lv_event_t *e)
{
    int v = lv_slider_get_value(s_vol_sl);
    set_val(s_vol_val, "%d%%", v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        muse_settings_set_volume(v);
        muse_voice_request_chirp();
    } else {
        muse_audio_set_volume(v);
    }
}

static void on_gain(lv_event_t *e)
{
    int db = lv_slider_get_value(s_gain_sl) * 3;
    set_val(s_gain_val, "%d dB", db);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        muse_settings_set_mic_gain(db);
    } else {
        muse_audio_set_mic_gain(db);
    }
}

static void build_sound_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_sound = page(tile, "SOUND", &list);
    s_spk_sw = switch_row(list, "Speaker", muse_settings_speaker_on(), on_speaker_sw);
    s_vol_sl = slider(list, "Volume", 0, 100, muse_settings_volume(), &s_vol_val, on_volume);
    s_gain_sl = slider(list, "Mic gain", 0, MUSE_MIC_GAIN_MAX / 3, muse_settings_mic_gain() / 3, &s_gain_val, on_gain);

    lv_obj_t *meter = lv_obj_create(list);
    lv_obj_remove_style_all(meter);
    lv_obj_set_size(meter, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(meter, MUSE_ROW_PAD, 0);   /* in line with the sliders */
    lv_obj_remove_flag(meter, LV_OBJ_FLAG_SCROLLABLE);
    label(meter, MUSE_FONT_NOTE, MUSE_COLOR_DIM, "Mic level");
    s_mic_val = label(meter, MUSE_FONT_NOTE, MUSE_COLOR_DIM, "");
    lv_obj_align(s_mic_val, LV_ALIGN_TOP_RIGHT, 0, 0);
    s_mic_bar = lv_bar_create(meter);
    lv_obj_set_size(s_mic_bar, lv_pct(94), 10);
    lv_obj_align(s_mic_bar, LV_ALIGN_TOP_MID, 0, 26);
    lv_bar_set_range(s_mic_bar, 0, 60);   /* -70..-10 dBFS */
    lv_obj_set_style_bg_color(s_mic_bar, lv_color_hex(MUSE_COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_anim_duration(s_mic_bar, 80, 0);
    note(list, "Talk at arm's length: the bar should reach green (-30 to -15 dBFS) without going orange.");

    set_val(s_vol_val, "%d%%", muse_settings_volume());
    set_val(s_gain_val, "%d dB", muse_settings_mic_gain() / 3 * 3);
    back_row(list, "Back");
}

static void tick_sound(void)
{
    bool on = muse_settings_speaker_on();   /* also toggled from the face */
    if (on != lv_obj_has_state(s_spk_sw, LV_STATE_CHECKED)) {
        lv_obj_set_state(s_spk_sw, LV_STATE_CHECKED, on);
    }
    float db = muse_voice_monitor_db();
    int v = (int)(db + 70.0f);
    v = v < 0 ? 0 : (v > 60 ? 60 : v);
    lv_bar_set_value(s_mic_bar, v, LV_ANIM_ON);
    uint32_t color = db > -12.0f ? MUSE_COLOR_WARN : (db > -30.0f ? MUSE_COLOR_OK : MUSE_COLOR_ACCENT);
    lv_obj_set_style_bg_color(s_mic_bar, lv_color_hex(color), LV_PART_INDICATOR);
    set_val(s_mic_val, "%d dBFS", (int)db);
}

/* ---------- Display ---------- */

static void on_bright(lv_event_t *e)
{
    int v = lv_slider_get_value(s_bright_sl);
    set_val(s_bright_val, "%d%%", v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        muse_settings_set_brightness(v);
    } else {
        muse_ui_preview_brightness(v);
    }
}

static void on_sleep_choice(lv_event_t *e)
{
    muse_settings_set_sleep_s(SLEEP_CHOICES[(int)(intptr_t)lv_event_get_user_data(e)]);
}

static void on_sleep_now(lv_event_t *e)
{
    (void)e;
    muse_state_set_asleep(true);
}

static void build_display_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_display = page(tile, "DISPLAY", &list);
    s_bright_sl = slider(list, "Brightness", 10, 100, muse_settings_brightness(), &s_bright_val, on_bright);
    set_val(s_bright_val, "%d%%", muse_settings_brightness());
    note(list, "Screen off when idle for");
    for (int i = 0; i < SLEEP_COUNT; i++) {
        row(list, NULL, SLEEP_NAMES[i], &s_sleep_checks[i], on_sleep_choice, (void *)(intptr_t)i);
        lv_obj_set_style_text_color(s_sleep_checks[i], lv_color_hex(MUSE_COLOR_ACCENT), 0);
    }
    button(list, LV_SYMBOL_EYE_CLOSE "  Sleep now", MUSE_BUTTON_ACCENT, on_sleep_now, NULL);
    back_row(list, "Back");
}

static const char *sleep_name(int secs)
{
    for (int i = 0; i < SLEEP_COUNT; i++) {
        if (SLEEP_CHOICES[i] == secs) {
            return SLEEP_NAMES[i];
        }
    }
    return "Custom";
}

static void tick_display(void)
{
    int cur = muse_settings_sleep_s();
    for (int i = 0; i < SLEEP_COUNT; i++) {
        set_text(s_sleep_checks[i], SLEEP_CHOICES[i] == cur ? LV_SYMBOL_OK : "");
    }
}

/* ---------- Mode ---------- */

#if CONFIG_MUSE_GADGET_HOME_EXTRAS
static void on_clock_24h_sw(lv_event_t *e)
{
    muse_home_extras_set_24h(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}
#endif

enum { PICK_HOME, PICK_AWAY };
#define PICK_NONE 0xff   /* "None": no network */

static void on_mode_net(void *user)
{
    intptr_t v = (intptr_t)user;
    int which = (int)(v >> 8), i = (int)(v & 0xff);
    const char *ssid = i == PICK_NONE ? "" : s_mode_nets[i].ssid;
    if (which == PICK_HOME) {
        muse_settings_set_home_ssid(ssid);
    } else {
        muse_settings_set_away_ssid(ssid);
    }
}

/* A Wi-Fi row was tapped: the saved networks and "None" to pick from, the
 * one set now ticked. */
static void on_mode_pick(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    char cur[MUSE_SSID_MAX + 1];
    if (which == PICK_HOME) {
        muse_settings_home_ssid(cur);
    } else {
        muse_settings_away_ssid(cur);
    }
    int n = muse_wifi_saved(s_mode_nets, MUSE_WIFI_SAVED_MAX);
    muse_dialog_option_t options[MUSE_WIFI_SAVED_MAX + 1];
    bool ticked = false;
    for (int i = 0; i < n; i++) {
        bool on = !strcmp(cur, s_mode_nets[i].ssid);
        ticked |= on;
        options[i] = (muse_dialog_option_t){ s_mode_nets[i].ssid, LV_SYMBOL_WIFI, on, on_mode_net,
                                             (void *)(intptr_t)((which << 8) | i) };
    }
    options[n] = (muse_dialog_option_t){ "None", LV_SYMBOL_CLOSE, !ticked, on_mode_net,
                                         (void *)(intptr_t)((which << 8) | PICK_NONE) };
    const muse_dialog_t d = {
        .title = which == PICK_HOME ? "Home Wi-Fi" : "On-the-go Wi-Fi",
        .text = n ? NULL : "No saved networks yet: add one on the Wi-Fi page.",
        .help = which == PICK_HOME
                    ? "Home Wi-Fi is the network where you usually are. On another one, Muse offers On-the-go mode."
                    : "On-the-go Wi-Fi is one you join when you're out, such as your phone's hotspot. "
                      "Joining it switches Muse to On-the-go mode by itself.",
        .options = options,
        .option_count = n + 1,
    };
    muse_dialog_open(s_tile, &d);
}

/* ---- Mode: Night's times and the alarm's, set with a picker over the page ---- */

enum { TIME_NIGHT_FROM, TIME_NIGHT_TO, TIME_ALARM, TIME_COUNT };
static const char *const TIME_TITLES[TIME_COUNT] = { "NIGHT STARTS", "NIGHT ENDS", "ALARM" };
static lv_obj_t *s_time_vals[TIME_COUNT];
static lv_obj_t *s_time_box, *s_time_title, *s_time_h, *s_time_m;
static int s_time_which;
#if CONFIG_MUSE_GADGET_ALARM
static lv_obj_t *s_alarm_sw;
#endif

/* The time `which` is set to, in minutes after local midnight. */
static int time_get(int which)
{
    int from, to;
    muse_gadget_mode_night(&from, &to);
#if CONFIG_MUSE_GADGET_ALARM
    if (which == TIME_ALARM) {
        return muse_rtc_alarm(NULL);
    }
#endif
    return which == TIME_NIGHT_TO ? to : from;
}

static void time_set(int which, int min)
{
    int from, to;
    muse_gadget_mode_night(&from, &to);
    if (which == TIME_NIGHT_FROM) {
        muse_gadget_mode_set_night(min, to);
    } else if (which == TIME_NIGHT_TO) {
        muse_gadget_mode_set_night(from, min);
    }
#if CONFIG_MUSE_GADGET_ALARM
    if (which == TIME_ALARM) {
        muse_rtc_set_alarm(min, true);   /* setting its time turns it on */
        lv_obj_add_state(s_alarm_sw, LV_STATE_CHECKED);
    }
#endif
}

/* 21:00, or 9:00 PM, as the face's clock shows the time. */
static void time_text(int min, char *out, size_t cap)
{
    int h = min / 60, m = min % 60;
    if (muse_home_extras_24h()) {
        snprintf(out, cap, "%02d:%02d", h, m);
    } else {
        snprintf(out, cap, "%d:%02d %s", h % 12 ? h % 12 : 12, m, h < 12 ? "AM" : "PM");
    }
}

static void on_time_pick(lv_event_t *e)
{
    s_time_which = (int)(intptr_t)lv_event_get_user_data(e);
    int min = time_get(s_time_which);
    int step = (min % 60 + 2) / 5;   /* the nearest 5 minutes */
    lv_roller_set_selected(s_time_h, min / 60, LV_ANIM_OFF);
    lv_roller_set_selected(s_time_m, step > 11 ? 11 : step, LV_ANIM_OFF);
    set_text(s_time_title, TIME_TITLES[s_time_which]);
    lv_obj_remove_flag(s_time_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_time_box);
}

static void on_time_done(lv_event_t *e)
{
    if (lv_event_get_user_data(e)) {
        time_set(s_time_which, (int)lv_roller_get_selected(s_time_h) * 60 + (int)lv_roller_get_selected(s_time_m) * 5);
    }
    lv_obj_add_flag(s_time_box, LV_OBJ_FLAG_HIDDEN);
}

#if CONFIG_MUSE_GADGET_ALARM
static void on_alarm_sw(lv_event_t *e)
{
    muse_rtc_set_alarm(muse_rtc_alarm(NULL), lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}
#endif

static lv_obj_t *time_roller(lv_obj_t *box, const char *options, int x)
{
    lv_obj_t *r = lv_roller_create(box);
    lv_roller_set_options(r, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, 3);
    lv_obj_set_width(r, 112);
    lv_obj_set_style_text_font(r, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(r, lv_color_hex(MUSE_COLOR_DIM), 0);
    lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(MUSE_COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, MUSE_ROW_RADIUS, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(MUSE_COLOR_ACCENT), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, lv_color_hex(MUSE_COLOR_TEXT), LV_PART_SELECTED);
    lv_obj_align(r, LV_ALIGN_CENTER, x, -16);
    return r;
}

/* Cancel (outlined, as Back is) or OK (filled), side by side under the rollers. */
static void time_button(lv_obj_t *box, const char *text, muse_button_kind_t kind, int x, bool ok)
{
    lv_obj_t *b = muse_style_button(box, text, kind, MUSE_FONT_ROW, MUSE_ROW_H + 6);
    lv_obj_set_width(b, 140);
    lv_obj_align(b, LV_ALIGN_CENTER, x, 118);
    lv_obj_add_event_cb(b, on_time_done, LV_EVENT_CLICKED, ok ? (void *)1 : NULL);
}

/* A time picker over the whole page: hours and minutes (in fives) to roll,
 * OK and Cancel; on_time_pick opens it for a row. */
static void build_time_picker(lv_obj_t *p)
{
    s_time_box = lv_obj_create(p);
    lv_obj_remove_style_all(s_time_box);
    lv_obj_set_size(s_time_box, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_time_box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_time_box, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_time_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);   /* taps stop here */
    lv_obj_remove_flag(s_time_box, LV_OBJ_FLAG_SCROLLABLE);
    s_time_title = muse_style_title(s_time_box, "");
    char opts[80];   /* 24 two-digit lines */
    int n = 0;
    for (int h = 0; h < 24; h++) {
        n += snprintf(opts + n, sizeof(opts) - n, h ? "\n%02d" : "%02d", h);
    }
    s_time_h = time_roller(s_time_box, opts, -68);
    n = 0;
    for (int m = 0; m < 60; m += 5) {
        n += snprintf(opts + n, sizeof(opts) - n, m ? "\n%02d" : "%02d", m);
    }
    s_time_m = time_roller(s_time_box, opts, 68);
    lv_obj_align(label(s_time_box, &lv_font_montserrat_28, MUSE_COLOR_TEXT, ":"), LV_ALIGN_CENTER, 0, -18);
    time_button(s_time_box, "Cancel", MUSE_BUTTON_NEUTRAL, -78, false);
    time_button(s_time_box, LV_SYMBOL_OK "  OK", MUSE_BUTTON_ACCENT, 78, true);
}

/* A row showing a network, tapped to pick from the saved ones. */
static lv_obj_t *mode_net_row(lv_obj_t *list, const char *text, int which)
{
    lv_obj_t *value;
    row(list, LV_SYMBOL_WIFI, text, &value, on_mode_pick, (void *)(intptr_t)which);
    lv_obj_set_style_text_color(value, lv_color_hex(MUSE_COLOR_ACCENT), 0);
    return value;
}

static void build_mode_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_mode = page(tile, "MODES", &list);
    for (int i = TIME_NIGHT_FROM; i <= TIME_NIGHT_TO; i++) {
        row(list, LV_SYMBOL_EYE_CLOSE, i == TIME_NIGHT_FROM ? "Night starts" : "Night ends", &s_time_vals[i],
            on_time_pick, (void *)(intptr_t)i);
        lv_obj_set_style_text_color(s_time_vals[i], lv_color_hex(MUSE_COLOR_ACCENT), 0);
    }
#if CONFIG_MUSE_GADGET_ALARM
    bool alarm_on;
    muse_rtc_alarm(&alarm_on);
    s_alarm_sw = switch_row(list, "Morning alarm", alarm_on, on_alarm_sw);
    row(list, LV_SYMBOL_BELL, "Alarm time", &s_time_vals[TIME_ALARM], on_time_pick, (void *)(intptr_t)TIME_ALARM);
    lv_obj_set_style_text_color(s_time_vals[TIME_ALARM], lv_color_hex(MUSE_COLOR_ACCENT), 0);
#endif
    s_mode_home = mode_net_row(list, "Home Wi-Fi", PICK_HOME);
    s_mode_away = mode_net_row(list, "On-the-go Wi-Fi", PICK_AWAY);
#if CONFIG_MUSE_GADGET_HOME_EXTRAS
    switch_row(list, "24-hour clock", muse_home_extras_24h(), on_clock_24h_sw);
#endif
    back_row(list, "Back");
    build_time_picker(s_mode);
}

static void tick_mode(void)
{
    char home[MUSE_SSID_MAX + 1];
    muse_settings_home_ssid(home);
    set_text(s_mode_home, home[0] ? home : "Not set");
    muse_settings_away_ssid(home);
    set_text(s_mode_away, home[0] ? home : "Not set");
    for (int i = 0; i < TIME_COUNT; i++) {
        if (s_time_vals[i]) {
            char t[16];
            time_text(time_get(i), t, sizeof(t));
            set_text(s_time_vals[i], t);
        }
    }
}

/* ---------- Battery ---------- */

static void on_battery_reset(lv_event_t *e)
{
    (void)e;
    muse_battery_reset();
    s_batt_shown_us = 0;   /* show it now */
}

static void build_battery_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_battery = page(tile, "BATTERY", &list);
    s_batt_shown_us = 0;
    s_batt_status = note(list, "");
    s_batt_level = info_row(list, "Battery");
    s_batt_drain = info_row(list, "Used");
    s_batt_full = info_row(list, "A full charge");
    s_batt_off = info_row(list, "Screen off");
    s_batt_slept = info_row(list, "Chip asleep");
    s_batt_wakes = info_row(list, "Wakes");
    s_batt_busy = info_row(list, "CPU busy");
    s_batt_awake = note(list, "");
    button(list, LV_SYMBOL_REFRESH "  Start over", MUSE_BUTTON_ACCENT, on_battery_reset, NULL);
    note(list, "Measures from unplugging USB until it's plugged back in. The gauge moves in 1% steps, so give it a "
               "few hours.");
    back_row(list, "Back");
}

/* A per-mille figure as a percentage. */
static void set_pm(lv_obj_t *l, int pm)
{
    char buf[16] = "-";
    if (pm >= 0) {
        snprintf(buf, sizeof(buf), "%d.%d%%", pm / 10, pm % 10);
    }
    set_text(l, buf);
}

static void tick_battery(void)
{
    int64_t now = esp_timer_get_time();
    if (s_batt_shown_us && now - s_batt_shown_us < 1000000) {
        return;
    }
    s_batt_shown_us = now;
    muse_battery_t b;
    muse_battery_read(&b);
    muse_power_t p = muse_state_power();
    char buf[96], t[24];

    int h = (int)(b.secs / 3600), m = (int)(b.secs / 60 % 60);
    if (h) {
        snprintf(t, sizeof(t), "%d h %d min", h, m);
    } else {
        snprintf(t, sizeof(t), "%d min", m);
    }
    if (!b.started) {
        strlcpy(buf, p.battery_pct < 0 ? "No battery" : "Unplug USB to start measuring.", sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), b.running ? "On battery for %s" : "Last run: %s on battery", t);
    }
    set_text(s_batt_status, buf);

    if (p.battery_pct < 0) {
        strlcpy(buf, "None", sizeof(buf));
    } else if (p.battery_mv) {
        snprintf(buf, sizeof(buf), "%s%d%%  %d.%02d V", p.charging ? LV_SYMBOL_CHARGE " " : "", p.battery_pct,
                 p.battery_mv / 1000, p.battery_mv % 1000 / 10);
    } else {
        snprintf(buf, sizeof(buf), "%s%d%%", p.charging ? LV_SYMBOL_CHARGE " " : "", p.battery_pct);
    }
    set_text(s_batt_level, buf);

    int used = b.pct_start - b.pct_now, rate10, full_h;
    if (!b.started) {
        set_text(s_batt_drain, "-");
        set_text(s_batt_full, "-");
    } else if (muse_battery_drain(&b, &rate10, &full_h)) {
        snprintf(buf, sizeof(buf), "%d%%, %d.%d%%/h", used, rate10 / 10, rate10 % 10);
        set_text(s_batt_drain, buf);
        snprintf(buf, sizeof(buf), "Lasts ~%d h", full_h);
        set_text(s_batt_full, buf);
    } else {
        snprintf(buf, sizeof(buf), "%d%% so far", used > 0 ? used : 0);
        set_text(s_batt_drain, buf);
        set_text(s_batt_full, "Measuring");
    }

    set_pm(s_batt_off, b.started ? b.screen_off_pm : -1);
    set_pm(s_batt_slept, b.started ? b.slept_pm : -1);
    set_pm(s_batt_busy, b.started ? b.busy_pm : -1);
    if (b.started && b.secs && b.slept_pm >= 0) {
        int per10 = (int)(b.sleeps * 10LL / b.secs);
        snprintf(buf, sizeof(buf), "%d.%d/s", per10 / 10, per10 % 10);
        set_text(s_batt_wakes, buf);
    } else {
        set_text(s_batt_wakes, "-");
    }
    buf[0] = '\0';
    if (b.started && b.awake[0]) {
        snprintf(buf, sizeof(buf), "Also kept awake by: %s", b.awake);
    }
    set_text(s_batt_awake, buf);
}

/* ---------- Power ---------- */

static void on_power_off(lv_event_t *e)
{
    (void)e;
    show(s_home);
    muse_ui_show_face();
    muse_input_request_power_off();
}

static void build_power_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_power = page(tile, "POWER", &list);
    button(list, LV_SYMBOL_POWER "  Power off", MUSE_BUTTON_DANGER, on_power_off, NULL);
    char text[128];
    const char *power = muse_board->power_button ? muse_board->power_button : muse_board->talk_button;
    const char *sleep = muse_board->power_button ? muse_board->power_button : muse_board->aux_button;
    snprintf(text, sizeof(text), "Press the %s button to turn it back on. To just turn the screen off, press the %s button.",
             power, sleep);
    note(list, text);
    back_row(list, "Cancel");
}

/* ---------- Passcode (muse_lock.h) ---------- */

static void on_pass_done(bool ok)
{
    (void)ok;
    s_pass_shown = -1;   /* the switch and rows as they are now */
}

static void on_pass_sw(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool want = lv_obj_has_state(sw, LV_STATE_CHECKED), has = muse_lock_has_pin();
    lv_obj_set_state(sw, LV_STATE_CHECKED, has);   /* as it is, till the keypad's done */
    if (want && !has) {
        muse_lock_ui_set_pin(on_pass_done);
    } else if (!want && has) {
        muse_lock_ui_turn_off(on_pass_done);
    }
}

static void on_pass_change(lv_event_t *e)
{
    (void)e;
    muse_lock_ui_change_pin(on_pass_done);
}

static void on_after_pick(void *user)
{
    intptr_t v = (intptr_t)user;
    muse_lock_set_after(v >> 8, (int)(v & 0xff));
}

/* "On battery" or "On a charger": how long asleep before it's asked for again. */
static void on_after(lv_event_t *e)
{
    int charger = (int)(intptr_t)lv_event_get_user_data(e);
    int cur = muse_lock_after(charger);
    muse_dialog_option_t options[MUSE_LOCK_AFTER_COUNT];
    for (int i = 0; i < MUSE_LOCK_AFTER_COUNT; i++) {
        options[i] = (muse_dialog_option_t){ MUSE_LOCK_AFTER_NAMES[i], NULL, i == cur, on_after_pick,
                                             (void *)(intptr_t)(charger << 8 | i) };
    }
    const muse_dialog_t d = {
        .title = charger ? "On a charger" : "On battery",
        .help = "How long the screen can be off before Muse asks for the passcode again. With some of it on "
                "battery and some on a charger, the shorter of the two counts.",
        .options = options,
        .option_count = MUSE_LOCK_AFTER_COUNT,
    };
    muse_dialog_open(s_tile, &d);
}

static void build_passcode_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_passcode = page(tile, "PASSCODE", &list);
    s_pass_shown = -1;
    s_pass_sw = switch_row(list, "Passcode", muse_lock_has_pin(), on_pass_sw);
    s_pass_more = column(list);
    row(s_pass_more, LV_SYMBOL_EDIT, "Change passcode", NULL, on_pass_change, NULL);
    note(s_pass_more, "Ask again after sleeping for");
    for (int charger = 0; charger < 2; charger++) {
        row(s_pass_more, charger ? LV_SYMBOL_CHARGE : LV_SYMBOL_BATTERY_FULL, charger ? "On a charger" : "On battery",
            &s_pass_after[charger], on_after, (void *)(intptr_t)charger);
        lv_obj_set_style_text_color(s_pass_after[charger], lv_color_hex(MUSE_COLOR_ACCENT), 0);
    }
    char text[160];
    snprintf(text, sizeof(text), "A 6-digit passcode, asked for whenever Muse starts. %d wrong tries lock it for an hour; "
                                 "%d more erase everything on it.", MUSE_LOCK_TRIES, MUSE_LOCK_TRIES);
    note(list, text);
    back_row(list, "Back");
}

static void tick_passcode(void)
{
    bool has = muse_lock_has_pin();
    if ((int)has != s_pass_shown) {
        s_pass_shown = has;
        lv_obj_set_state(s_pass_sw, LV_STATE_CHECKED, has);
        lv_obj_set_flag(s_pass_more, LV_OBJ_FLAG_HIDDEN, !has);
    }
    for (int charger = 0; charger < 2; charger++) {
        set_text(s_pass_after[charger], MUSE_LOCK_AFTER_NAMES[muse_lock_after(charger)]);
    }
}

/* ---------- Home ---------- */

static const page_t PASSCODE = { &s_passcode, build_passcode_page };
static const page_t WIFI = { &s_wifi, build_wifi_page };
static const page_t HATCH = { &s_hatch, build_hatch_page };
static const page_t BLE = { &s_ble, build_ble_page };
static const page_t SOUND = { &s_sound, build_sound_page };
static const page_t DISPLAY = { &s_display, build_display_page };
static const page_t BATTERY = { &s_battery, build_battery_page };
static const page_t POWER = { &s_power, build_power_page };
static const page_t MODE = { &s_mode, build_mode_page };

/* ---------- Advanced: what's set once, or looked at when something's wrong ---------- */

/* ---------- Reset device: two warnings, then everything's erased ---------- */

static const char *const RESET_WARNINGS[] = {
    "This erases everything on this device: Wi-Fi, its pairing with your Muse account, "
    "chats, modes and every setting. You'll set it up again in the Muse app, as if it were new.",
    "Are you sure? This can't be undone. Your conversations stay in the Muse app, "
    "but this device forgets them all and restarts.",
};
static const char *const RESET_BUTTONS[] = { "Continue", "Erase and restart" };

static void reset_show_step(void)
{
    set_text(s_reset_note, RESET_WARNINGS[s_reset_step]);
    set_text(s_reset_go_lbl, RESET_BUTTONS[s_reset_step]);
}

static void on_reset_go(lv_event_t *e)
{
    (void)e;
    if (s_reset_step == 0) {
        s_reset_step = 1;
        reset_show_step();
        return;
    }
    set_text(s_reset_note, "Erasing... it restarts in a moment, ready to set up in the Muse app.");
    set_text(s_reset_go_lbl, "");
    muse_link_factory_reset();
}

static void build_reset_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_reset = page(tile, "RESET DEVICE", &list);
    s_reset_step = 0;
    s_reset_note = note(list, "");
    lv_obj_set_style_text_color(s_reset_note, lv_color_hex(MUSE_COLOR_WARN), 0);
    button(list, "", MUSE_BUTTON_DANGER, on_reset_go, &s_reset_go_lbl);
    back_row(list, "Cancel");
    reset_show_step();
}

static const page_t RESET = { &s_reset, build_reset_page };

/* ---------- About: what this device is, and how it's doing ---------- */

/* A card of a dim label over its value, the whole width for a long one. */
static lv_obj_t *about_row(lv_obj_t *list, const char *text, const char *value)
{
    lv_obj_t *c = card(list, false);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(c, 8, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(c, 2, 0);
    label(c, MUSE_FONT_NOTE, MUSE_COLOR_DIM, text);
    lv_obj_t *v = label(c, MUSE_FONT_VALUE, MUSE_COLOR_TEXT, value);
    lv_obj_set_width(v, lv_pct(100));
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
    return v;
}

static void build_about_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_about = page(tile, "ABOUT", &list);
    about_row(list, "Firmware", esp_app_get_description()->version);
    about_row(list, "Board", muse_board->name);
    muse_ble_status_t b;
    muse_ble_status(&b);
    about_row(list, "Device name", b.name[0] ? b.name : "-");
    s_about_wifi = about_row(list, "Wi-Fi network", "");
    s_about_signal = about_row(list, "Signal", "");
    s_about_ip = about_row(list, "IP address", "");
    char buf[18] = "-";
    uint8_t m[6];
    if (esp_read_mac(m, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    about_row(list, "MAC address", buf);
    s_about_uptime = about_row(list, "Up for", "");
    s_about_mem = about_row(list, "Free memory", "");
    back_row(list, "Back");
}

/* 1.5 MB, 18 KB */
static void bytes_text(size_t n, char *out, size_t cap)
{
    if (n >= 1024 * 1024) {
        snprintf(out, cap, "%u.%u MB", (unsigned)(n >> 20), (unsigned)((n & 0xfffff) * 10 >> 20));
    } else {
        snprintf(out, cap, "%u KB", (unsigned)(n >> 10));
    }
}

static void tick_about(void)
{
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    bool on = w.state == MUSE_WIFI_CONNECTED;
    char buf[64];
    set_text(s_about_wifi, on ? w.ssid : "Not connected");
    set_text(s_about_ip, on ? w.ip : "-");
    if (on) {
        const char *how = w.rssi >= -60 ? "strong" : w.rssi >= -70 ? "good" : w.rssi >= -80 ? "fair" : "weak";
        snprintf(buf, sizeof(buf), "%d dBm, %s", w.rssi, how);
    } else {
        strlcpy(buf, "-", sizeof(buf));
    }
    set_text(s_about_signal, buf);

    int64_t up = esp_timer_get_time() / 1000000;
    int d = (int)(up / 86400), h = (int)(up / 3600 % 24), min = (int)(up / 60 % 60);
    if (d) {
        snprintf(buf, sizeof(buf), "%d d %d h %d min", d, h, min);
    } else if (h) {
        snprintf(buf, sizeof(buf), "%d h %d min", h, min);
    } else {
        snprintf(buf, sizeof(buf), "%d min", min);
    }
    set_text(s_about_uptime, buf);

    char in[16], ps[16];
    bytes_text(heap_caps_get_free_size(MALLOC_CAP_INTERNAL), in, sizeof(in));
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM)) {
        bytes_text(heap_caps_get_free_size(MALLOC_CAP_SPIRAM), ps, sizeof(ps));
        snprintf(buf, sizeof(buf), "%s internal, %s PSRAM", in, ps);
    } else {
        snprintf(buf, sizeof(buf), "%s", in);
    }
    set_text(s_about_mem, buf);
}

static const page_t ABOUT = { &s_about, build_about_page };

static void build_advanced_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_advanced = page(tile, "ADVANCED", &list);
    row(list, LV_SYMBOL_HOME, "Muse connection", &s_adv_hatch, on_nav, (void *)&HATCH);
    row(list, LV_SYMBOL_BLUETOOTH, "Bluetooth", &s_adv_ble, on_nav, (void *)&BLE);
    row(list, LV_SYMBOL_BATTERY_FULL, "Battery", &s_adv_battery, on_nav, (void *)&BATTERY);
    row(list, LV_SYMBOL_FILE, "About", NULL, on_nav, (void *)&ABOUT);
    row(list, LV_SYMBOL_WARNING, "Reset device", NULL, on_nav, (void *)&RESET);
    back_row(list, "Back");
}

static void tick_advanced(void)
{
    /* A tick once it's connected, as the row has little room for words. */
    muse_hatch_status_t h;
    muse_hatch_status(&h);
    const char *sym = LV_SYMBOL_CLOSE;   /* not set up */
    uint32_t color = MUSE_COLOR_DIM;
    if (h.state == MUSE_HATCH_REACHABLE) {
        sym = LV_SYMBOL_OK;
        color = MUSE_COLOR_OK;
    } else if (h.state == MUSE_HATCH_TESTING || h.state == MUSE_HATCH_UNTESTED) {
        sym = LV_SYMBOL_REFRESH;
    } else if (h.state == MUSE_HATCH_UNREACHABLE || h.state == MUSE_HATCH_OFFLINE) {
        sym = LV_SYMBOL_WARNING;
        color = MUSE_COLOR_WARN;
    }
    set_text(s_adv_hatch, sym);
    lv_obj_set_style_text_color(s_adv_hatch, lv_color_hex(color), 0);

    muse_ble_status_t b;
    muse_ble_status(&b);
    set_text(s_adv_ble, b.state == MUSE_BLE_OFF ? "Off" : (b.state == MUSE_BLE_CONNECTED ? "Connected" : "On"));

    muse_power_t p = muse_state_power();
    char buf[96];
    if (p.battery_pct < 0) {
        strlcpy(buf, p.usb ? "USB power" : "", sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "%s%d%%", p.charging ? LV_SYMBOL_CHARGE " " : "", p.battery_pct);
    }
    set_text(s_adv_battery, buf);
}

static const page_t ADVANCED = { &s_advanced, build_advanced_page };


/* ---------- General: the everyday settings ---------- */

static void build_general_page(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_general = page(tile, "GENERAL", &list);
    row(list, LV_SYMBOL_WIFI, "Wi-Fi", &s_home_wifi, on_nav, (void *)&WIFI);
    row(list, LV_SYMBOL_IMAGE, "Display", &s_home_display, on_nav, (void *)&DISPLAY);
    row(list, LV_SYMBOL_VOLUME_MAX, "Sound", &s_home_sound, on_nav, (void *)&SOUND);
    lv_obj_t *pass = row(list, NULL, "Passcode", &s_home_passcode, on_nav, (void *)&PASSCODE);
    lv_obj_move_to_index(muse_style_padlock(pass, 22, MUSE_COLOR_ACCENT), 0);   /* no padlock in the symbol font */
    back_row(list, "Back");
}

static const page_t GENERAL = { &s_general, build_general_page };

/* ---------- Home ---------- */

static void build_home(lv_obj_t *tile)
{
    lv_obj_t *list;
    s_home = page(tile, "SETTINGS", &list);
    row(list, LV_SYMBOL_SHUFFLE, "Modes", &s_home_mode, on_nav, (void *)&MODE);
    row(list, LV_SYMBOL_LIST, "General", NULL, on_nav, (void *)&GENERAL);
    row(list, LV_SYMBOL_SETTINGS, "Advanced", NULL, on_nav, (void *)&ADVANCED);
    row(list, LV_SYMBOL_POWER, "Power off", NULL, on_nav, (void *)&POWER);
}

static void tick_general(void)
{
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    static const char *const WIFI_VALUES[] = { "Off", "Not set", "Joining", "", "Failed", "Not nearby" };
    set_text(s_home_wifi, w.state == MUSE_WIFI_CONNECTED ? w.ssid : WIFI_VALUES[w.state]);

    if (muse_settings_speaker_on()) {
        set_val(s_home_sound, "Vol %d%%", muse_settings_volume());
    } else {
        set_text(s_home_sound, "Muted");
    }
    set_text(s_home_display, sleep_name(muse_settings_sleep_s()));
    set_text(s_home_passcode, muse_lock_has_pin() ? "On" : "Off");
}

static void tick_home(void)
{
    set_text(s_home_mode, muse_gadget_mode_name(muse_gadget_mode()));
}

/* ---------- public ---------- */

void muse_settings_ui_build(lv_obj_t *tile)
{
    if (muse_board->round && muse_board->height < 466) {
        s_text_scale = muse_board->height;
    }
    s_tile = tile;
    build_home(tile);
    show(s_home);
}

void muse_settings_ui_tick(bool visible)
{
    static bool was_visible;
    if (visible != was_visible) {
        was_visible = visible;
        /* Only listen to the mic while the Sound page is actually on screen. */
        muse_voice_set_monitor(visible && s_current == s_sound);
    }
    if (!visible) {
        muse_dialog_close_on(s_tile);   /* not still open on the way back */
        return;
    }
    if (s_current && s_current == s_home) {
        tick_home();
    } else if (s_current == s_general) {
        tick_general();
    } else if (s_current == s_wifi) {
        tick_wifi();
    } else if (s_current == s_hatch) {
        tick_hatch();
    } else if (s_current == s_ble) {
        tick_ble();
    } else if (s_current == s_sound) {
        tick_sound();
    } else if (s_current == s_display) {
        tick_display();
    } else if (s_current == s_battery) {
        tick_battery();
    } else if (s_current == s_mode) {
        tick_mode();
    } else if (s_current == s_advanced) {
        tick_advanced();
    } else if (s_current == s_about) {
        tick_about();
    } else if (s_current == s_passcode) {
        tick_passcode();
    }
}

void muse_settings_ui_go_home(void)
{
    muse_dialog_close_on(s_tile);
    for (int i = 0; i < 8 && s_current && s_current != s_home; i++) {
        go_back();
    }
}

bool muse_settings_ui_in_subpage(void)
{
    return s_current != s_home || muse_dialog_is_open();   /* a dialog holds the screen as a page does */
}
