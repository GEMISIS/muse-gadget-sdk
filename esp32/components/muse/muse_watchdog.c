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

/* The face's freeze watch (muse_watchdog.h). */
#include "muse_watchdog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "muse_state.h"

static const char *TAG = "muse_wdog";

#define CHECK_US (5 * 1000000LL)
#define FACE_STALL_US (20 * 1000000LL)     /* awake, and no frame drawn this long */
#define SCREEN_STALL_US (15 * 1000000LL)   /* a sleep or wake still going */
#define AWAKE_SETTLE_US (5 * 1000000LL)    /* after waking, before frames count */
#define STALL_MAGIC 0x57d06e11u

/* Kept through the abort's restart (not a power cut): why it was. */
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    char why[96];
} s_stall;

static volatile int64_t s_beat_us;
static volatile int64_t s_screen_since_us;   /* 0: no sleep or wake under way */
static int64_t s_awake_since_us;
static bool s_was_awake;
static char s_last[96];   /* the last boot's stall, for ">crash" */

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "restart";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "USB";
    default: return "other";
    }
}

static void stall(const char *why)
{
    s_stall.magic = STALL_MAGIC;
    strlcpy(s_stall.why, why, sizeof(s_stall.why));
    ESP_LOGE(TAG, "%s: aborting for the core dump", why);
    abort();
}

static void check(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    int64_t screen = s_screen_since_us;
    if (screen && now - screen > SCREEN_STALL_US) {
        char why[96];
        snprintf(why, sizeof(why), "screen sleep/wake stuck %llds", (long long)((now - screen) / 1000000));
        stall(why);
    }
    bool awake = !muse_state_asleep();
    if (awake && !s_was_awake) {
        s_awake_since_us = now;
    }
    s_was_awake = awake;
    if (awake && !screen && now - s_awake_since_us > AWAKE_SETTLE_US && s_beat_us
        && now - s_beat_us > FACE_STALL_US) {
        char why[96];
        snprintf(why, sizeof(why), "face drew nothing for %llds while awake", (long long)((now - s_beat_us) / 1000000));
        stall(why);
    }
}

void muse_watchdog_start(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    if (s_stall.magic == STALL_MAGIC) {
        strlcpy(s_last, s_stall.why, sizeof(s_last));
        ESP_LOGW(TAG, "last boot ended frozen: %s (core dump: idf.py coredump-info)", s_last);
    } else if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT
               || r == ESP_RST_BROWNOUT) {
        ESP_LOGW(TAG, "last boot ended: %s", reset_name(r));
    }
    s_stall.magic = 0;
    s_beat_us = esp_timer_get_time();
    s_awake_since_us = s_beat_us;
    const esp_timer_create_args_t args = { .callback = check, .name = "muse_wdog" };
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_periodic(t, CHECK_US);
    }
}

void muse_watchdog_beat(void)
{
    s_beat_us = esp_timer_get_time();
}

void muse_watchdog_screen(bool busy)
{
    s_screen_since_us = busy ? esp_timer_get_time() : 0;
    if (!busy) {
        s_beat_us = esp_timer_get_time();   /* frames restart from now */
    }
}

void muse_watchdog_print(void)
{
    printf("@crash {\"reset\":\"%s\",\"stall\":\"%s\",\"uptime_s\":%lld}\n", reset_name(esp_reset_reason()), s_last,
           (long long)(esp_timer_get_time() / 1000000));
    fflush(stdout);
}
