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

#include "muse_settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "muse_link.h"

static const char *TAG = "muse_settings";

#define NS "muse"
#define DEFAULT_HOST "hatch.metaaivm.com"

static struct {
    uint8_t volume;
    bool speaker_on;
    uint8_t mic_gain;
    uint8_t brightness;
    uint16_t sleep_s;
    bool wifi_on;
    bool ble_on;
    uint8_t gadget_mode;
    bool mode_override;
    uint32_t override_until;
    char home_ssid[MUSE_SSID_MAX + 1];
    char away_ssid[MUSE_SSID_MAX + 1];
    char ssid[MUSE_SSID_MAX + 1];
    char pass[MUSE_PASS_MAX + 1];
    char host[MUSE_HOST_MAX + 1];
    char vm[MUSE_VM_MAX + 1];
    char token[MUSE_TOKEN_MAX + 1];
    char chat_sid[MUSE_CHAT_SID_MAX + 1];
} s = {
    .volume = CONFIG_MUSE_DEFAULT_VOLUME,
    .speaker_on = true,
    .mic_gain = 30,
    .brightness = 100,
    .sleep_s = 120,
    .wifi_on = true,
    .host = DEFAULT_HOST,
};

static SemaphoreHandle_t s_lock;
static nvs_handle_t s_nvs;
static muse_setting_cb_t s_listener;

#define LOCKED(body) do { xSemaphoreTake(s_lock, portMAX_DELAY); body; xSemaphoreGive(s_lock); } while (0)

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Missing keys leave the default in place. */
static void load_str(const char *key, char *out, size_t len)
{
    size_t n = len;
    nvs_get_str(s_nvs, key, out, &n);
}

static void load_u8(const char *key, uint8_t *out)
{
    nvs_get_u8(s_nvs, key, out);
}

static void save_u8(const char *key, uint8_t v)
{
    nvs_set_u8(s_nvs, key, v);
    nvs_commit(s_nvs);
}

static void save_str(const char *key, const char *v)
{
    nvs_set_str(s_nvs, key, v);
    nvs_commit(s_nvs);
}

static void notify(muse_setting_t what)
{
    if (s_listener) {
        s_listener(what);
    }
}

esp_err_t muse_settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout changed, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    s_lock = xSemaphoreCreateMutex();
    err = nvs_open(NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t b;
    load_u8("volume", &s.volume);
    if (nvs_get_u8(s_nvs, "speaker", &b) == ESP_OK) {
        s.speaker_on = b;
    }
    load_u8("mic_gain", &s.mic_gain);
    load_u8("bright", &s.brightness);
    nvs_get_u16(s_nvs, "sleep_s", &s.sleep_s);
    if (nvs_get_u8(s_nvs, "wifi_on", &b) == ESP_OK) {
        s.wifi_on = b;
    }
    if (nvs_get_u8(s_nvs, "ble_on", &b) == ESP_OK) {
        s.ble_on = b;
    }
    load_u8("gmode", &s.gadget_mode);
    if (nvs_get_u8(s_nvs, "gmode_ovr", &b) == ESP_OK) {
        s.mode_override = b;
    }
    nvs_get_u32(s_nvs, "gmode_until", &s.override_until);
    load_str("home_ssid", s.home_ssid, sizeof(s.home_ssid));
    load_str("away_ssid", s.away_ssid, sizeof(s.away_ssid));
    load_str("ssid", s.ssid, sizeof(s.ssid));
    load_str("pass", s.pass, sizeof(s.pass));
    load_str("host", s.host, sizeof(s.host));
    load_str("vm", s.vm, sizeof(s.vm));
    load_str("token", s.token, sizeof(s.token));
    load_str("chat_sid", s.chat_sid, sizeof(s.chat_sid));
    if (s.chat_sid[0] && !muse_settings_chat_sid_valid(s.chat_sid)) {
        s.chat_sid[0] = '\0';
    }

    s.volume = clampi(s.volume, 0, 100);
    s.mic_gain = clampi(s.mic_gain, 0, MUSE_MIC_GAIN_MAX);
    s.brightness = clampi(s.brightness, 10, 100);
    s.gadget_mode = clampi(s.gadget_mode, 0, 2);
    ESP_LOGI(TAG, "vol %d%s, mic %d dB, bright %d, sleep %ds, wifi %s (%s), ble %s, muse %s",
             s.volume, s.speaker_on ? "" : " (speaker off)", s.mic_gain, s.brightness, s.sleep_s, s.wifi_on ? "on" : "off",
             "network saved by Link", s.ble_on ? "on" : "off", s.token[0] ? "token set" : "no token");
    ESP_LOGI(TAG, "muse chat: %s", s.chat_sid[0] ? s.chat_sid : "main");
    return ESP_OK;
}

void muse_settings_set_listener(muse_setting_cb_t cb)
{
    s_listener = cb;
}

int muse_settings_volume(void) { return s.volume; }
bool muse_settings_speaker_on(void) { return s.speaker_on; }
int muse_settings_mic_gain(void) { return s.mic_gain; }
int muse_settings_brightness(void) { return s.brightness; }
int muse_settings_sleep_s(void) { return s.sleep_s; }
bool muse_settings_wifi_on(void) { return s.wifi_on; }
bool muse_settings_ble_on(void) { return s.ble_on; }
int muse_settings_gadget_mode(void) { return s.gadget_mode; }

bool muse_settings_mode_override(uint32_t *until)
{
    bool on = false;
    LOCKED({
        on = s.mode_override;
        if (until) {
            *until = s.override_until;
        }
    });
    return on;
}

void muse_settings_home_ssid(char out[MUSE_SSID_MAX + 1])
{
    LOCKED(strlcpy(out, s.home_ssid, MUSE_SSID_MAX + 1));
}

void muse_settings_away_ssid(char out[MUSE_SSID_MAX + 1])
{
    LOCKED(strlcpy(out, s.away_ssid, MUSE_SSID_MAX + 1));
}

/* Home Link owns the saved networks (this is the first); the local copy is only a fallback. */
void muse_settings_wifi(char ssid[MUSE_SSID_MAX + 1], char pass[MUSE_PASS_MAX + 1])
{
    if (muse_link_wifi_get(ssid, pass)) {
        return;
    }
    LOCKED({
        strlcpy(ssid, s.ssid, MUSE_SSID_MAX + 1);
        if (pass) {
            strlcpy(pass, s.pass, MUSE_PASS_MAX + 1);
        }
    });
}

void muse_settings_hatch_host(char out[MUSE_HOST_MAX + 1])
{
    LOCKED(strlcpy(out, s.host, MUSE_HOST_MAX + 1));
}

void muse_settings_hatch_vm(char out[MUSE_VM_MAX + 1])
{
    LOCKED(strlcpy(out, s.vm, MUSE_VM_MAX + 1));
}

void muse_settings_hatch_token(char out[MUSE_TOKEN_MAX + 1])
{
    LOCKED(strlcpy(out, s.token, MUSE_TOKEN_MAX + 1));
}

size_t muse_settings_hatch_token_len(void)
{
    size_t n;
    LOCKED(n = strlen(s.token));
    return n;
}

void muse_settings_chat_sid(char out[MUSE_CHAT_SID_MAX + 1])
{
    LOCKED(strlcpy(out, s.chat_sid, MUSE_CHAT_SID_MAX + 1));
}

void muse_settings_gadget_chat_sid(char out[MUSE_CHAT_SID_MAX + 1])
{
    uint8_t m[6] = { 0 };
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(out, MUSE_CHAT_SID_MAX + 1, "gadget-%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

bool muse_settings_chat_sid_valid(const char *sid)
{
    size_t n = 0;
    for (; sid && sid[n]; n++) {
        char c = sid[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return n >= 1 && n <= MUSE_CHAT_SID_MAX;
}

void muse_settings_set_volume(int pct)
{
    s.volume = clampi(pct, 0, 100);
    save_u8("volume", s.volume);
    notify(MUSE_SETTING_VOLUME);
}

void muse_settings_set_speaker_on(bool on)
{
    s.speaker_on = on;
    save_u8("speaker", on);
    notify(MUSE_SETTING_SPEAKER);
}

void muse_settings_set_mic_gain(int db)
{
    s.mic_gain = clampi(db, 0, MUSE_MIC_GAIN_MAX);
    save_u8("mic_gain", s.mic_gain);
    notify(MUSE_SETTING_MIC_GAIN);
}

void muse_settings_set_brightness(int pct)
{
    s.brightness = clampi(pct, 10, 100);
    save_u8("bright", s.brightness);
    notify(MUSE_SETTING_BRIGHTNESS);
}

void muse_settings_set_sleep_s(int secs)
{
    s.sleep_s = clampi(secs, 0, 3600);
    nvs_set_u16(s_nvs, "sleep_s", s.sleep_s);
    nvs_commit(s_nvs);
    notify(MUSE_SETTING_SLEEP);
}

void muse_settings_set_wifi_on(bool on)
{
    s.wifi_on = on;
    save_u8("wifi_on", on);
    notify(MUSE_SETTING_WIFI);
}

void muse_settings_set_ble_on(bool on)
{
    s.ble_on = on;
    save_u8("ble_on", on);
    notify(MUSE_SETTING_BLE);
}

void muse_settings_set_gadget_mode(int mode)
{
    s.gadget_mode = clampi(mode, 0, 2);
    save_u8("gmode", s.gadget_mode);
    notify(MUSE_SETTING_GADGET_MODE);
}

void muse_settings_set_mode_override(bool on, uint32_t until)
{
    LOCKED({
        s.mode_override = on;
        s.override_until = on ? until : 0;
        nvs_set_u8(s_nvs, "gmode_ovr", on);
        nvs_set_u32(s_nvs, "gmode_until", s.override_until);
        nvs_commit(s_nvs);
    });
    notify(MUSE_SETTING_GADGET_MODE);
}

void muse_settings_set_home_ssid(const char *ssid)
{
    LOCKED({
        strlcpy(s.home_ssid, ssid ? ssid : "", sizeof(s.home_ssid));
        save_str("home_ssid", s.home_ssid);
    });
    ESP_LOGI(TAG, "home network: %s", s.home_ssid[0] ? s.home_ssid : "(none)");
    notify(MUSE_SETTING_GADGET_MODE);
}

void muse_settings_set_away_ssid(const char *ssid)
{
    LOCKED({
        strlcpy(s.away_ssid, ssid ? ssid : "", sizeof(s.away_ssid));
        save_str("away_ssid", s.away_ssid);
    });
    ESP_LOGI(TAG, "on-the-go network: %s", s.away_ssid[0] ? s.away_ssid : "(none)");
    notify(MUSE_SETTING_GADGET_MODE);
}

void muse_settings_set_wifi(const char *ssid, const char *pass)
{
    if (muse_link_wifi_set(ssid ? ssid : "", ssid && ssid[0] && pass ? pass : "")) {
        ESP_LOGI(TAG, "wifi network: %s", ssid && ssid[0] ? ssid : "(all forgotten)");
        notify(MUSE_SETTING_WIFI);
        return;
    }
    LOCKED({
        strlcpy(s.ssid, ssid ? ssid : "", sizeof(s.ssid));
        strlcpy(s.pass, s.ssid[0] && pass ? pass : "", sizeof(s.pass));
        save_str("ssid", s.ssid);
        save_str("pass", s.pass);
    });
    ESP_LOGI(TAG, "wifi network: %s", s.ssid[0] ? s.ssid : "(forgotten)");
    notify(MUSE_SETTING_WIFI);
}

void muse_settings_set_hatch_host(const char *host)
{
    LOCKED({
        strlcpy(s.host, host && host[0] ? host : DEFAULT_HOST, sizeof(s.host));
        save_str("host", s.host);
    });
    notify(MUSE_SETTING_HATCH);
}

void muse_settings_set_hatch_vm(const char *vm)
{
    LOCKED({
        strlcpy(s.vm, vm ? vm : "", sizeof(s.vm));
        save_str("vm", s.vm);
    });
    notify(MUSE_SETTING_HATCH);
}

esp_err_t muse_settings_set_hatch_token(const char *token, bool append)
{
    esp_err_t err = ESP_OK;
    LOCKED({
        size_t have = append ? strlen(s.token) : 0;
        size_t add = strlen(token ? token : "");
        if (have + add > MUSE_TOKEN_MAX) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            memcpy(s.token + have, token, add);
            s.token[have + add] = '\0';
            save_str("token", s.token);
        }
    });
    if (err == ESP_OK) {
        notify(MUSE_SETTING_HATCH);
    }
    return err;
}

bool muse_settings_set_chat_sid(const char *sid)
{
    if (!sid) {
        sid = "";
    }
    if (sid[0] && !muse_settings_chat_sid_valid(sid)) {
        return false;
    }
    bool changed = false;
    LOCKED({
        changed = strcmp(s.chat_sid, sid) != 0;
        if (changed) {
            strlcpy(s.chat_sid, sid, sizeof(s.chat_sid));
            save_str("chat_sid", s.chat_sid);
        }
    });
    if (changed) {
        ESP_LOGI(TAG, "muse chat: %s", sid[0] ? sid : "main");
        notify(MUSE_SETTING_CHAT);
    }
    return true;
}
