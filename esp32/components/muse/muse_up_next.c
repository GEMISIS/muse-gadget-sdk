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
 * request, in a chat of the gadget's own that's never picked or listed.
 */
#include "muse_up_next.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "muse_chat.h"
#include "muse_extras.h"
#include "muse_gadget_mode.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_text.h"

static const char *TAG = "up_next";

#define PROMPT                                                                                                     \
    "In one short line (max ~40 characters), what should I prepare for next today? Use my calendar and plans if " \
    "you know them. Reply with just the line, no preamble."
#define UP_NEXT_MAX 72
#define EVERY_US (3600LL * 1000000)            /* asked at most this often, failures included */
#define FIRST_AFTER_US (30LL * 1000000)        /* from boot: Wi-Fi and the connection settle first */
#define STALE_US (3LL * 3600 * 1000000)        /* an answer this old isn't shown */
#define MORNING_AWAKE_US (10LL * 1000000)      /* the screen on this long, mornings, before asking */
#define MORNING_UNTIL_MIN (12 * 60)            /* mornings end at noon */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static char s_line[UP_NEXT_MAX];   /* with s_lock */
static int64_t s_line_us;         /* when it came; 0 for none */

/* Extras task only. */
static int64_t s_asked_us;        /* the last ask; 0 for none yet */
static bool s_asking;
static int64_t s_awake_since;
static int s_morning_day = -1;    /* year * 1000 + day of the year of the last morning ask */
static volatile bool s_force;     /* ">brief" */

/* The hidden chat: a UUID made from the Wi-Fi MAC like the gadget's own
 * chat (muse_settings_gadget_chat_sid), with another prefix ("muse", "up"). */
static void chat_sid(char out[MUSE_CHAT_SID_MAX + 1])
{
    uint8_t m[6] = { 0 };
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(out, MUSE_CHAT_SID_MAX + 1, "6d757365-7570-4e78-8000-%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3],
             m[4], m[5]);
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
    muse_chat_bg_state_t st = muse_chat_bg_result(reply, sizeof(reply));
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
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_line, line, sizeof(s_line));
    s_line_us = line[0] ? esp_timer_get_time() : 0;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "up next: \"%s\"", line);
}

void muse_up_next_tick(void)
{
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
    if (!muse_hatch_ready() || muse_hatch_turn_busy() || muse_state_mode(NULL) != MUSE_MODE_IDLE
        || (!awake && muse_state_on_battery())) {
        return;
    }
    const char *why = s_force ? "asked for" : NULL;
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
    char sid[MUSE_CHAT_SID_MAX + 1];
    chat_sid(sid);
    s_asking = muse_chat_bg_ask(sid, PROMPT);
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
