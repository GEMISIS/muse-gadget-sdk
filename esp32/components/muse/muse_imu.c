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
 * QMI8658 IMU (muse_imu.h).
 *
 * Registers and the CTRL9 command protocol: the QMI8658A datasheet, as
 * SensorLib's SensorQMI8658.hpp drives it (waveshareteam/
 * ESP32-S3-Touch-AMOLED-2.16, examples/arduino/libraries/SensorLib; the
 * vendor's 04_LVGL_QMI8658_ui example uses it at 0x6B). A CTRL9 command
 * takes its parameters in CAL1-CAL4, is written to CTRL9, completes when
 * STATUSINT bit 7 sets (CTRL8 bit 7 selects that handshake), and is
 * acknowledged by writing 0 to CTRL9, which clears the bit.
 *
 * Only the accelerometer runs (4 g, 125 Hz): the chip's own tap and
 * pedometer engines need nothing else, and the gyroscope would take ~0.6 mA
 * more. Tap and pedometer events latch in STATUS1 until it's read, so a
 * slow poll misses none; lift and shake are worked out here from the
 * acceleration.
 */
#include "muse_imu.h"

#include <math.h>
#include <stdatomic.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_extras.h"
#include "muse_state.h"

static const char *TAG = "imu";

#define IMU_ADDR 0x6B
#define I2C_TIMEOUT_MS 50

#define REG_WHOAMI 0x00
#define REG_CTRL1 0x02
#define REG_CTRL2 0x03
#define REG_CTRL7 0x08
#define REG_CTRL8 0x09
#define REG_CTRL9 0x0A
#define REG_CAL1_L 0x0B         /* CAL1_L .. CAL4_H: 0x0B .. 0x12 */
#define REG_CAL4_L 0x11
#define REG_CAL4_H 0x12
#define REG_STATUSINT 0x2D
#define REG_STATUS1 0x2F
#define REG_AX_L 0x35
#define REG_TAP_STATUS 0x59
#define REG_STEP_CNT_L 0x5A
#define REG_RESET 0x60
#define REG_RST_RESULT 0x4D

#define WHOAMI_QMI8658 0x05
#define RESET_CMD 0xB0
#define RST_RESULT_OK 0x80

#define CTRL1_ADDR_AI 0x40      /* auto-increment, little endian, INT pins off */
#define CTRL2_4G_125HZ 0x16     /* aFS 4 g (1 << 4), aODR 125 Hz (6) */
#define CTRL7_ACCEL 0x01
#define CTRL8_HANDSHAKE 0x80    /* CTRL9 done on STATUSINT bit 7 */
#define CTRL8_PEDOMETER 0x10
#define CTRL8_TAP 0x01
#define STATUSINT_CMD_DONE 0x80
#define STATUS1_TAP 0x02
#define TAP_DOUBLE 2

#define CMD_ACK 0x00
#define CMD_CONFIGURE_TAP 0x0C
#define CMD_CONFIGURE_PEDOMETER 0x0D
#define CMD_RESET_PEDOMETER 0x0F

#define ODR_HZ 125
#define LSB_PER_G 8192.0f       /* 4 g full scale */

/* Polling: fast enough to see a shake with the screen on; asleep, a lift
 * only needs noticing within a quarter second. */
#define POLL_AWAKE_MS 40
#define POLL_ASLEEP_MS 250

/* Lift or tilt: the gravity vector this far from where it last rested, or a
 * jolt this far from 1 g, wakes the screen. */
#define WAKE_TILT_DEG 30.0f
#define WAKE_JOLT_G 0.45f
/* Resting: within this of 1 g and turning less than this, for REST_MS. */
#define REST_JOLT_G 0.08f
#define REST_TURN_DEG 4.0f
#define REST_MS 1500
/* Not straight after sleeping: the hand that put it down may still be on it. */
#define WAKE_AFTER_SLEEP_MS 3000

/* Shake: this many samples (of SHAKE_WINDOW_MS) more than SHAKE_G from 1 g. */
#define SHAKE_G 1.3f
#define SHAKE_SAMPLES 8
#define SHAKE_WINDOW_MS 1000
#define SHAKE_COOLDOWN_MS 4000

#define STEPS_SAVE_MS (5 * 60 * 1000)

/* Replays the last reply; provided by the TTS code if the build has it. */
__attribute__((weak)) void muse_tts_replay_last(void);

static i2c_master_dev_handle_t s_dev;
static atomic_int s_steps = -1;     /* today's, for the UI */
static int s_steps_base;            /* counted before the chip's counter last reset */
static int32_t s_day;               /* YYYYMMDD the steps are for, 0 unknown */
static int s_saved_steps = -1;
static int64_t s_saved_at;

/* Motion state, in g. */
static float s_rest[3];             /* where it last rested */
static bool s_rest_valid;
static float s_prev[3];
static int64_t s_still_since;
static int64_t s_slept_at;
static bool s_was_asleep;
static int64_t s_shake_start, s_shake_until;
static int s_shake_count;

static esp_err_t rd(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, len, I2C_TIMEOUT_MS);
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static bool wait_cmd_done(bool done)
{
    for (int i = 0; i < 100; i++) {
        uint8_t st;
        if (rd(REG_STATUSINT, &st, 1) == ESP_OK && !!(st & STATUSINT_CMD_DONE) == done) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

static esp_err_t command(uint8_t cmd)
{
    esp_err_t err = wr(REG_CTRL9, cmd);
    if (err != ESP_OK) {
        return err;
    }
    bool ok = wait_cmd_done(true);
    wr(REG_CTRL9, CMD_ACK);
    if (!wait_cmd_done(false) || !ok) {
        ESP_LOGW(TAG, "CTRL9 command 0x%02x timed out", cmd);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/* CAL1_L .. CAL4_H, then the command. */
static esp_err_t command_with(uint8_t cmd, const uint8_t cal[8])
{
    for (int i = 0; i < 8; i++) {
        esp_err_t err = wr(REG_CAL1_L + i, cal[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return command(cmd);
}

/*
 * Tap engine, scaled from SensorLib's 500 Hz example to 125 Hz: a peak within
 * 40 ms, 100 ms quiet before a second tap, which must come within 500 ms.
 * Thresholds in 0.001 g^2: peak 0.8 g^2, quiet 0.4 g^2.
 */
static esp_err_t configure_tap(void)
{
    const uint16_t tap_window = 13, dtap_window = 63, peak = 800, quiet = 400;
    const uint8_t first[8] = {
        5, 0,                                   /* peak window (samples), axis priority X>Y>Z */
        tap_window & 0xff, tap_window >> 8,
        dtap_window & 0xff, dtap_window >> 8,
        0, 0x01,                                /* CAL4_H 1: first part */
    };
    const uint8_t second[8] = {
        8, 32,                                  /* alpha 1/16, gamma 1/4 (7-bit fractions) */
        peak & 0xff, peak >> 8,
        quiet & 0xff, quiet >> 8,
        0, 0x02,                                /* CAL4_H 2: second part */
    };
    esp_err_t err = command_with(CMD_CONFIGURE_TAP, first);
    return err == ESP_OK ? command_with(CMD_CONFIGURE_TAP, second) : err;
}

/*
 * Pedometer, SensorLib's example settings in time rather than samples: 0.8 s
 * windows, 200 mg peak to peak and 100 mg over the mean for a step, steps
 * 0.32 s to 3.2 s apart, counted after 10 in a row (shaking it in a hand
 * shouldn't count), the count updated every step.
 */
static esp_err_t configure_pedometer(void)
{
    const uint16_t window = ODR_HZ * 4 / 5, p2p = 200, peak = 100;
    const uint16_t time_up = ODR_HZ * 16 / 5;
    const uint8_t first[8] = {
        window & 0xff, window >> 8,
        p2p & 0xff, p2p >> 8,
        peak & 0xff, peak >> 8,
        0x02, 0x01,                             /* CAL4_L 2, CAL4_H 1: first part */
    };
    const uint8_t second[8] = {
        time_up & 0xff, time_up >> 8,
        ODR_HZ * 8 / 25,                        /* time_low: 0.32 s */
        10,                                     /* steps before counting starts */
        0,                                      /* fix precision */
        1,                                      /* update every step */
        0x02, 0x02,                             /* CAL4_L 2, CAL4_H 2: second part */
    };
    esp_err_t err = command_with(CMD_CONFIGURE_PEDOMETER, first);
    return err == ESP_OK ? command_with(CMD_CONFIGURE_PEDOMETER, second) : err;
}

static int32_t today(void)
{
    if (!muse_time_valid()) {
        return 0;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

static int read_counter(void)
{
    uint8_t b[3];
    if (rd(REG_STEP_CNT_L, b, sizeof(b)) != ESP_OK) {
        return -1;
    }
    return b[0] | b[1] << 8 | b[2] << 16;
}

esp_err_t muse_imu_init(i2c_master_bus_handle_t bus)
{
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = IMU_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t id = 0;
    err = rd(REG_WHOAMI, &id, 1);
    if (err != ESP_OK || id != WHOAMI_QMI8658) {
        ESP_LOGW(TAG, "QMI8658 not found (%s, id 0x%02x): no motion or steps",
                 esp_err_to_name(err), id);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }

    wr(REG_RESET, RESET_CMD);
    uint8_t res = 0;
    for (int i = 0; i < 20 && res != RST_RESULT_OK; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
        rd(REG_RST_RESULT, &res, 1);
    }
    if (res != RST_RESULT_OK) {
        ESP_LOGW(TAG, "reset result 0x%02x; carrying on", res);
    }
    /* Configured with the sensors off, as the engines require. */
    if ((err = wr(REG_CTRL1, CTRL1_ADDR_AI)) != ESP_OK
        || (err = wr(REG_CTRL7, 0)) != ESP_OK
        || (err = wr(REG_CTRL8, CTRL8_HANDSHAKE)) != ESP_OK
        || (err = wr(REG_CTRL2, CTRL2_4G_125HZ)) != ESP_OK
        || (err = configure_tap()) != ESP_OK
        || (err = configure_pedometer()) != ESP_OK
        || (err = wr(REG_CTRL7, CTRL7_ACCEL)) != ESP_OK
        || (err = wr(REG_CTRL8, CTRL8_HANDSHAKE | CTRL8_PEDOMETER | CTRL8_TAP)) != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 setup failed (%s)", esp_err_to_name(err));
        return err;
    }

    /* The chip's counter starts from 0; today's earlier steps are in NVS. */
    int32_t saved_day = muse_extras_get_i32("step_day", 0);
    int saved = (int)muse_extras_get_i32("steps", 0);
    s_day = today();
    if (!s_day || s_day == saved_day) {
        /* Same day, or no clock yet to tell (the tick sorts that out). */
        s_steps_base = saved;
        s_day = s_day ? s_day : saved_day;
    }
    s_saved_steps = s_steps_base;
    s_saved_at = esp_timer_get_time();
    atomic_store(&s_steps, s_steps_base);
    ESP_LOGI(TAG, "QMI8658 up: taps, lifts and steps (%d today so far)", s_steps_base);
    return ESP_OK;
}

static void wake(const char *why)
{
    if (muse_state_asleep()) {
        ESP_LOGI(TAG, "waking (%s)", why);
        muse_state_set_asleep(false);
    } else {
        muse_state_poke();
    }
}

static float angle_deg(const float a[3], const float b[3])
{
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    float na = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    float nb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (na < 0.1f || nb < 0.1f) {
        return 0;
    }
    float c = dot / (na * nb);
    c = c > 1 ? 1 : (c < -1 ? -1 : c);
    return acosf(c) * 180.0f / (float)M_PI;
}

static void on_shake(int64_t now)
{
    s_shake_until = now + SHAKE_COOLDOWN_MS * 1000LL;
    s_shake_count = 0;
    muse_mode_t mode = muse_state_mode(NULL);
    if (mode != MUSE_MODE_IDLE) {
        return;   /* not over a recording or a reply */
    }
    wake("shake");
    if (muse_tts_replay_last) {
        ESP_LOGI(TAG, "shake: replaying the last reply");
        muse_tts_replay_last();
    } else {
        ESP_LOGI(TAG, "shake (no replay in this build)");
    }
}

static void motion(const float a[3], bool asleep, int64_t now)
{
    float g = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    float jolt = fabsf(g - 1.0f);
    float turn = angle_deg(a, s_prev);
    for (int i = 0; i < 3; i++) {
        s_prev[i] = a[i];
    }

    /* Resting: remember which way up, to tell a lift from it. */
    if (jolt < REST_JOLT_G && turn < REST_TURN_DEG) {
        if (!s_still_since) {
            s_still_since = now;
        } else if (now - s_still_since >= REST_MS * 1000LL) {
            for (int i = 0; i < 3; i++) {
                s_rest[i] = a[i];
            }
            s_rest_valid = true;
        }
    } else {
        s_still_since = 0;
    }

    if (asleep && !s_was_asleep) {
        s_slept_at = now;
    }
    s_was_asleep = asleep;
#if CONFIG_MUSE_GADGET_MOTION_WAKE
    if (asleep && s_rest_valid && now - s_slept_at >= WAKE_AFTER_SLEEP_MS * 1000LL
        && (jolt > WAKE_JOLT_G || angle_deg(a, s_rest) > WAKE_TILT_DEG)) {
        /* Where it is now is where it rests next, so one lift wakes once. */
        for (int i = 0; i < 3; i++) {
            s_rest[i] = a[i];
        }
        wake(jolt > WAKE_JOLT_G ? "moved" : "tilted");
    }
#endif

    /* Shaking: only seen at the awake poll rate. */
    if (asleep || now < s_shake_until) {
        return;
    }
    if (jolt > SHAKE_G) {
        if (!s_shake_count || now - s_shake_start > SHAKE_WINDOW_MS * 1000LL) {
            s_shake_start = now;
            s_shake_count = 0;
        }
        if (++s_shake_count >= SHAKE_SAMPLES) {
            on_shake(now);
        }
    }
}

int muse_imu_poll(void)
{
    bool asleep = muse_state_asleep();
    int64_t now = esp_timer_get_time();
    uint8_t st1;
    if (rd(REG_STATUS1, &st1, 1) == ESP_OK && (st1 & STATUS1_TAP)) {
        uint8_t tap;
        if (rd(REG_TAP_STATUS, &tap, 1) == ESP_OK && (tap & 0x03) == TAP_DOUBLE) {
            wake("double tap");
        }
    }
    uint8_t raw[6];
    if (rd(REG_AX_L, raw, sizeof(raw)) == ESP_OK) {
        float a[3];
        for (int i = 0; i < 3; i++) {
            a[i] = (int16_t)(raw[2 * i] | raw[2 * i + 1] << 8) / LSB_PER_G;
        }
        motion(a, asleep, now);
    }
    return asleep ? POLL_ASLEEP_MS : POLL_AWAKE_MS;
}

void muse_imu_tick(void)
{
    int32_t day = today();
    if (day && !s_day) {
        s_day = day;   /* the clock's just been set: these steps are today's */
    } else if (day && day != s_day) {
        /* Midnight, or the clock now known and a day on from the saved count. */
        ESP_LOGI(TAG, "new day: %d steps on %ld", atomic_load(&s_steps), (long)s_day);
        command(CMD_RESET_PEDOMETER);
        s_steps_base = 0;
        s_day = day;
        atomic_store(&s_steps, 0);
        muse_extras_set_i32("step_day", s_day);
        muse_extras_set_i32("steps", 0);
        s_saved_steps = 0;
        s_saved_at = esp_timer_get_time();
        return;
    }
    int counter = read_counter();
    if (counter < 0) {
        return;
    }
    int steps = s_steps_base + counter;
    atomic_store(&s_steps, steps);
    int64_t now = esp_timer_get_time();
    if (steps != s_saved_steps && now - s_saved_at >= STEPS_SAVE_MS * 1000LL) {
        muse_extras_set_i32("step_day", s_day);
        muse_extras_set_i32("steps", steps);
        s_saved_steps = steps;
        s_saved_at = now;
    }
}

int muse_imu_steps(void)
{
    return atomic_load(&s_steps);
}
