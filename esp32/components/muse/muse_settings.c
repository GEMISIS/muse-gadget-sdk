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
#include <strings.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
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
    char chat_sid[MUSE_CHAT_SID_MAX + 1];      /* RAM only: every boot starts in the main chat */
    muse_chat_entry_t chats[MUSE_CHATS_MAX];   /* NVS "chats": a blob of chats_n entries */
    int chats_n;
    uint32_t chats_gen;                        /* bumped when the list or the pick changes */
    bool chats_dirty;                          /* retitled: save_chats() is due (muse_settings_chats_flush) */
    char new_sid[MUSE_CHAT_SID_MAX + 1];       /* the new chat picked, until the Muse titles it (RAM only) */
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

/* A chat's id as kept: lower case, so one chat is always the same string. */
static void lower(char *sid)
{
    for (; *sid; sid++) {
        if (*sid >= 'A' && *sid <= 'F') {
            *sid += 'a' - 'A';
        }
    }
}

static void save_chats(void)
{
    if (s.chats_n) {
        nvs_set_blob(s_nvs, "chats", s.chats, s.chats_n * sizeof(s.chats[0]));
    } else {
        nvs_erase_key(s_nvs, "chats");
    }
    nvs_commit(s_nvs);
}

/* Entries that don't hold a name and a valid id are dropped. */
static void load_chats(void)
{
    size_t n = sizeof(s.chats);
    if (nvs_get_blob(s_nvs, "chats", s.chats, &n) != ESP_OK || n % sizeof(s.chats[0])) {
        n = 0;
    }
    int kept = 0;
    for (int i = 0; i < (int)(n / sizeof(s.chats[0])); i++) {
        muse_chat_entry_t *c = &s.chats[i];
        if (!memchr(c->name, '\0', sizeof(c->name)) || !c->name[0] || !memchr(c->sid, '\0', sizeof(c->sid))
            || !muse_settings_chat_sid_valid(c->sid)) {
            continue;
        }
        lower(c->sid);
        s.chats[kept++] = *c;
    }
    s.chats_n = kept;
}

static int find_chat_sid(const char *sid)
{
    for (int i = 0; i < s.chats_n; i++) {
        if (!strcasecmp(s.chats[i].sid, sid)) {
            return i;
        }
    }
    return -1;
}

static int find_chat_name(const char *name)
{
    for (int i = 0; i < s.chats_n; i++) {
        if (!strcasecmp(s.chats[i].name, name)) {
            return i;
        }
    }
    return -1;
}

/* A name without the spaces around it: false if that's empty, too long or holds a control character. */
static bool chat_name(const char *in, char out[MUSE_CHAT_NAME_MAX + 1])
{
    if (!in) {
        return false;
    }
    while (*in == ' ') {
        in++;
    }
    size_t n = strlen(in);
    while (n && in[n - 1] == ' ') {
        n--;
    }
    if (!n || n > MUSE_CHAT_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)in[i] < 0x20 || in[i] == 0x7f) {
            return false;
        }
    }
    memcpy(out, in, n);
    out[n] = '\0';
    return true;
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
    /* Older builds kept the picked chat; now each boot starts in the main one. */
    if (nvs_erase_key(s_nvs, "chat_sid") == ESP_OK) {
        nvs_commit(s_nvs);
    }
    load_chats();

    s.volume = clampi(s.volume, 0, 100);
    s.mic_gain = clampi(s.mic_gain, 0, MUSE_MIC_GAIN_MAX);
    s.brightness = clampi(s.brightness, 10, 100);
    s.gadget_mode = clampi(s.gadget_mode, 0, 2);
    ESP_LOGI(TAG, "vol %d%s, mic %d dB, bright %d, sleep %ds, wifi %s (%s), ble %s, muse %s",
             s.volume, s.speaker_on ? "" : " (speaker off)", s.mic_gain, s.brightness, s.sleep_s, s.wifi_on ? "on" : "off",
             "network saved by Link", s.ble_on ? "on" : "off", s.token[0] ? "token set" : "no token");
    ESP_LOGI(TAG, "muse chat: %s, %d named", s.chat_sid[0] ? s.chat_sid : "main", s.chats_n);
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

/*
 * A fixed prefix and the Wi-Fi MAC, so it's the same chat after a reflash or a
 * reset. The prefix is "musegadg" in ASCII, with the version nibble set to 4
 * and the variant to 8 so it reads as a valid random (v4) UUID.
 */
void muse_settings_gadget_chat_sid(char out[MUSE_CHAT_SID_MAX + 1])
{
    uint8_t m[6] = { 0 };
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(out, MUSE_CHAT_SID_MAX + 1, "6d757365-6761-4467-8000-%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3],
             m[4], m[5]);
}

bool muse_settings_chat_sid_valid(const char *sid)
{
    if (!sid || strlen(sid) != MUSE_CHAT_SID_MAX) {
        return false;
    }
    for (int i = 0; i < MUSE_CHAT_SID_MAX; i++) {
        char c = sid[i];
        bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (dash ? c != '-' : !hex) {
            return false;
        }
    }
    return true;
}

int muse_settings_chats(muse_chat_entry_t *out, int max)
{
    int n;
    LOCKED({
        n = s.chats_n < max ? s.chats_n : max;
        memcpy(out, s.chats, n * sizeof(s.chats[0]));
    });
    return n;
}

bool muse_settings_chat_find(const char *name, char sid_out[MUSE_CHAT_SID_MAX + 1])
{
    char want[MUSE_CHAT_NAME_MAX + 1];
    if (!chat_name(name, want)) {
        return false;
    }
    int i;
    LOCKED({
        i = find_chat_name(want);
        if (i >= 0 && sid_out) {
            strlcpy(sid_out, s.chats[i].sid, MUSE_CHAT_SID_MAX + 1);
        }
    });
    return i >= 0;
}

bool muse_settings_chat_name(const char *sid, char name_out[MUSE_CHAT_NAME_MAX + 1])
{
    if (!sid || !sid[0]) {
        return false;
    }
    int i;
    LOCKED({
        i = find_chat_sid(sid);
        if (i >= 0 && name_out) {
            strlcpy(name_out, s.chats[i].name, MUSE_CHAT_NAME_MAX + 1);
        }
    });
    return i >= 0;
}

esp_err_t muse_settings_chat_add(const char *name, char sid_out[MUSE_CHAT_SID_MAX + 1])
{
    char clean[MUSE_CHAT_NAME_MAX + 1];
    if (!chat_name(name, clean)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* A random (v4) UUID: version nibble 4, variant bits 10. */
    uint8_t u[16];
    esp_fill_random(u, sizeof(u));
    u[6] = (u[6] & 0x0f) | 0x40;
    u[8] = (u[8] & 0x3f) | 0x80;
    char sid[MUSE_CHAT_SID_MAX + 1];
    snprintf(sid, sizeof(sid), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0], u[1],
             u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    esp_err_t err = ESP_OK;
    LOCKED({
        int i = find_chat_name(clean);
        if (i >= 0) {
            strlcpy(sid, s.chats[i].sid, sizeof(sid));
            err = ESP_ERR_INVALID_STATE;
        } else if (s.chats_n >= MUSE_CHATS_MAX) {
            err = ESP_ERR_NO_MEM;
        } else {
            muse_chat_entry_t *c = &s.chats[s.chats_n++];
            memset(c, 0, sizeof(*c));
            strlcpy(c->name, clean, sizeof(c->name));
            strlcpy(c->sid, sid, sizeof(c->sid));
            save_chats();
            s.chats_gen++;
        }
    });
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "new chat \"%s\": %s", clean, sid);
    }
    if (sid_out && err != ESP_ERR_NO_MEM) {
        strlcpy(sid_out, sid, MUSE_CHAT_SID_MAX + 1);
    }
    return err;
}

esp_err_t muse_settings_chat_pick_new(void)
{
    uint8_t u[16];
    esp_fill_random(u, sizeof(u));
    u[6] = (u[6] & 0x0f) | 0x40;
    u[8] = (u[8] & 0x3f) | 0x80;
    char sid[MUSE_CHAT_SID_MAX + 1];
    snprintf(sid, sizeof(sid), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0], u[1],
             u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    bool full;
    LOCKED({
        full = s.chats_n >= MUSE_CHATS_MAX;
        if (!full) {
            strlcpy(s.new_sid, sid, sizeof(s.new_sid));
        }
    });
    if (full) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "new chat: %s, named once the Muse titles it", sid);
    muse_settings_set_chat_sid(sid);
    return ESP_OK;
}

bool muse_settings_chat_untitled(const char *sid)
{
    bool untitled = false;
    if (sid && sid[0]) {
        LOCKED(untitled = !strcmp(s.new_sid, sid));
    }
    return untitled;
}

bool muse_settings_chat_retitle(const char *sid, const char *title, bool *started)
{
    if (started) {
        *started = false;
    }
    char clean[MUSE_CHAT_NAME_MAX + 1];
    if (!sid || !sid[0] || !title) {
        return false;
    }
    /* A long title is cut at a word, with an ellipsis, to fit. */
    char cut[MUSE_CHAT_NAME_MAX + 1];
    if (strlen(title) > MUSE_CHAT_NAME_MAX) {
        size_t n = MUSE_CHAT_NAME_MAX - 3;
        while (n > 8 && title[n] != ' ') {
            n--;
        }
        snprintf(cut, sizeof(cut), "%.*s...", (int)n, title);
        title = cut;
    }
    if (!chat_name(title, clean)) {
        return false;
    }
    bool found = false, changed = false, joined = false;
    LOCKED({
        int i = find_chat_sid(sid);
        if (i < 0 && !strcmp(s.new_sid, sid) && s.chats_n < MUSE_CHATS_MAX) {
            /* The new chat has started: it joins the named ones, under its title. */
            i = s.chats_n++;
            memset(&s.chats[i], 0, sizeof(s.chats[i]));
            strlcpy(s.chats[i].sid, sid, sizeof(s.chats[i].sid));
            s.new_sid[0] = '\0';
            joined = true;
        }
        if (i >= 0) {
            found = true;
            changed = strcmp(s.chats[i].name, clean) != 0;
            if (changed) {
                strlcpy(s.chats[i].name, clean, sizeof(s.chats[i].name));
                s.chats_dirty = true;
                s.chats_gen++;
            }
        }
    });
    if (changed) {
        ESP_LOGI(TAG, "chat %s titled \"%s\"", sid, clean);
    }
    if (started) {
        *started = joined;
    }
    return found;
}

void muse_settings_chats_flush(void)
{
    LOCKED({
        if (s.chats_dirty) {
            s.chats_dirty = false;
            save_chats();
        }
    });
}

esp_err_t muse_settings_chat_new(const char *name, char sid_out[MUSE_CHAT_SID_MAX + 1])
{
    char sid[MUSE_CHAT_SID_MAX + 1];
    esp_err_t err = muse_settings_chat_add(name, sid);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        muse_settings_set_chat_sid(sid);
        if (sid_out) {
            strlcpy(sid_out, sid, MUSE_CHAT_SID_MAX + 1);
        }
    }
    return err;
}

static void chat_item(muse_chat_item_t *it, muse_chat_kind_t kind, const char *name, const char *sid)
{
    it->kind = kind;
    strlcpy(it->name, name, sizeof(it->name));
    strlcpy(it->sid, sid, sizeof(it->sid));
}

int muse_settings_chat_items(muse_chat_item_t *out, int max, int *current)
{
    char gadget[MUSE_CHAT_SID_MAX + 1];
    muse_settings_gadget_chat_sid(gadget);
    int n = 0, cur = 0;
    LOCKED({
        if (n < max) {
            chat_item(&out[n], MUSE_CHAT_MAIN, "Main chat", "");
        }
        n++;
        if (n < max) {
            chat_item(&out[n], MUSE_CHAT_GADGET, "Gadget chat", gadget);
        }
        if (!strcmp(s.chat_sid, gadget)) {
            cur = n;
        }
        n++;
        for (int i = 0; i < s.chats_n; i++, n++) {
            if (n < max) {
                chat_item(&out[n], MUSE_CHAT_NAMED, s.chats[i].name, s.chats[i].sid);
            }
            if (!strcmp(s.chat_sid, s.chats[i].sid)) {
                cur = n;
            }
        }
        /* Picked by its id alone (the set_chat command, the console). */
        bool is_new = s.new_sid[0] && !strcmp(s.chat_sid, s.new_sid);
        if (s.chat_sid[0] && !cur && !is_new) {
            if (n < max) {
                chat_item(&out[n], MUSE_CHAT_CUSTOM, "Other chat", s.chat_sid);
            }
            cur = n++;
        }
        if (n < max) {
            chat_item(&out[n], MUSE_CHAT_NEW, "New chat", is_new ? s.new_sid : "");
        }
        if (is_new) {
            cur = n;
        }
        n++;
    });
    if (current) {
        *current = cur < max ? cur : -1;
    }
    return n < max ? n : max;
}

uint32_t muse_settings_chats_gen(void)
{
    uint32_t gen;
    LOCKED(gen = s.chats_gen);
    return gen;
}

bool muse_settings_chat_forget(const char *sid)
{
    if (!sid || !sid[0]) {
        return false;
    }
    bool found = false, current = false;
    LOCKED({
        int i = find_chat_sid(sid);
        if (i >= 0) {
            found = true;
            current = !strcmp(s.chat_sid, s.chats[i].sid);
            ESP_LOGI(TAG, "forgot chat \"%s\": %s", s.chats[i].name, s.chats[i].sid);
            memmove(&s.chats[i], &s.chats[i + 1], (s.chats_n - i - 1) * sizeof(s.chats[0]));
            s.chats_n--;
            save_chats();
            s.chats_gen++;
        }
    });
    if (current) {
        muse_settings_set_chat_sid("");
    }
    return found;
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
    char want[MUSE_CHAT_SID_MAX + 1];
    strlcpy(want, sid, sizeof(want));
    lower(want);
    bool changed = false;
    LOCKED({
        changed = strcmp(s.chat_sid, want) != 0;
        if (changed) {
            strlcpy(s.chat_sid, want, sizeof(s.chat_sid));
            s.chats_gen++;
        }
    });
    if (changed) {
        ESP_LOGI(TAG, "muse chat: %s", want[0] ? want : "main");
        notify(MUSE_SETTING_CHAT);
    }
    return true;
}
