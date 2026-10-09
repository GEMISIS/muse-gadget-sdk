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

#include "muse_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_attr.h"
#include "muse_lock.h"
#include "muse_text.h"

#define HAPPY_SECS 1.6f

#define AWAKE_BIT BIT0   /* while !s_asleep */
#define NUDGE_BIT BIT1

static volatile muse_mode_t s_mode = MUSE_MODE_BOOT;
static volatile int64_t s_mode_since_us;
static volatile muse_turn_t s_turn;
static volatile float s_level;
static volatile float s_progress;
static volatile int64_t s_last_poke_us;
static volatile int64_t s_happy_until_us;
static volatile bool s_asleep;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_caption[MUSE_CAPTION_MAX];
static uint32_t s_caption_version;
static SemaphoreHandle_t s_format_lock;
static EventGroupHandle_t s_wake;
static volatile int s_page_cols = 16, s_page_lines = 2;
static volatile int s_cjk_cols, s_cjk_lines;
static muse_power_t s_power = { .battery_pct = -1 };
static volatile bool s_as_if_battery;
EXT_RAM_BSS_ATTR static volatile int s_activity;   /* muse_activity_t; in PSRAM, as internal RAM is short */
/* The reply (muse_state_set_reply): its text with stand-ins, and which caption it's with. */
EXT_RAM_BSS_ATTR static char *s_reply;              /* MUSE_REPLY_MAX, PSRAM; NULL without */
EXT_RAM_BSS_ATTR static uint32_t s_reply_hash;      /* of the text as given, to skip it unchanged */
EXT_RAM_BSS_ATTR static size_t s_reply_given;
EXT_RAM_BSS_ATTR static uint32_t s_reply_version;
EXT_RAM_BSS_ATTR static uint32_t s_reply_for;       /* the caption's version it was set with */
EXT_RAM_BSS_ATTR static size_t s_reply_at;
EXT_RAM_BSS_ATTR static bool s_reply_spoken;

static float secs_since(int64_t us)
{
    return (float)(esp_timer_get_time() - us) / 1e6f;
}

void muse_state_init(void)
{
    static StaticSemaphore_t lock;
    s_format_lock = xSemaphoreCreateMutexStatic(&lock);
    static StaticEventGroup_t wake;
    s_wake = xEventGroupCreateStatic(&wake);
    xEventGroupSetBits(s_wake, AWAKE_BIT);
    int64_t now = esp_timer_get_time();
    s_mode_since_us = now;
    s_last_poke_us = now;
    s_reply = heap_caps_calloc(1, MUSE_REPLY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_reply_for = UINT32_MAX;
}

void muse_state_set_mode(muse_mode_t mode)
{
    if (mode == s_mode) {
        return;
    }
    /* Shutting down wins over the voice pipeline finishing a turn. */
    if (s_mode == MUSE_MODE_OFF && mode != MUSE_MODE_IDLE) {
        return;
    }
    s_mode_since_us = esp_timer_get_time();
    s_turn = MUSE_TURN_SENDING;
    s_mode = mode;
}

void muse_state_set_turn(muse_turn_t turn)
{
    s_turn = turn;
}

muse_turn_t muse_state_turn(void)
{
    return s_turn;
}

void muse_state_set_activity(muse_activity_t activity)
{
    s_activity = activity;
}

muse_activity_t muse_state_activity(void)
{
    return (muse_activity_t)s_activity;
}

muse_mode_t muse_state_mode(float *secs_in_mode)
{
    if (secs_in_mode) {
        *secs_in_mode = secs_since(s_mode_since_us);
    }
    return s_mode;
}

void muse_state_set_level(float level)
{
    s_level = level < 0 ? 0 : (level > 1 ? 1 : level);
}

float muse_state_level(void)
{
    return s_level;
}

void muse_state_set_progress(float progress)
{
    s_progress = progress < 0 ? 0 : (progress > 1 ? 1 : progress);
}

float muse_state_progress(void)
{
    return s_progress;
}

static char s_caption_buf[sizeof(s_caption)];   /* too big for some callers' stacks; under s_format_lock */

/* s_caption_buf as the caption; with s_format_lock held. */
static void caption_from_buf(void)
{
    muse_text_to_ascii(s_caption_buf, sizeof(s_caption_buf));   /* replies have curly quotes and dashes */
    portENTER_CRITICAL(&s_lock);
    if (strcmp(s_caption_buf, s_caption) != 0) {
        memcpy(s_caption, s_caption_buf, sizeof(s_caption));
        s_caption_version++;
    }
    portEXIT_CRITICAL(&s_lock);
}

void muse_state_set_caption(const char *fmt, ...)
{
    xSemaphoreTake(s_format_lock, portMAX_DELAY);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_caption_buf, sizeof(s_caption_buf), fmt, ap);
    va_end(ap);
    caption_from_buf();
    xSemaphoreGive(s_format_lock);
}

static uint32_t hash(const char *s, size_t *len)
{
    uint32_t h = 2166136261u;   /* FNV-1a */
    const char *p = s;
    for (; *p; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    *len = (size_t)(p - s);
    return h;
}

void muse_state_set_reply(const char *page, const char *text, size_t at, bool spoken)
{
    xSemaphoreTake(s_format_lock, portMAX_DELAY);
    strlcpy(s_caption_buf, page, sizeof(s_caption_buf));
    caption_from_buf();
    if (s_reply) {
        size_t given;
        uint32_t h = hash(text, &given);
        bool changed = h != s_reply_hash || given != s_reply_given;
        if (changed) {
            strlcpy(s_reply, text, MUSE_REPLY_MAX);
            muse_text_to_ascii(s_reply, MUSE_REPLY_MAX);
            s_reply_hash = h;
            s_reply_given = given;
        }
        /* `at` in the text with its stand-ins. */
        size_t shown = 0;
        for (const char *p = text; *p && (size_t)(p - text) < at;) {
            size_t len;
            char a[4];
            int n = muse_text_ascii(p, &len, a);
            shown += n < 0 ? len : (size_t)n;
            p += len;
        }
        size_t max = strlen(s_reply);
        portENTER_CRITICAL(&s_lock);
        s_reply_version += changed;
        s_reply_at = shown < max ? shown : max;
        s_reply_spoken = spoken;
        s_reply_for = s_caption_version;
        portEXIT_CRITICAL(&s_lock);
    }
    xSemaphoreGive(s_format_lock);
}

bool muse_state_reply(char *out, size_t out_len, uint32_t *version, size_t *at, bool *spoken)
{
    if (!s_reply) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    bool live = s_reply_for == s_caption_version;
    bool changed = *version != s_reply_version;
    *at = s_reply_at;
    *spoken = s_reply_spoken;
    portEXIT_CRITICAL(&s_lock);
    if (live && changed) {
        xSemaphoreTake(s_format_lock, portMAX_DELAY);   /* a few KB: not with interrupts off */
        strlcpy(out, s_reply, out_len);
        *version = s_reply_version;
        xSemaphoreGive(s_format_lock);
    }
    return live;
}

bool muse_state_caption(char *out, size_t out_len, uint32_t *version)
{
    bool changed = false;
    portENTER_CRITICAL(&s_lock);
    if (*version != s_caption_version) {
        strlcpy(out, s_caption, out_len);
        *version = s_caption_version;
        changed = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return changed;
}

void muse_state_set_page(int cols, int lines)
{
    s_page_cols = cols;
    s_page_lines = lines;
}

void muse_state_set_cjk_page(int cols, int lines)
{
    s_cjk_cols = cols;
    s_cjk_lines = lines;
}

void muse_state_page(bool cjk, int *cols, int *lines)
{
    bool own = cjk && s_cjk_cols > 0;
    *cols = own ? s_cjk_cols : s_page_cols;
    *lines = own ? s_cjk_lines : s_page_lines;
}

void muse_state_set_power(const muse_power_t *power)
{
    bool was = muse_state_on_battery();
    portENTER_CRITICAL(&s_lock);
    s_power = *power;
    portEXIT_CRITICAL(&s_lock);
    muse_lock_note_power(muse_state_on_battery());   /* asleep: the power it slept on */
    if (muse_state_on_battery() != was) {
        muse_state_nudge();
    }
}

muse_power_t muse_state_power(void)
{
    portENTER_CRITICAL(&s_lock);
    muse_power_t p = s_power;
    portEXIT_CRITICAL(&s_lock);
    return p;
}

bool muse_state_on_battery(void)
{
    muse_power_t p = muse_state_power();
    return s_as_if_battery || (!p.usb && p.battery_pct >= 0);
}

void muse_state_set_as_if_battery(bool on)
{
    if (on != s_as_if_battery) {
        s_as_if_battery = on;
        muse_state_nudge();
    }
}

void muse_state_poke(void)
{
    s_last_poke_us = esp_timer_get_time();
}

float muse_state_idle_secs(void)
{
    return secs_since(s_last_poke_us);
}

void muse_state_set_asleep(bool asleep)
{
    muse_lock_note_asleep(asleep);   /* first: waking after long enough, it wakes locked */
    if (!asleep) {
        muse_state_poke();
    }
    s_asleep = asleep;
    if (asleep) {
        xEventGroupClearBits(s_wake, AWAKE_BIT);
    } else {
        xEventGroupSetBits(s_wake, AWAKE_BIT);
    }
}

bool muse_state_asleep(void)
{
    return s_asleep;
}

void muse_state_wait_awake(uint32_t timeout_ms)
{
    xEventGroupWaitBits(s_wake, AWAKE_BIT | NUDGE_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    xEventGroupClearBits(s_wake, NUDGE_BIT);
}

void muse_state_nudge(void)
{
    xEventGroupSetBits(s_wake, NUDGE_BIT);
}

void muse_state_make_happy(void)
{
    s_happy_until_us = esp_timer_get_time() + (int64_t)(HAPPY_SECS * 1e6f);
    muse_state_poke();
}

float muse_state_happiness(void)
{
    float left = (float)(s_happy_until_us - esp_timer_get_time()) / 1e6f;
    if (left <= 0) {
        return 0;
    }
    /* Ease out over the last 0.4 s. */
    return left > 0.4f ? 1.0f : left / 0.4f;
}
