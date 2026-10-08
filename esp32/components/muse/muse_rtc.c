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
 * PCF85063A real-time clock, SNTP and the daily alarm (muse_rtc.h).
 *
 * Registers (NXP PCF85063A datasheet, section 8; also SensorLib's
 * PCF85063Constants.h in waveshareteam/ESP32-S3-Touch-AMOLED-2.16):
 * 0x00 Control_1 (bit 5 STOP, bit 1 12/24), then BCD time from 0x04:
 * seconds (bit 7 OS: the oscillator stopped, so the time is lost), minutes,
 * hours, days, weekdays, months, years (00-99). It runs in UTC here.
 */
#include "muse_rtc.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_extras.h"
#include "muse_state.h"
#include "muse_voice.h"
#include "muse_wifi.h"

static const char *TAG = "rtc";

#define RTC_ADDR 0x51
#define REG_CTRL1 0x00
#define REG_SECONDS 0x04
#define CTRL1_STOP 0x20
#define CTRL1_12H 0x02
#define SECONDS_OS 0x80
#define I2C_TIMEOUT_MS 50

#define ALARM_ROUNDS 3          /* chirp pairs, a beat apart */
#define ALARM_GAP_MS 700

#if CONFIG_MUSE_GADGET_RTC
static i2c_master_dev_handle_t s_dev;
static atomic_bool s_synced;     /* SNTP set the clock since the last tick */
static bool s_sntp_started;

static uint8_t bcd(uint8_t v, uint8_t mask)
{
    v &= mask;
    return (v >> 4) * 10 + (v & 0x0f);
}

static uint8_t to_bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

/* Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's
 * days_from_civil); newlib has no timegm(). */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

static esp_err_t rtc_write_time(time_t t)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    uint8_t buf[8] = {
        REG_SECONDS,
        to_bcd(tm.tm_sec),    /* OS cleared: the time is good again */
        to_bcd(tm.tm_min),
        to_bcd(tm.tm_hour),
        to_bcd(tm.tm_mday),
        (uint8_t)tm.tm_wday,
        to_bcd(tm.tm_mon + 1),
        to_bcd(tm.tm_year % 100),
    };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t muse_rtc_init(i2c_master_bus_handle_t bus)
{
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t reg = REG_CTRL1, ctrl1;
    err = i2c_master_transmit_receive(s_dev, &reg, 1, &ctrl1, 1, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 not answering (%s): time from NTP only", esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return err;
    }
    bool lost = false;
    if (ctrl1 & (CTRL1_STOP | CTRL1_12H)) {
        /* Stopped, or counting 12-hour time: run it in 24-hour time, and
         * don't trust what it holds. */
        uint8_t buf[2] = { REG_CTRL1, (uint8_t)(ctrl1 & ~(CTRL1_STOP | CTRL1_12H)) };
        i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
        lost = true;
    }

    reg = REG_SECONDS;
    uint8_t t[7];
    err = i2c_master_transmit_receive(s_dev, &reg, 1, t, sizeof(t), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    if (lost || (t[0] & SECONDS_OS)) {
        ESP_LOGI(TAG, "RTC lost its time (power or crystal): waiting for NTP");
        return ESP_OK;
    }
    int year = 2000 + bcd(t[6], 0xff);
    int64_t days = days_from_civil(year, bcd(t[5], 0x1f), bcd(t[3], 0x3f));
    time_t secs = (time_t)(days * 86400 + bcd(t[2], 0x3f) * 3600 + bcd(t[1], 0x7f) * 60
                           + bcd(t[0], 0x7f));
    if (year < 2024) {
        ESP_LOGI(TAG, "RTC says %d: not set yet, waiting for NTP", year);
        return ESP_OK;
    }
    struct timeval tv = { .tv_sec = secs };
    settimeofday(&tv, NULL);
    char when[32];
    muse_time_format(when, sizeof(when), "%Y-%m-%d %H:%M:%S %Z");
    ESP_LOGI(TAG, "time from RTC: %s", when);
    return ESP_OK;
}

static void on_sntp_sync(struct timeval *tv)
{
    (void)tv;
    atomic_store(&s_synced, true);
}

static void start_sntp(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_MUSE_GADGET_NTP_SERVER);
    cfg.sync_cb = on_sntp_sync;
    /* It retries by itself, and resyncs every CONFIG_LWIP_SNTP_UPDATE_DELAY. */
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP: %s", esp_err_to_name(err));
        return;
    }
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP started (%s)", CONFIG_MUSE_GADGET_NTP_SERVER);
}

#else
esp_err_t muse_rtc_init(i2c_master_bus_handle_t bus)
{
    (void)bus;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

#if CONFIG_MUSE_GADGET_ALARM
static void sound_alarm(int minute)
{
    ESP_LOGI(TAG, "alarm %02d:%02d", minute / 60, minute % 60);
    muse_state_set_asleep(false);
    for (int i = 0; i < ALARM_ROUNDS; i++) {
        /* The voice task plays it once idle, and before resting. */
        muse_voice_request_chirp();
        vTaskDelay(pdMS_TO_TICKS(250));
        muse_voice_request_chirp();
        vTaskDelay(pdMS_TO_TICKS(ALARM_GAP_MS));
    }
}

/*
 * The alarm's time in minutes after local midnight, and whether it's on:
 * "alarm_min" and "alarm_on" in NVS override the Kconfig time. An older
 * "alarm_min" of -1 (off) leaves it off at the Kconfig time. Read once, then
 * kept here; -2 until then.
 */
static atomic_int s_alarm_min = -2;
static atomic_bool s_alarm_on;

static void load_alarm(void)
{
    if (atomic_load(&s_alarm_min) != -2) {
        return;
    }
    int def = CONFIG_MUSE_GADGET_ALARM_HOUR * 60 + CONFIG_MUSE_GADGET_ALARM_MINUTE;
    int m = (int)muse_extras_get_i32("alarm_min", def);
    bool on = muse_extras_get_i32("alarm_on", 1) != 0 && m >= 0;
    atomic_store(&s_alarm_on, on);
    atomic_store(&s_alarm_min, m >= 0 && m < 24 * 60 ? m : def);
    if (on) {
        ESP_LOGI(TAG, "daily alarm at %02d:%02d", atomic_load(&s_alarm_min) / 60, atomic_load(&s_alarm_min) % 60);
    }
}

int muse_rtc_alarm(bool *on)
{
    load_alarm();
    if (on) {
        *on = atomic_load(&s_alarm_on);
    }
    return atomic_load(&s_alarm_min);
}

void muse_rtc_set_alarm(int minute, bool on)
{
    minute = minute < 0 ? 0 : minute >= 24 * 60 ? 24 * 60 - 1 : minute;
    atomic_store(&s_alarm_min, minute);
    atomic_store(&s_alarm_on, on);
    muse_extras_set_i32("alarm_min", minute);
    muse_extras_set_i32("alarm_on", on);
    ESP_LOGI(TAG, "daily alarm %s at %02d:%02d", on ? "on" : "off", minute / 60, minute % 60);
}

static void check_alarm(void)
{
    static int rang_on = -1;    /* year * 1000 + day of the year it last rang */
    bool on;
    int alarm = muse_rtc_alarm(&on);
    if (!on || !muse_time_valid()) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int today = tm.tm_year * 1000 + tm.tm_yday;
    if (tm.tm_hour * 60 + tm.tm_min == alarm && rang_on != today) {
        rang_on = today;
        sound_alarm(alarm);
    }
}
#endif

void muse_rtc_tick(void)
{
#if CONFIG_MUSE_GADGET_RTC
    if (!s_sntp_started && muse_wifi_connected()) {
        start_sntp();
    }
    if (atomic_exchange(&s_synced, false)) {
        char when[32];
        muse_time_format(when, sizeof(when), "%Y-%m-%d %H:%M:%S %Z");
        esp_err_t err = s_dev ? rtc_write_time(time(NULL)) : ESP_ERR_INVALID_STATE;
        ESP_LOGI(TAG, "time from NTP: %s%s", when, err == ESP_OK ? ", saved to the RTC" : "");
    }
#endif
#if CONFIG_MUSE_GADGET_ALARM
    check_alarm();
#endif
}
