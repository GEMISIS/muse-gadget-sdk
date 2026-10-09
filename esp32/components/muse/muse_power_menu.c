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

#include "muse_power_menu.h"

#include <stdint.h>
#include <stdio.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "muse_input.h"
#include "muse_lock.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_style.h"
#include "muse_voice.h"

static const char *TAG = "muse_power_menu";

#define VOLUME_SHOW_S 1.5f      /* the volume bar, after the last step */
#define IDLE_CLOSE_S 15.0f      /* the menu, left alone */
#define KEY_QUEUE 8
#define CARD_W 300
/* The keys' hint: the cards' text on the 2.16, as it was elsewhere. */
#if CONFIG_MUSE_BOARD_WAVESHARE_S3_216
#define FONT_HINT MUSE_FONT_NOTE
#else
#define FONT_HINT (&lv_font_montserrat_14)
#endif

/* Lock now only with a passcode, and not locked already (muse_lock.h). */
enum { ITEM_LOCK, ITEM_SLEEP, ITEM_POWER_OFF, ITEM_RESTART, ITEM_CANCEL, ITEM_COUNT };

static const char *const ITEM_TEXT[ITEM_COUNT] = {
    [ITEM_LOCK] = "Lock now",
    [ITEM_SLEEP] = LV_SYMBOL_EYE_CLOSE "  Sleep",
    [ITEM_POWER_OFF] = LV_SYMBOL_POWER "  Power off",
    [ITEM_RESTART] = LV_SYMBOL_REFRESH "  Restart",
    [ITEM_CANCEL] = "Cancel",
};

static const uint32_t ITEM_COLOR[ITEM_COUNT] = {
    [ITEM_LOCK] = MUSE_COLOR_ACCENT,
    [ITEM_SLEEP] = MUSE_COLOR_ACCENT,
    [ITEM_POWER_OFF] = MUSE_COLOR_DANGER,
    [ITEM_RESTART] = MUSE_COLOR_TEXT,
    [ITEM_CANCEL] = MUSE_COLOR_TEXT,
};

/* Written by the input task, read by the LVGL task. */
static QueueHandle_t s_keys;
static volatile bool s_open;
static volatile int s_volume_pct;
static volatile uint32_t s_volume_seq;

/* LVGL task only. */
static lv_obj_t *s_backdrop;
EXT_RAM_BSS_ATTR static lv_obj_t *s_items[ITEM_COUNT];   /* PSRAM: Lock now made it longer */
static lv_obj_t *s_volume, *s_volume_lbl, *s_volume_bar;
static lv_obj_t *s_hint;
static int s_sel;
static bool s_shown;
static float s_idle_until;
static float s_volume_until;
static uint32_t s_volume_shown_seq;

void muse_power_menu_key(muse_power_menu_key_t key)
{
    if (!s_keys) {
        return;
    }
    if (key == MUSE_POWER_MENU_OPEN) {
        s_open = true;
    }
    uint8_t k = (uint8_t)key;
    xQueueSend(s_keys, &k, 0);
}

bool muse_power_menu_is_open(void)
{
    return s_open;
}

void muse_power_menu_show_volume(int pct)
{
    s_volume_pct = pct;
    s_volume_seq++;   /* only the input task writes it */
}

/* ---------- LVGL task ---------- */

static void highlight(void)
{
    for (int i = 0; i < ITEM_COUNT; i++) {
        bool sel = i == s_sel;
        lv_obj_set_style_bg_color(s_items[i], lv_color_hex(sel ? MUSE_COLOR_RAISED_PRESSED : MUSE_COLOR_CARD_PRESSED), 0);
        lv_obj_set_style_border_width(s_items[i], sel ? MUSE_CARD_BORDER : 0, 0);
    }
}

static bool item_shown(int i)
{
    return i != ITEM_LOCK || (muse_lock_has_pin() && !muse_lock_locked());
}

/* The next item shown, `step` (+1 or -1) on from s_sel, round the ends. */
static void step_sel(int step)
{
    for (int n = 0; n < ITEM_COUNT; n++) {
        s_sel = (s_sel + step + ITEM_COUNT) % ITEM_COUNT;
        if (item_shown(s_sel)) {
            return;
        }
    }
}

static void close_menu(void)
{
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
    s_shown = false;
    s_open = false;
}

static void open_menu(float now)
{
    for (int i = 0; i < ITEM_COUNT; i++) {
        lv_obj_set_flag(s_items[i], LV_OBJ_FLAG_HIDDEN, !item_shown(i));
    }
    s_sel = item_shown(ITEM_LOCK) ? ITEM_LOCK : ITEM_SLEEP;
    highlight();
    lv_obj_remove_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
    s_shown = true;
    s_open = true;
    s_idle_until = now + IDLE_CLOSE_S;
    muse_voice_earcon(MUSE_EARCON_CLICK);
}

static void select_item(int item)
{
    static const char *const NAMES[ITEM_COUNT] = { "lock", "sleep", "power off", "restart", "cancel" };
    ESP_LOGI(TAG, "%s", NAMES[item]);
    close_menu();
    switch (item) {
    case ITEM_LOCK:
        muse_lock_now("power menu");
        break;
    case ITEM_SLEEP:
        muse_state_set_asleep(true);
        break;
    case ITEM_POWER_OFF:
        muse_input_request_power_off();   /* the goodbye, then the board's power_off */
        break;
    case ITEM_RESTART:
        esp_restart();
        break;
    default:
        break;
    }
}

static void on_item(lv_event_t *e)
{
    select_item((int)(intptr_t)lv_event_get_user_data(e));
}

static void on_backdrop(lv_event_t *e)
{
    (void)e;
    close_menu();   /* a tap beside the card */
}

static void build_menu(lv_obj_t *layer, int w)
{
    (void)w;   /* muse_style_backdrop keeps the card inside the screen */
    /* Dims the face and takes every tap while the menu is up: a dialog's
     * card (muse_dialog.h), its items the dialog's options. */
    lv_obj_t *card;
    s_backdrop = muse_style_backdrop(layer, CARD_W, &card);
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_backdrop, on_backdrop, LV_EVENT_CLICKED, NULL);

    muse_style_label(card, MUSE_FONT_CARD_TITLE, MUSE_COLOR_TEXT, "Power");

    for (int i = 0; i < ITEM_COUNT; i++) {
        lv_obj_t *b = muse_style_row(card, true, true);
        lv_obj_set_height(b, MUSE_BUTTON_H);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_border_color(b, lv_color_hex(MUSE_COLOR_ACCENT), 0);
        lv_obj_add_event_cb(b, on_item, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_set_style_pad_column(b, 10, 0);
        if (i == ITEM_LOCK) {
            muse_style_padlock(b, 18, ITEM_COLOR[i]);   /* as the others' symbols, before the words */
        }
        muse_style_label(b, MUSE_FONT_BUTTON, ITEM_COLOR[i], ITEM_TEXT[i]);
        s_items[i] = b;
    }

    s_hint = muse_style_label(card, FONT_HINT, MUSE_COLOR_DIM, "");
    lv_obj_set_width(s_hint, lv_pct(100));
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_MODE_WRAP);
    muse_power_menu_set_turn(0);
}

/* A card as the dialogs' are, a little see-through, over the top of the face. */
static void build_volume(lv_obj_t *layer, int w, int h)
{
    s_volume = lv_obj_create(layer);
    lv_obj_remove_style_all(s_volume);
    lv_obj_set_size(s_volume, w - 32 < 280 ? w - 32 : 280, LV_SIZE_CONTENT);
    lv_obj_align(s_volume, LV_ALIGN_TOP_MID, 0, h / 6);
    muse_style_card(s_volume);
    lv_obj_set_style_bg_opa(s_volume, LV_OPA_90, 0);
    lv_obj_set_style_pad_row(s_volume, 10, 0);
    lv_obj_set_flex_flow(s_volume, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_volume, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_volume, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_volume, LV_OBJ_FLAG_HIDDEN);

    s_volume_lbl = muse_style_label(s_volume, MUSE_FONT_CARD_TITLE, MUSE_COLOR_TEXT, "");

    s_volume_bar = lv_bar_create(s_volume);
    lv_obj_set_size(s_volume_bar, lv_pct(100), 12);
    lv_bar_set_range(s_volume_bar, 0, 100);
    lv_obj_set_style_bg_color(s_volume_bar, lv_color_hex(MUSE_COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_volume_bar, lv_color_hex(MUSE_COLOR_ACCENT), LV_PART_INDICATOR);
}

void muse_power_menu_set_turn(int quarters)
{
    /* Turned over, the keys are along the bottom, in the other order; on its
     * side, they're named rather than placed. */
    static const char *const HINTS[4] = {
        "Left key up, middle key down,\nright key selects",
        "BOOT key up, PWR key down,\ntalk key selects",
        "Right key up, middle key down,\nleft key selects",
        "BOOT key up, PWR key down,\ntalk key selects",
    };
    lv_label_set_text(s_hint, HINTS[quarters & 3]);
}

void muse_power_menu_build(lv_obj_t *layer, int w, int h)
{
    s_keys = xQueueCreate(KEY_QUEUE, sizeof(uint8_t));
    build_volume(layer, w, h);
    build_menu(layer, w);
}

static void show_volume(float now, bool asleep)
{
    uint32_t seq = s_volume_seq;
    if (seq != s_volume_shown_seq) {
        s_volume_shown_seq = seq;
        if (!asleep) {
            int pct = s_volume_pct;
            char text[48];
            if (muse_settings_speaker_on()) {
                snprintf(text, sizeof(text), LV_SYMBOL_VOLUME_MAX "  %d%%", pct);
            } else {
                snprintf(text, sizeof(text), LV_SYMBOL_MUTE "  Muted");
            }
            lv_label_set_text(s_volume_lbl, text);
            lv_bar_set_value(s_volume_bar, pct, LV_ANIM_OFF);
            lv_obj_remove_flag(s_volume, LV_OBJ_FLAG_HIDDEN);
            s_volume_until = now + VOLUME_SHOW_S;
        }
    }
    if (!lv_obj_has_flag(s_volume, LV_OBJ_FLAG_HIDDEN) && (asleep || now > s_volume_until)) {
        lv_obj_add_flag(s_volume, LV_OBJ_FLAG_HIDDEN);
    }
}

void muse_power_menu_tick(float now)
{
    if (!s_backdrop) {
        return;
    }
    bool asleep = muse_state_asleep();
    uint8_t k;
    while (xQueueReceive(s_keys, &k, 0) == pdTRUE) {
        if (asleep) {
            if (k == MUSE_POWER_MENU_OPEN) {
                s_open = false;   /* the screen went dark before it opened */
            }
            continue;
        }
        s_idle_until = now + IDLE_CLOSE_S;
        switch ((muse_power_menu_key_t)k) {
        case MUSE_POWER_MENU_OPEN:
            if (!s_shown) {
                open_menu(now);
            }
            break;
        case MUSE_POWER_MENU_UP:
        case MUSE_POWER_MENU_DOWN:
            if (s_shown) {
                step_sel(k == MUSE_POWER_MENU_UP ? -1 : 1);
                highlight();
            }
            break;
        case MUSE_POWER_MENU_SELECT:
            if (s_shown) {
                select_item(s_sel);
            }
            break;
        case MUSE_POWER_MENU_CLOSE:
            if (s_shown) {
                close_menu();
            }
            break;
        }
    }
    if (s_shown && (asleep || now > s_idle_until)) {
        close_menu();
    }
    show_volume(now, asleep);
}
