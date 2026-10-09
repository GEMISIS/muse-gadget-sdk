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
 * The passcode (muse_lock.h): its NVS, the lock's triggers and the attempts.
 * NVS is written only from tasks with internal-RAM stacks (the LVGL task, the
 * one that starts the app); the rest only read flags here.
 */
#include "muse_lock.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "muse_link.h"
#include "muse_lock_hash.h"
#include "muse_sd.h"
#include "muse_state.h"

static const char *TAG = "muse_lock";

#define NS "muse_lock"
#define KEY_PIN "pin"           /* muse_lock_record_t */
#define KEY_STATE "state"       /* saved_t */
#define KEY_AFTER_BAT "after_bat"
#define KEY_AFTER_CHG "after_chg"
#define KEY_MODES "modes"       /* a bit per mode that asks for it */
#define SAVE_EVERY_US (30 * 1000000LL)   /* the lockout's time left, while it runs */
#define HASH_TARGET_MS 400      /* a guess's cost on this chip */
#define PROBE_ROUNDS 2000

/* What's saved of the attempts: the policy, and a version to tell it by. */
typedef struct {
    uint8_t version;
    uint8_t failed;
    uint8_t round;
    uint8_t pad;
    uint32_t lockout_s;
} saved_t;
#define SAVED_VERSION 1

EXT_RAM_BSS_ATTR static nvs_handle_t s_nvs;
EXT_RAM_BSS_ATTR static volatile bool s_locked;
EXT_RAM_BSS_ATTR static volatile bool s_has_pin;
EXT_RAM_BSS_ATTR static volatile uint32_t s_gen;
EXT_RAM_BSS_ATTR static muse_lock_record_t s_rec;
EXT_RAM_BSS_ATTR static muse_lock_policy_t s_policy;
EXT_RAM_BSS_ATTR static int64_t s_lockout_at_us;   /* when s_policy.lockout_s was right */
EXT_RAM_BSS_ATTR static int64_t s_saved_at_us;
EXT_RAM_BSS_ATTR static volatile uint8_t s_after[2];   /* battery, charger */
EXT_RAM_BSS_ATTR static volatile uint8_t s_modes;
/* The sleep under way: when it started, and the power it's seen. */
EXT_RAM_BSS_ATTR static volatile int64_t s_slept_at_us;
EXT_RAM_BSS_ATTR static volatile bool s_slept_battery, s_slept_charger;
/* ">pin=", waiting for the LVGL task. */
EXT_RAM_BSS_ATTR static char s_bench_pin[MUSE_LOCK_PIN_LEN + 1];
EXT_RAM_BSS_ATTR static volatile bool s_bench_waiting;

static void save_state(void)
{
    saved_t v = { SAVED_VERSION, s_policy.failed, s_policy.round, 0, s_policy.lockout_s };
    esp_err_t err = s_nvs ? nvs_set_blob(s_nvs, KEY_STATE, &v, sizeof(v)) : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "couldn't save the attempts: %s", esp_err_to_name(err));
    }
    s_saved_at_us = esp_timer_get_time();
}

static void set_locked(bool locked, const char *why)
{
    if (locked != s_locked) {
        ESP_LOGI(TAG, "%s (%s)", locked ? "locked" : "unlocked", why);
    }
    s_locked = locked;
    s_gen++;
}

void muse_lock_init(void)
{
    if (nvs_open(NS, NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGE(TAG, "no NVS namespace " NS ": no passcode");
        s_nvs = 0;
        return;
    }
    size_t n = sizeof(s_rec);
    s_has_pin = nvs_get_blob(s_nvs, KEY_PIN, &s_rec, &n) == ESP_OK && n == sizeof(s_rec)
                && s_rec.version == MUSE_LOCK_RECORD_VERSION;
    saved_t v;
    n = sizeof(v);
    muse_lock_policy_reset(&s_policy);
    if (nvs_get_blob(s_nvs, KEY_STATE, &v, &n) == ESP_OK && n == sizeof(v) && v.version == SAVED_VERSION) {
        s_policy.failed = v.failed;
        s_policy.round = v.round;
        s_policy.lockout_s = v.lockout_s;   /* resumed whole: time off doesn't count */
        if (!muse_lock_policy_sane(&s_policy)) {
            ESP_LOGW(TAG, "attempts out of bounds: kept to the stricter");
            save_state();
        }
    }
    s_lockout_at_us = esp_timer_get_time();
    uint8_t b = MUSE_LOCK_AFTER_BATTERY_DEFAULT, c = MUSE_LOCK_AFTER_CHARGER_DEFAULT, m = 0;
    nvs_get_u8(s_nvs, KEY_AFTER_BAT, &b);
    nvs_get_u8(s_nvs, KEY_AFTER_CHG, &c);
    nvs_get_u8(s_nvs, KEY_MODES, &m);
    s_after[0] = b < MUSE_LOCK_AFTER_COUNT ? b : MUSE_LOCK_AFTER_BATTERY_DEFAULT;
    s_after[1] = c < MUSE_LOCK_AFTER_COUNT ? c : MUSE_LOCK_AFTER_CHARGER_DEFAULT;
    s_modes = m;
    /* Erasing was due (the power went as it started): it still is. */
    s_locked = s_has_pin || muse_lock_policy_wipe_due(&s_policy);
    ESP_LOGI(TAG, "passcode %s; %d wrong, round %d, lockout %u s%s", s_has_pin ? "set" : "off", s_policy.failed,
             s_policy.round, (unsigned)s_policy.lockout_s, muse_lock_policy_wipe_due(&s_policy) ? ", erase due" : "");
}

bool muse_lock_locked(void)
{
    return s_locked;
}

bool muse_lock_has_pin(void)
{
    return s_has_pin;
}

uint32_t muse_lock_gen(void)
{
    return s_gen;
}

void muse_lock_now(const char *why)
{
    if (s_has_pin && !s_locked) {
        set_locked(true, why);
    }
}

/* ---- Sleep ---- */

void muse_lock_note_asleep(bool asleep)
{
    if (asleep) {
        if (!s_slept_at_us) {
            bool battery = muse_state_on_battery();
            s_slept_battery = battery;
            s_slept_charger = !battery;
            s_slept_at_us = esp_timer_get_time();
        }
        return;
    }
    int64_t at = s_slept_at_us;
    if (!at) {
        return;
    }
    s_slept_at_us = 0;
    uint32_t slept_s = (uint32_t)((esp_timer_get_time() - at) / 1000000);
    if (s_has_pin && !s_locked
        && muse_lock_policy_sleep_locks(slept_s, s_slept_battery, s_slept_charger, s_after[0], s_after[1])) {
        char why[48];
        snprintf(why, sizeof(why), "asleep %u s on %s", (unsigned)slept_s,
                 s_slept_battery && s_slept_charger ? "battery and charger" : s_slept_battery ? "battery" : "charger");
        set_locked(true, why);
    }
}

void muse_lock_note_power(bool on_battery)
{
    if (s_slept_at_us) {
        if (on_battery) {
            s_slept_battery = true;
        } else {
            s_slept_charger = true;
        }
    }
}

/* ---- Modes ---- */

bool muse_lock_mode_requires(int mode)
{
    return mode >= 0 && mode < 8 && (s_modes >> mode & 1);
}

void muse_lock_set_mode_requires(int mode, bool on)
{
    if (mode < 0 || mode >= 8 || !s_nvs) {
        return;
    }
    uint8_t m = on ? s_modes | (1u << mode) : s_modes & ~(1u << mode);
    if (m != s_modes) {
        s_modes = m;
        nvs_set_u8(s_nvs, KEY_MODES, m);
        nvs_commit(s_nvs);
    }
}

void muse_lock_mode_entered(int mode)
{
    if (muse_lock_mode_requires(mode)) {
        muse_lock_now("a mode that asks for it");
    }
}

int muse_lock_after(bool charger)
{
    return s_after[charger];
}

void muse_lock_set_after(bool charger, int choice)
{
    if (choice < 0 || choice >= MUSE_LOCK_AFTER_COUNT || choice == s_after[charger] || !s_nvs) {
        return;
    }
    s_after[charger] = (uint8_t)choice;
    nvs_set_u8(s_nvs, charger ? KEY_AFTER_CHG : KEY_AFTER_BAT, (uint8_t)choice);
    nvs_commit(s_nvs);
}

/* ---- Attempts ---- */

static bool valid_pin(const char *pin)
{
    if (strlen(pin) != MUSE_LOCK_PIN_LEN) {
        return false;
    }
    for (const char *p = pin; *p; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return true;
}

muse_lock_result_t muse_lock_try(const char *pin)
{
    muse_lock_tick();   /* the lockout as of now */
    if (!s_has_pin || !muse_lock_policy_can_try(&s_policy)) {
        return MUSE_LOCK_REFUSED;
    }
    /* Counted as wrong, and saved, before it's checked: cutting the power
     * while the hash runs can't make it not have happened. */
    muse_lock_policy_t before = s_policy;
    muse_lock_result_t r = muse_lock_policy_wrong(&s_policy);
    save_state();
    s_lockout_at_us = esp_timer_get_time();
    int64_t t0 = esp_timer_get_time();
    bool right = valid_pin(pin) && muse_lock_hash_check(&s_rec, pin, strlen(pin));
    ESP_LOGI(TAG, "checked in %d ms", (int)((esp_timer_get_time() - t0) / 1000));
    if (right) {
        muse_lock_policy_reset(&s_policy);
        if (before.failed || before.round || before.lockout_s) {
            save_state();
        }
        set_locked(false, "passcode");
        return MUSE_LOCK_RIGHT;
    }
    ESP_LOGW(TAG, "wrong passcode: %d wrong, round %d%s", s_policy.failed, s_policy.round,
             r == MUSE_LOCK_LOCKOUT ? ", locked out" : r == MUSE_LOCK_WIPE ? ", erasing" : "");
    if (r == MUSE_LOCK_LOCKOUT || r == MUSE_LOCK_WIPE) {
        set_locked(true, r == MUSE_LOCK_WIPE ? "erasing" : "too many wrong");
    }
    s_gen++;
    return r;
}

bool muse_lock_set_pin(const char *pin)
{
    if (!valid_pin(pin) || !s_nvs) {
        return false;
    }
    uint8_t salt[MUSE_LOCK_SALT_LEN];
    esp_fill_random(salt, sizeof(salt));
    /* As many rounds as take this chip HASH_TARGET_MS: timed on a few first. */
    uint8_t probe[MUSE_LOCK_HASH_LEN];
    int64_t t0 = esp_timer_get_time();
    bool ok = muse_lock_hash_derive(pin, MUSE_LOCK_PIN_LEN, salt, PROBE_ROUNDS, probe);
    uint32_t rounds = muse_lock_hash_rounds_for(PROBE_ROUNDS, esp_timer_get_time() - t0, HASH_TARGET_MS);
    memset(probe, 0, sizeof(probe));
    muse_lock_record_t rec;
    t0 = esp_timer_get_time();
    ok = ok && muse_lock_hash_make(pin, MUSE_LOCK_PIN_LEN, salt, rounds, &rec);
    ESP_LOGI(TAG, "passcode hashed: %u rounds, %d ms", (unsigned)rounds, (int)((esp_timer_get_time() - t0) / 1000));
    if (!ok || nvs_set_blob(s_nvs, KEY_PIN, &rec, sizeof(rec)) != ESP_OK || nvs_commit(s_nvs) != ESP_OK) {
        ESP_LOGE(TAG, "couldn't keep the passcode");
        return false;
    }
    s_rec = rec;
    muse_lock_policy_reset(&s_policy);
    save_state();
    s_has_pin = true;
    s_gen++;
    return true;
}

void muse_lock_clear_pin(void)
{
    if (s_nvs) {
        nvs_erase_key(s_nvs, KEY_PIN);
        nvs_commit(s_nvs);
    }
    memset(&s_rec, 0, sizeof(s_rec));
    muse_lock_policy_reset(&s_policy);
    save_state();
    s_has_pin = false;
    set_locked(false, "passcode off");
}

void muse_lock_state(muse_lock_policy_t *out)
{
    *out = s_policy;
}

void muse_lock_tick(void)
{
    if (!s_policy.lockout_s) {
        return;
    }
    int64_t now = esp_timer_get_time();
    uint32_t secs = (uint32_t)((now - s_lockout_at_us) / 1000000);
    if (!secs) {
        return;
    }
    s_lockout_at_us += (int64_t)secs * 1000000;
    bool ended = muse_lock_policy_elapse(&s_policy, secs);
    /* Saved now and then, so a restart resumes it about where it was: it
     * loses no more than SAVE_EVERY_US, and only ever runs longer. */
    if (ended || now - s_saved_at_us >= SAVE_EVERY_US) {
        save_state();
    }
    if (ended) {
        ESP_LOGI(TAG, "lockout over: the final round");
        s_gen++;
    }
}

bool muse_lock_wipe_due(void)
{
    return muse_lock_policy_wipe_due(&s_policy);
}

void muse_lock_wipe(void)
{
    ESP_LOGW(TAG, "too many wrong passcodes: erasing everything");
    muse_sd_wipe();
    if (muse_link_factory_reset()) {
        return;   /* its task erases NVS and restarts */
    }
    nvs_flash_deinit();
    nvs_flash_erase();
    esp_restart();
}

/* ---- Bench ---- */

void muse_lock_print(void)
{
    muse_lock_policy_t p = s_policy;
    printf("@lock {\"locked\":%s,\"passcode\":%s,\"failed\":%d,\"round\":%d,\"lockout_s\":%u,\"wipe_armed\":%s}\n",
           s_locked ? "true" : "false", s_has_pin ? "true" : "false", p.failed, p.round, (unsigned)p.lockout_s,
           muse_lock_policy_final(&p) ? "true" : "false");
    fflush(stdout);
}

void muse_lock_bench_pin(const char *pin)
{
    if (s_bench_waiting) {
        printf("@pin {\"result\":\"busy\"}\n");
        fflush(stdout);
        return;
    }
    strlcpy(s_bench_pin, pin, sizeof(s_bench_pin));
    s_bench_waiting = true;
}

bool muse_lock_bench_take(char pin[MUSE_LOCK_PIN_LEN + 1])
{
    if (!s_bench_waiting) {
        return false;
    }
    memcpy(pin, s_bench_pin, MUSE_LOCK_PIN_LEN + 1);
    memset(s_bench_pin, 0, sizeof(s_bench_pin));
    s_bench_waiting = false;
    return true;
}
