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

#include "muse_gadget_mode.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "muse_chat.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_wifi.h"

static const char *TAG = "muse_mode";

#define TICK_MS 2000            /* Wi-Fi and the pending message */
#define SCHEDULE_TICKS 30       /* the schedule: every minute */
#define NIGHT_FROM_H 21
#define NIGHT_TO_H 5
#define CLOCK_VALID_YEAR 2025   /* before this, the clock was never set */
#define NIGHT_BRIGHTNESS 20     /* at most */
#define TOAST_S 10.0f

#define COLOR_TEXT 0xf2efff
#define COLOR_DIM 0x8b84a8
#define COLOR_CARD 0x1a1530
#define COLOR_ACCENT 0xa77dff
#define COLOR_NIGHT 0x7d8cff
#define COLOR_AWAY 0xffb45c

static const char *const NAMES[MUSE_GADGET_MODE_COUNT] = {
    [MUSE_GADGET_DESK] = "Desk",
    [MUSE_GADGET_NIGHT] = "Night",
    [MUSE_GADGET_ON_THE_GO] = "On-the-go",
};

static const char *const KEYS[MUSE_GADGET_MODE_COUNT] = {
    [MUSE_GADGET_DESK] = "desk",
    [MUSE_GADGET_NIGHT] = "night",
    [MUSE_GADGET_ON_THE_GO] = "on_the_go",
};

/* What the Muse is told when the mode changes to each. */
static const char *const CONTRACTS[MUSE_GADGET_MODE_COUNT] = {
    [MUSE_GADGET_DESK] =
        "[gadget mode: DESK] DESK mode is on. Be proactive: offer feedback, suggest follow-ups, "
        "full detail is fine.",
    [MUSE_GADGET_NIGHT] =
        "[gadget mode: NIGHT] NIGHT mode is on. Default to backgrounding: anything that takes real "
        "work, run as a background task and summarize in the morning; keep this chat to one-line "
        "acknowledgments. Do not speak replies aloud.",
    [MUSE_GADGET_ON_THE_GO] =
        "[gadget mode: ON-THE-GO] ON-THE-GO mode is on. Keep replies to two sentences max. "
        "Captions only — no spoken replies unless explicitly asked. Do not send images unless "
        "explicitly asked (hotspot data).",
};

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile int s_pending = -1;         /* the mode whose message is still to go, or -1 */
static volatile uint32_t s_suggest_seq;     /* bumped to offer On-the-go */

/* LVGL task only. */
static lv_obj_t *s_chip;
static lv_obj_t *s_toast;
static int s_shown_mode = -1;
static uint32_t s_shown_suggest;
static float s_toast_until;

muse_gadget_mode_t muse_gadget_mode(void)
{
    return (muse_gadget_mode_t)muse_settings_gadget_mode();
}

const char *muse_gadget_mode_name(muse_gadget_mode_t mode)
{
    return mode < MUSE_GADGET_MODE_COUNT ? NAMES[mode] : "?";
}

bool muse_gadget_mode_parse(const char *name, muse_gadget_mode_t *out)
{
    for (int m = 0; name && m < MUSE_GADGET_MODE_COUNT; m++) {
        if (!strcmp(name, KEYS[m])) {
            *out = (muse_gadget_mode_t)m;
            return true;
        }
    }
    return false;
}

bool muse_gadget_tts_allowed(void)
{
    return muse_gadget_mode() == MUSE_GADGET_DESK;
}

int muse_gadget_mode_brightness(int pct)
{
    switch (muse_gadget_mode()) {
    case MUSE_GADGET_NIGHT:
        return pct < NIGHT_BRIGHTNESS ? pct : NIGHT_BRIGHTNESS;
    case MUSE_GADGET_ON_THE_GO:
        return 100;   /* outdoors */
    default:
        return pct;
    }
}

/* Local time, if the clock has been set (SNTP or an RTC). */
static bool clock_valid(time_t *now, struct tm *tm)
{
    *now = time(NULL);
    localtime_r(now, tm);
    return tm->tm_year + 1900 >= CLOCK_VALID_YEAR;
}

static muse_gadget_mode_t scheduled(const struct tm *tm)
{
    return tm->tm_hour >= NIGHT_FROM_H || tm->tm_hour < NIGHT_TO_H ? MUSE_GADGET_NIGHT : MUSE_GADGET_DESK;
}

/* On the On-the-go network (muse_gadget_mode_set_away()) now. */
static bool on_away_network(void)
{
    char away[MUSE_SSID_MAX + 1];
    muse_settings_away_ssid(away);
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    return away[0] && w.state == MUSE_WIFI_CONNECTED && !strcmp(w.ssid, away);
}

/* The schedule's next switch after `now`: the coming 05:00 or 21:00. */
static uint32_t next_boundary(time_t now)
{
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    if (tm.tm_hour < NIGHT_TO_H) {
        tm.tm_hour = NIGHT_TO_H;
    } else if (tm.tm_hour < NIGHT_FROM_H) {
        tm.tm_hour = NIGHT_FROM_H;
    } else {
        tm.tm_hour = NIGHT_TO_H;
        tm.tm_mday++;   /* mktime carries it into the next month */
    }
    return (uint32_t)mktime(&tm);
}

/* With s_lock held. */
static void apply(muse_gadget_mode_t mode, const char *why)
{
    if (mode == muse_gadget_mode()) {
        return;
    }
    ESP_LOGI(TAG, "%s mode (%s)", NAMES[mode], why);
    muse_settings_set_gadget_mode(mode);
    muse_state_set_caption("%s MODE", mode == MUSE_GADGET_DESK ? "DESK" : mode == MUSE_GADGET_NIGHT ? "NIGHT" : "ON-THE-GO");
    s_pending = mode;   /* the latest change is the one the Muse needs */
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void muse_gadget_mode_pick(muse_gadget_mode_t mode)
{
    if (mode >= MUSE_GADGET_MODE_COUNT || !s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    time_t now;
    struct tm tm;
    /* Without the clock, the hold starts counting once it's set (check_schedule). */
    muse_settings_set_mode_override(true, clock_valid(&now, &tm) ? next_boundary(now) : 0);
    apply(mode, "picked");
    xSemaphoreGive(s_lock);
}

/*
 * The chats told which mode since boot, so switching back to one doesn't tell
 * it again (each told message stays in that chat's history). With s_lock held.
 */
#define TOLD_MAX (MUSE_CHATS_MAX + 4)
static struct {
    char sid[MUSE_CHAT_SID_MAX + 1];   /* "" for the main chat */
    int8_t mode;
} s_told[TOLD_MAX];
static int s_told_n;

static int told_mode(const char *sid)
{
    for (int i = 0; i < s_told_n; i++) {
        if (!strcmp(s_told[i].sid, sid)) {
            return s_told[i].mode;
        }
    }
    return -1;
}

static void set_told(const char *sid, int mode)
{
    int i = 0;
    while (i < s_told_n && strcmp(s_told[i].sid, sid)) {
        i++;
    }
    if (i == s_told_n) {
        if (s_told_n == TOLD_MAX) {
            memmove(&s_told[0], &s_told[1], sizeof(s_told[0]) * (TOLD_MAX - 1));   /* full: the oldest goes */
            i = TOLD_MAX - 1;
        } else {
            s_told_n++;
        }
        strlcpy(s_told[i].sid, sid, sizeof(s_told[i].sid));
    }
    s_told[i].mode = (int8_t)mode;
}

void muse_gadget_mode_resend(void)
{
    if (!s_lock) {
        return;
    }
    muse_gadget_mode_t mode = muse_gadget_mode();
    char sid[MUSE_CHAT_SID_MAX + 1];
    muse_settings_chat_sid(sid);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (told_mode(sid) == (int)mode) {
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "%s mode: this chat was told already", NAMES[mode]);
        return;
    }
    s_pending = mode;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s mode: telling the Muse again", NAMES[mode]);
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

bool muse_gadget_mode_set_away(void)
{
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    if (w.state != MUSE_WIFI_CONNECTED || !w.ssid[0]) {
        return false;
    }
    muse_settings_set_away_ssid(w.ssid);
    return true;
}

void muse_gadget_mode_clear_away(void)
{
    muse_settings_set_away_ssid("");
}

bool muse_gadget_mode_set_home(void)
{
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    if (w.state != MUSE_WIFI_CONNECTED || !w.ssid[0]) {
        return false;
    }
    muse_settings_set_home_ssid(w.ssid);
    return true;
}

static void check_schedule(void)
{
    time_t now;
    struct tm tm;
    bool away = on_away_network();
    bool timed = clock_valid(&now, &tm);
    if (!timed && !away) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t until;
    bool hold = muse_settings_mode_override(&until);
    if (!timed) {
        /* The hotspot is known even before the clock is. */
        if (!hold) {
            apply(MUSE_GADGET_ON_THE_GO, "on-the-go network");
        }
        xSemaphoreGive(s_lock);
        return;
    }
    if (hold && !until) {
        /* Picked before the clock was set: hold until the next boundary from now. */
        muse_settings_set_mode_override(true, next_boundary(now));
    } else if (hold && (uint32_t)now >= until) {
        ESP_LOGI(TAG, "picked mode ends: back to the schedule");
        muse_settings_set_mode_override(false, 0);
        hold = false;
    }
    if (!hold) {
        apply(away ? MUSE_GADGET_ON_THE_GO : scheduled(&tm), away ? "on-the-go network" : "schedule");
    }
    xSemaphoreGive(s_lock);
}

/*
 * Joining or leaving the On-the-go network switches straight away, ending a
 * mode picked by hand; another network away from home just offers it, once
 * per network joined.
 */
static void check_network(void)
{
    static char offered[MUSE_SSID_MAX + 1];
    static int was_away = -1;
    int away = on_away_network();
    if (away != was_away) {
        if (was_away >= 0) {
            ESP_LOGI(TAG, "%s the on-the-go network", away ? "joined" : "left");
            xSemaphoreTake(s_lock, portMAX_DELAY);
            muse_settings_set_mode_override(false, 0);
            xSemaphoreGive(s_lock);
        }
        was_away = away;
        check_schedule();
    }
    char home[MUSE_SSID_MAX + 1], away_ssid[MUSE_SSID_MAX + 1];
    muse_settings_home_ssid(home);
    muse_settings_away_ssid(away_ssid);
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    if (away_ssid[0] || !home[0] || w.state != MUSE_WIFI_CONNECTED || !w.ssid[0]) {
        return;   /* with an On-the-go network set, that's the signal instead */
    }
    if (!strcmp(w.ssid, home)) {
        offered[0] = '\0';   /* home again: leaving offers it again */
        return;
    }
    if (muse_gadget_mode() == MUSE_GADGET_ON_THE_GO || !strcmp(w.ssid, offered)) {
        return;
    }
    strlcpy(offered, w.ssid, sizeof(offered));
    ESP_LOGI(TAG, "away from the home network: offering On-the-go");
    s_suggest_seq++;
}

/* The pending mode's message, once the Muse can take a typed turn: one isn't
 * taken while a voice turn runs, so wait those out. */
static void send_pending(void)
{
    int mode = s_pending;
    if (mode < 0) {
        return;
    }
#if CONFIG_MUSE_HATCH
    float t;
    muse_mode_t face = muse_state_mode(&t);
    if (!muse_hatch_ready() || face == MUSE_MODE_LISTENING || face == MUSE_MODE_THINKING
        || face == MUSE_MODE_SPEAKING) {
        return;   /* try again next tick */
    }
    char *text = strdup(CONTRACTS[mode]);
    if (!text) {
        return;
    }
    char sid[MUSE_CHAT_SID_MAX + 1];
    muse_settings_chat_sid(sid);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool current = s_pending == mode;
    if (current) {
        s_pending = -1;
        set_told(sid, mode);
    }
    xSemaphoreGive(s_lock);
    if (!current) {
        free(text);   /* changed again meanwhile: send that one next */
        return;
    }
    ESP_LOGI(TAG, "telling the Muse: %s mode", NAMES[mode]);
    muse_hatch_text_turn(text);   /* frees it */
#else
    ESP_LOGW(TAG, "%s mode: this build can't send the Muse a typed turn", NAMES[mode]);
    s_pending = -1;
#endif
}

static void mode_task(void *arg)
{
    (void)arg;
    for (int tick = 0;; tick++) {
        if (tick % SCHEDULE_TICKS == 0) {
            check_schedule();
        }
        check_network();
        send_pending();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS));
    }
}

void muse_gadget_mode_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "%s mode%s", NAMES[muse_gadget_mode()], muse_settings_mode_override(NULL) ? " (picked)" : "");
    /* Internal stack: it writes NVS, which can't run with a stack in PSRAM. */
    if (!s_lock || xTaskCreate(mode_task, "muse_mode", 3072, NULL, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "couldn't start the mode schedule");
    }
}

/* ---------- LVGL task ---------- */

static void on_toast(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    muse_gadget_mode_pick(MUSE_GADGET_ON_THE_GO);
}

void muse_gadget_mode_build_chip(lv_obj_t *status)
{
    s_chip = lv_label_create(status);
    lv_obj_set_style_text_font(s_chip, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_chip, "");
}

void muse_gadget_mode_build_toast(lv_obj_t *layer, int w)
{
    s_toast = lv_button_create(layer);
    lv_obj_remove_style_all(s_toast);
    lv_obj_set_size(s_toast, w - 32 < 340 ? w - 32 : 340, LV_SIZE_CONTENT);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -64);
    lv_obj_set_flex_flow(s_toast, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_toast, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_toast, 12, 0);
    lv_obj_set_style_pad_row(s_toast, 4, 0);
    lv_obj_set_style_radius(s_toast, 20, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(COLOR_AWAY), 0);
    lv_obj_set_style_border_width(s_toast, 2, 0);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_toast, on_toast, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = lv_label_create(s_toast);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COLOR_TEXT), 0);
    lv_label_set_text(l, "Not on your home Wi-Fi");
    l = lv_label_create(s_toast);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COLOR_AWAY), 0);
    lv_label_set_text(l, LV_SYMBOL_GPS "  Tap for On-the-go mode");
    s_shown_suggest = s_suggest_seq;
}

void muse_gadget_mode_ui_tick(float now)
{
    muse_gadget_mode_t mode = muse_gadget_mode();
    if (s_chip && (int)mode != s_shown_mode) {
        static const char *const CHIPS[MUSE_GADGET_MODE_COUNT] = {
            [MUSE_GADGET_DESK] = LV_SYMBOL_HOME " DESK",
            [MUSE_GADGET_NIGHT] = LV_SYMBOL_EYE_CLOSE " NIGHT",
            [MUSE_GADGET_ON_THE_GO] = LV_SYMBOL_GPS " ON-THE-GO",
        };
        static const uint32_t COLORS[MUSE_GADGET_MODE_COUNT] = {
            [MUSE_GADGET_DESK] = COLOR_DIM,
            [MUSE_GADGET_NIGHT] = COLOR_NIGHT,
            [MUSE_GADGET_ON_THE_GO] = COLOR_AWAY,
        };
        lv_label_set_text(s_chip, CHIPS[mode]);
        lv_obj_set_style_text_color(s_chip, lv_color_hex(COLORS[mode]), 0);
        s_shown_mode = mode;
    }
    if (!s_toast) {
        return;
    }
    uint32_t seq = s_suggest_seq;
    if (seq != s_shown_suggest) {
        /* Only called awake, so an offer made asleep shows on waking. */
        s_shown_suggest = seq;
        lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_until = now + TOAST_S;
    }
    if (!lv_obj_has_flag(s_toast, LV_OBJ_FLAG_HIDDEN)
        && (now > s_toast_until || mode == MUSE_GADGET_ON_THE_GO)) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    }
}
