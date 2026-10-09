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
 * The face's "up next" line (muse_up_next.h): when to ask the Muse, and the
 * answer kept. The asking itself is muse_chat_session.cpp's background
 * request, each time in a fresh chat that's never picked or listed; that ask
 * also has the Muse delete the one before, so only the latest is left on the
 * Muse. The answer, its time and that chat's id are kept in NVS ("gadget"),
 * so a restart shows the line until STALE_US and deletes the chat next time.
 */
#include "muse_up_next.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "esp_random.h"
#include "nvs.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "muse_chat.h"
#include "muse_extras.h"
#include "muse_gadget_mode.h"
#include "muse_lock.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_text.h"

static const char *TAG = "up_next";

#define PROMPT                                                                                                     \
    "In one short line (40 characters max), what should I prepare for next today? Use my calendar and plans if "  \
    "you know them. Reply with just the line, no preamble."
/* After PROMPT when a line's showing: kept, word for word, unless it needs to change. */
#define KEEP " Right now I show: \"%s\". If that's still right, reply with just the word SAME instead."
#define SAME "SAME"
/* Ahead of PROMPT when an earlier ask's chat is still on the Muse. */
#define DELETE_FIRST "First, quietly delete the chat with session id %s (an earlier one of these questions); don't mention it. Then: "
#define NVS_NS "gadget"
#define UP_NEXT_MAX 72
#define EVERY_US (3600LL * 1000000)            /* asked at most this often, failures included */
#define FIRST_AFTER_US (30LL * 1000000)        /* from boot: Wi-Fi and the connection settle first */
#define STALE_US (3LL * 3600 * 1000000)        /* an answer this old isn't shown */
#define MORNING_AWAKE_US (10LL * 1000000)      /* the screen on this long, mornings, before asking */
#define MORNING_UNTIL_MIN (12 * 60)            /* mornings end at noon */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static char s_line[UP_NEXT_MAX];   /* with s_lock */
EXT_RAM_BSS_ATTR static char s_full[256];           /* the whole reply the line came from, with s_lock */
static int64_t s_line_us;         /* when it came; 0 for none */

/* Extras task only. */
static int64_t s_asked_us;        /* the last ask; 0 for none yet */
static bool s_asking;
static int64_t s_awake_since;
static int s_morning_day = -1;    /* year * 1000 + day of the year of the last morning ask */
static volatile bool s_force;     /* ">brief" */
static volatile int64_t s_due_us; /* asked again then, after a turn; 0 for none */
#define AFTER_TURN_US (20LL * 1000000)     /* after a turn, once no other's come this long */
#define AFTER_TURN_MIN_US (60LL * 1000000) /* but not more often than this */

/* Extras task only: the chat this ask is in, and the last answered one's, to delete. */
static char s_sid[MUSE_CHAT_SID_MAX + 1];
static char s_prev[MUSE_CHAT_SID_MAX + 1];
static bool s_loaded;

/* The fixed chat earlier firmware asked in (a UUID made from the Wi-Fi MAC
 * with "muse", "up" ahead): the first one to delete after an update. */
static void old_chat_sid(char out[MUSE_CHAT_SID_MAX + 1])
{
    uint8_t m[6] = { 0 };
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(out, MUSE_CHAT_SID_MAX + 1, "6d757365-7570-4e78-8000-%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3],
             m[4], m[5]);
}

/* A fresh random (v4) UUID for each ask. */
static void new_chat_sid(char out[MUSE_CHAT_SID_MAX + 1])
{
    uint8_t u[16];
    esp_fill_random(u, sizeof(u));
    u[6] = (u[6] & 0x0f) | 0x40;
    u[8] = (u[8] & 0x3f) | 0x80;
    snprintf(out, MUSE_CHAT_SID_MAX + 1, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

/* Restores the kept line (if the clock says it's still fresh) and the chat to delete. */
static void load(void)
{
    s_loaded = true;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        old_chat_sid(s_prev);
        return;
    }
    size_t n = sizeof(s_prev);
    if (nvs_get_str(h, "un_prev", s_prev, &n) != ESP_OK) {
        old_chat_sid(s_prev);
    }
    char line[UP_NEXT_MAX];
    n = sizeof(line);
    int64_t at = 0;
    time_t wall = time(NULL);
    if (nvs_get_str(h, "un_line", line, &n) == ESP_OK && nvs_get_i64(h, "un_at", &at) == ESP_OK && at > 0
        && wall > at && (int64_t)(wall - at) * 1000000 < STALE_US) {
        int64_t age_us = (int64_t)(wall - at) * 1000000;
        int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&s_lock);
        strlcpy(s_line, line, sizeof(s_line));
        s_line_us = now > age_us ? now - age_us : 1;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "up next (kept): \"%s\"", line);
    }
    nvs_close(h);
}

/* The answer, when it came, and its chat as the next one to delete. */
static void save(const char *line)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, "un_prev", s_prev);
    nvs_set_str(h, "un_line", line);
    time_t wall = time(NULL);
    nvs_set_i64(h, "un_at", wall > 1735689600 ? (int64_t)wall : 0);   /* 0 if the clock isn't set */
    nvs_commit(h);
    nvs_close(h);
}

/*
 * The reply as one line for the face: its first line with any text, without
 * list marks, emphasis or quotes around it, in the fonts' characters.
 */
static void tidy(const char *reply, char *out, size_t cap)
{
    const char *p = reply;
    while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') {
        p++;
    }
    size_t n = strcspn(p, "\r\n");
    while (n && strchr("-*#>\"'` \t", *p)) {   /* "- ", "**", "> ", a quote */
        p++;
        n--;
    }
    n = n < cap - 1 ? n : cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    while (n && strchr("*\"'` \t", out[n - 1])) {
        out[--n] = '\0';
    }
    muse_text_to_ascii(out, cap);
}

/* The first time the screen's been on a while since Night ended, before noon. */
static bool morning_due(int64_t now)
{
    if (!s_awake_since || now - s_awake_since < MORNING_AWAKE_US || !muse_time_valid()) {
        return false;
    }
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    int from, to, m = tm.tm_hour * 60 + tm.tm_min, day = tm.tm_year * 1000 + tm.tm_yday;
    muse_gadget_mode_night(&from, &to);
    (void)from;
    if (day == s_morning_day || m < to || m >= MORNING_UNTIL_MIN) {
        return false;
    }
    s_morning_day = day;
    return true;
}

static void take_reply(void)
{
    char reply[256];
    muse_chat_bg_state_t st = muse_chat_bg_result_for(MUSE_CHAT_BG_FOR_UP_NEXT, reply, sizeof(reply));
    if (st == MUSE_CHAT_BG_BUSY) {
        return;
    }
    s_asking = false;
    if (st != MUSE_CHAT_BG_DONE) {
        ESP_LOGW(TAG, "no answer this time: asking again in an hour");
        return;
    }
    char line[UP_NEXT_MAX];
    tidy(reply, line, sizeof(line));
    if (!strncasecmp(line, SAME, strlen(SAME)) && !line[strspn(line + strlen(SAME), ".! ") + strlen(SAME)]) {
        portENTER_CRITICAL(&s_lock);
        bool kept = s_line_us != 0;
        s_line_us = kept ? esp_timer_get_time() : 0;   /* still right: fresh again, word for word */
        strlcpy(line, s_line, sizeof(line));
        portEXIT_CRITICAL(&s_lock);
        strlcpy(s_prev, s_sid, sizeof(s_prev));
        save(kept ? line : "");
        ESP_LOGI(TAG, "up next: unchanged");
        return;
    }
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_line, line, sizeof(s_line));
    strlcpy(s_full, reply, sizeof(s_full));
    s_line_us = line[0] ? esp_timer_get_time() : 0;
    portEXIT_CRITICAL(&s_lock);
    strlcpy(s_prev, s_sid, sizeof(s_prev));   /* this chat goes with the next ask */
    save(line);
    ESP_LOGI(TAG, "up next: \"%s\"", line);
}

void muse_up_next_tick(void)
{
    if (!s_loaded && time(NULL) > 1735689600) {
        load();   /* once the clock's set: the kept line's age needs it */
    }
    if (s_asking) {
        take_reply();
        return;
    }
    int64_t now = esp_timer_get_time();
    bool awake = !muse_state_asleep();
    if (!awake) {
        s_awake_since = 0;
    } else if (!s_awake_since) {
        s_awake_since = now;
    }
    /* In reach, between turns, and not asleep on battery, when Wi-Fi rests:
     * waking makes up for it, as it's due by then. */
    if (!muse_hatch_ready() || muse_hatch_turn_busy() || muse_state_mode(NULL) != MUSE_MODE_IDLE || muse_lock_locked()
        || (!awake && muse_state_on_battery())) {
        return;
    }
    const char *why = s_force ? "asked for" : NULL;
    if (!why && s_due_us && now >= s_due_us) {
        s_due_us = 0;
        if (!s_asked_us || now - s_asked_us >= AFTER_TURN_MIN_US) {
            why = "after a turn";
        }
    }
    if (!why && now >= FIRST_AFTER_US && (!s_asked_us || now - s_asked_us >= EVERY_US)) {
        why = s_asked_us ? "hourly" : "first since boot";
    }
    if (!why && morning_due(now)) {
        why = "morning";
    }
    if (!why) {
        return;
    }
    s_force = false;
    s_asked_us = now;
    if (!s_loaded) {
        load();   /* no clock yet: the line can't be dated, but the chat to delete is known */
    }
    new_chat_sid(s_sid);
    static char ask[sizeof(DELETE_FIRST) + MUSE_CHAT_SID_MAX + sizeof(PROMPT) + sizeof(KEEP) + UP_NEXT_MAX];
    char shown[UP_NEXT_MAX];
    muse_up_next_line(shown, sizeof(shown));
    int n = s_prev[0] ? snprintf(ask, sizeof(ask), DELETE_FIRST "%s", s_prev, PROMPT) : snprintf(ask, sizeof(ask), "%s", PROMPT);
    if (shown[0] && n > 0 && (size_t)n < sizeof(ask)) {
        snprintf(ask + n, sizeof(ask) - n, KEEP, shown);
    }
    s_asking = muse_chat_bg_ask_for(MUSE_CHAT_BG_FOR_UP_NEXT, s_sid, ask);
    ESP_LOGI(TAG, "%s Muse what's up next (%s)", s_asking ? "asking" : "couldn't ask", why);
}

bool muse_up_next_line(char *out, size_t cap)
{
    if (!cap) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    bool fresh = s_line_us && now - s_line_us < STALE_US;
    strlcpy(out, fresh ? s_line : "", cap);
    portEXIT_CRITICAL(&s_lock);
    return fresh && out[0];
}

void muse_up_next_turn_text(const char *text)
{
    (void)text;
}

/* A turn's done: asked again once things go quiet, the turn restarting the wait. */
void muse_up_next_turn_done(void)
{
    s_due_us = esp_timer_get_time() + AFTER_TURN_US;
}

bool muse_up_next_full(char *out, size_t cap)
{
    if (!cap || !muse_up_next_line(out, cap)) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    if (s_full[0]) {
        strlcpy(out, s_full, cap);
    }
    portEXIT_CRITICAL(&s_lock);
    return true;
}

void muse_up_next_refresh(void)
{
    s_force = true;
}

void muse_up_next_print(void)
{
    char line[UP_NEXT_MAX];
    portENTER_CRITICAL(&s_lock);
    strlcpy(line, s_line, sizeof(line));
    int64_t at = s_line_us;
    portEXIT_CRITICAL(&s_lock);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "line", line);
    cJSON_AddNumberToObject(j, "age_s", at ? (double)((esp_timer_get_time() - at) / 1000000) : -1);
    cJSON_AddBoolToObject(j, "shown", muse_up_next_line(line, sizeof(line)));
    cJSON_AddBoolToObject(j, "asking", s_asking || s_force);
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (text) {
        printf("@brief %s\n", text);
        fflush(stdout);
        cJSON_free(text);
    }
}
