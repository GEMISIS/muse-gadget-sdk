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
 * The Waveshare 2.16's microSD slot, RTC and IMU (muse_extras.h): setup and
 * the one task that polls them.
 */
#include "muse_extras.h"

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "muse_imu.h"
#include "muse_rtc.h"
#include "muse_sd.h"

static const char *TAG = "extras";

#define NS "gadget"
/* Internal RAM, not PSRAM: the task writes NVS (steps), which runs with the
 * flash cache off, when a PSRAM stack can't be reached. */
#define TASK_STACK 4096
#define TASK_PRIORITY 2         /* below input (6), voice (6), UI (5), image (4) */
#define SLOW_MS 1000            /* clock, alarm, steps, the caption log */

/* 2024-01-01T00:00:00Z: anything earlier is the clock never having been set. */
#define TIME_VALID_AFTER 1704067200

static nvs_handle_t s_nvs;

int32_t muse_extras_get_i32(const char *key, int32_t def)
{
    int32_t v;
    return s_nvs && nvs_get_i32(s_nvs, key, &v) == ESP_OK ? v : def;
}

void muse_extras_set_i32(const char *key, int32_t value)
{
    int32_t old;
    if (!s_nvs || (nvs_get_i32(s_nvs, key, &old) == ESP_OK && old == value)) {
        return;
    }
    if (nvs_set_i32(s_nvs, key, value) != ESP_OK || nvs_commit(s_nvs) != ESP_OK) {
        ESP_LOGW(TAG, "couldn't save %s", key);
    }
}

bool muse_time_valid(void)
{
    return time(NULL) > TIME_VALID_AFTER;
}

bool muse_time_format(char *out, size_t cap, const char *fmt)
{
    out[0] = '\0';
    if (!muse_time_valid()) {
        return false;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return strftime(out, cap, fmt, &tm) > 0;
}

static void extras_task(void *arg)
{
    (void)arg;
    /* The card first: without one the mount takes a moment to give up, and
     * nothing else here needs it. */
    muse_sd_mount();
#if CONFIG_MUSE_GADGET_IMU
    bool imu = muse_imu_init(bsp_i2c_get_handle()) == ESP_OK;
#endif
    int64_t slow_at = 0;
    for (;;) {
        int wait_ms = SLOW_MS;
#if CONFIG_MUSE_GADGET_IMU
        if (imu) {
            wait_ms = muse_imu_poll();
        }
#endif
        int64_t now = esp_timer_get_time();
        if (now >= slow_at) {
            slow_at = now + SLOW_MS * 1000LL;
            muse_rtc_tick();
#if CONFIG_MUSE_GADGET_IMU
            if (imu) {
                muse_imu_tick();
            }
#endif
            muse_sd_caption_write();
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

void muse_extras_start(void)
{
    /* Local time for the clock, the alarm and file names; the RTC keeps UTC. */
    setenv("TZ", CONFIG_MUSE_GADGET_TZ, 1);
    tzset();
    if (nvs_open(NS, NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGW(TAG, "no NVS namespace " NS ": settings and steps won't be kept");
        s_nvs = 0;
    }
#if CONFIG_MUSE_GADGET_RTC
    /* Before anything logs or names a file with the time. */
    muse_rtc_init(bsp_i2c_get_handle());
#endif
    if (xTaskCreate(extras_task, "muse_extras", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the extras task: no SD card, steps or alarm");
    }
}
