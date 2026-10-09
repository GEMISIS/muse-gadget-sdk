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
 * The named chats in muse_settings.c for test_muse_settings_chats.py, against
 * an NVS kept in memory: the "chats" blob read in the layout before each
 * chat kept the mode it last heard, and the modes chats are told. The case
 * to run is argv[1]. The IDF headers muse_settings.c includes are empty
 * files; what they would declare is faked here, before it's built in.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_compat.h"

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_NO_FREE_PAGES 0x110d
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110
#define ESP_ERROR_CHECK(x) assert((x) == ESP_OK)

/* To stderr, so the formats get checked against their arguments. */
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "%s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "%s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "%s: " fmt "\n", tag, ##__VA_ARGS__)

#define CONFIG_MUSE_DEFAULT_VOLUME 70
#define EXT_RAM_BSS_ATTR

typedef int SemaphoreHandle_t;
#define portMAX_DELAY 0
#define xSemaphoreCreateMutex() 1
#define xSemaphoreTake(lock, wait) ((void)(lock), (void)(wait))
#define xSemaphoreGive(lock) ((void)(lock))

#define ESP_MAC_WIFI_STA 0
#define GADGET_SID "6d757365-6761-4467-8000-0a1b2c3d4e5f"

static esp_err_t esp_read_mac(uint8_t *mac, int type)
{
    static const uint8_t m[6] = { 0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f };
    (void)type;
    memcpy(mac, m, sizeof(m));
    return ESP_OK;
}

static void esp_fill_random(void *buf, size_t n)
{
    static uint8_t next = 1;
    for (size_t i = 0; i < n; i++) {
        ((uint8_t *)buf)[i] = next++ * 37;
    }
}

static bool muse_link_wifi_get(char *ssid, char *pass)
{
    (void)ssid;
    (void)pass;
    return false;
}

static bool muse_link_wifi_set(const char *ssid, const char *pass)
{
    (void)ssid;
    (void)pass;
    return false;
}

/* ---- NVS, in memory: each key holds its value's bytes ---- */

typedef uint32_t nvs_handle_t;
#define NVS_READWRITE 1

static struct {
    char key[16];
    uint8_t data[1024];
    size_t len;
} s_store[32];
static int s_store_n;

static int store_find(const char *key)
{
    for (int i = 0; i < s_store_n; i++) {
        if (!strcmp(s_store[i].key, key)) {
            return i;
        }
    }
    return -1;
}

static esp_err_t store_set(const char *key, const void *data, size_t len)
{
    assert(strlen(key) <= 15 && len <= sizeof(s_store[0].data));   /* NVS keys are 15 characters at most */
    int i = store_find(key);
    if (i < 0) {
        assert(s_store_n < 32);
        i = s_store_n++;
        strcpy(s_store[i].key, key);
    }
    memcpy(s_store[i].data, data, len);
    s_store[i].len = len;
    return ESP_OK;
}

static esp_err_t store_get(const char *key, void *out, size_t len)
{
    int i = store_find(key);
    if (i < 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    assert(s_store[i].len == len);
    memcpy(out, s_store[i].data, len);
    return ESP_OK;
}

static esp_err_t nvs_flash_init(void) { return ESP_OK; }
static esp_err_t nvs_flash_erase(void) { return ESP_OK; }
static esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) { (void)ns; (void)mode; *h = 1; return ESP_OK; }
static esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
static esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v) { (void)h; return store_set(k, &v, 1); }
static esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v) { (void)h; return store_get(k, v, 1); }
static esp_err_t nvs_set_i8(nvs_handle_t h, const char *k, int8_t v) { (void)h; return store_set(k, &v, 1); }
static esp_err_t nvs_get_i8(nvs_handle_t h, const char *k, int8_t *v) { (void)h; return store_get(k, v, 1); }
static esp_err_t nvs_set_u16(nvs_handle_t h, const char *k, uint16_t v) { (void)h; return store_set(k, &v, 2); }
static esp_err_t nvs_get_u16(nvs_handle_t h, const char *k, uint16_t *v) { (void)h; return store_get(k, v, 2); }
static esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v) { (void)h; return store_set(k, &v, 4); }
static esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *v) { (void)h; return store_get(k, v, 4); }
static esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) { (void)h; return store_set(k, v, strlen(v) + 1); }
static esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t n) { (void)h; return store_set(k, v, n); }

static esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *out, size_t *len)
{
    (void)h;
    int i = store_find(k);
    if (i < 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (*len < s_store[i].len) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, s_store[i].data, s_store[i].len);
    *len = s_store[i].len;
    return ESP_OK;
}

/* As IDF's: a NULL buffer asks for the length; one too small is refused. */
static esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *out, size_t *len)
{
    (void)h;
    int i = store_find(k);
    if (i < 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (out && *len < s_store[i].len) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    if (out) {
        memcpy(out, s_store[i].data, s_store[i].len);
    }
    *len = s_store[i].len;
    return ESP_OK;
}

static esp_err_t nvs_erase_key(nvs_handle_t h, const char *k)
{
    (void)h;
    int i = store_find(k);
    if (i < 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    s_store[i] = s_store[--s_store_n];
    return ESP_OK;
}

#include "muse_settings.c"

/* ---- Cases ---- */

#define WORK "7d3f2a10-5b6c-4e8d-9a1f-288485906f44"
#define HOME "0b5e7c92-3687-4fc7-aab0-6280b15fd2ad"

/* A blob as builds before told_mode wrote it: 70-byte entries, name then id. */
typedef struct {
    char name[33];
    char sid[37];
} old_entry_t;

static void put_old_chats(void)
{
    old_entry_t old[3];
    memset(old, 0, sizeof(old));
    strcpy(old[0].name, "Hangboard session warm-up");
    strcpy(old[0].sid, "7D3F2A10-5B6C-4E8D-9A1F-288485906F44");   /* kept in lower case from now on */
    strcpy(old[1].name, "Broken");
    strcpy(old[1].sid, "not-a-uuid");                              /* dropped */
    strcpy(old[2].name, "Stretches after bouldering");
    strcpy(old[2].sid, HOME);
    assert(sizeof(old_entry_t) == 70 && sizeof(muse_chat_entry_t) == 71);
    store_set("chats", old, sizeof(old));
}

static void expect_chats(int n)
{
    muse_chat_entry_t chats[MUSE_CHATS_MAX];
    assert(muse_settings_chats(chats, MUSE_CHATS_MAX) == n);
}

/* The entries in the "chats" blob as kept now. */
static int saved_chats(muse_chat_entry_t *out)
{
    int i = store_find("chats");
    if (i < 0) {
        return 0;
    }
    assert(s_store[i].len % sizeof(muse_chat_entry_t) == 0);
    memcpy(out, s_store[i].data, s_store[i].len);
    return (int)(s_store[i].len / sizeof(muse_chat_entry_t));
}

/* Named chats from before told_mode survive, told nothing, and are saved in the new layout. */
static void migrates_old_blob(void)
{
    put_old_chats();
    assert(muse_settings_init() == ESP_OK);
    muse_chat_entry_t chats[MUSE_CHATS_MAX];
    assert(muse_settings_chats(chats, MUSE_CHATS_MAX) == 2);
    assert(!strcmp(chats[0].name, "Hangboard session warm-up") && !strcmp(chats[0].sid, WORK));
    assert(!strcmp(chats[1].name, "Stretches after bouldering") && !strcmp(chats[1].sid, HOME));
    assert(chats[0].told_mode == -1 && chats[1].told_mode == -1);
    assert(muse_settings_chat_told(WORK) == -1 && muse_settings_chat_told("") == -1);

    muse_chat_entry_t saved[MUSE_CHATS_MAX];
    assert(saved_chats(saved) == 2);
    assert(!strcmp(saved[0].sid, WORK) && saved[0].told_mode == -1 && saved[1].told_mode == -1);

    /* Read back in the new layout, as the next boot does. */
    memset(s.chats, 0, sizeof(s.chats));
    s.chats_n = 0;
    assert(muse_settings_init() == ESP_OK);
    assert(muse_settings_chats(chats, MUSE_CHATS_MAX) == 2);
    assert(!strcmp(chats[1].name, "Stretches after bouldering") && chats[1].told_mode == -1);
}

/* What each chat was told is kept: main and gadget in their own keys, a named chat in its entry. */
static void told_modes_persist(void)
{
    assert(muse_settings_init() == ESP_OK);
    char work[MUSE_CHAT_SID_MAX + 1];
    assert(muse_settings_chat_add("Work", work) == ESP_OK);
    assert(muse_settings_chat_told(work) == -1);
    uint32_t gen = muse_settings_chats_gen();
    muse_settings_chat_set_told(work, 2);
    muse_settings_chat_set_told("", 1);
    muse_settings_chat_set_told(GADGET_SID, 0);
    assert(muse_settings_chats_gen() != gen);   /* the Chats screen shows it */
    assert(muse_settings_chat_told(work) == 2 && muse_settings_chat_told("") == 1);
    assert(muse_settings_chat_told(GADGET_SID) == 0);
    muse_settings_chat_set_told(work, 7);   /* not a mode: ignored */
    assert(muse_settings_chat_told(work) == 2);

    /* Nothing written until the flush, which the mode task makes. */
    assert(store_find("told_main") < 0);
    muse_chat_entry_t saved[MUSE_CHATS_MAX];
    assert(saved_chats(saved) == 1 && saved[0].told_mode == -1);
    muse_settings_chats_flush();
    assert(saved_chats(saved) == 1 && saved[0].told_mode == 2);
    int8_t v;
    assert(nvs_get_i8(0, "told_main", &v) == ESP_OK && v == 1);
    assert(nvs_get_i8(0, "told_gadget", &v) == ESP_OK && v == 0);

    /* The next boot. */
    memset(s.chats, 0, sizeof(s.chats));
    s.chats_n = 0;
    s.told_main = s.told_gadget = -1;
    assert(muse_settings_init() == ESP_OK);
    assert(muse_settings_chat_told(work) == 2 && muse_settings_chat_told("") == 1);
    assert(muse_settings_chat_told(GADGET_SID) == 0);
    muse_chat_item_t items[MUSE_CHAT_ITEMS_MAX];
    int n = muse_settings_chat_items(items, MUSE_CHAT_ITEMS_MAX, NULL);
    assert(n == 4);   /* main, gadget, Work, New chat */
    assert(items[0].told_mode == 1 && items[1].told_mode == 0 && items[2].told_mode == 2);
    assert(items[3].kind == MUSE_CHAT_NEW && items[3].told_mode == -1);
}

/* A new chat told the mode by its first message keeps it once the Muse titles it. */
static void new_chat_keeps_its_mode(void)
{
    assert(muse_settings_init() == ESP_OK);
    assert(muse_settings_chat_pick_new() == ESP_OK);
    char sid[MUSE_CHAT_SID_MAX + 1];
    muse_settings_chat_sid(sid);
    assert(muse_settings_chat_untitled(sid) && muse_settings_chat_told(sid) == -1);
    muse_settings_chat_set_told(sid, 1);   /* its first message's ack */
    assert(muse_settings_chat_told(sid) == 1);
    expect_chats(0);
    bool started;
    assert(muse_settings_chat_retitle(sid, "Hangboard session warm-up", &started) && started);
    muse_chat_entry_t chats[MUSE_CHATS_MAX];
    assert(muse_settings_chats(chats, MUSE_CHATS_MAX) == 1 && chats[0].told_mode == 1);
    muse_settings_chats_flush();
    muse_chat_entry_t saved[MUSE_CHATS_MAX];
    assert(saved_chats(saved) == 1 && !strcmp(saved[0].name, "Hangboard session warm-up"));
    assert(saved[0].told_mode == 1);

    /* The next new chat starts told nothing. */
    assert(muse_settings_chat_pick_new() == ESP_OK);
    muse_settings_chat_sid(sid);
    assert(muse_settings_chat_told(sid) == -1);
}

/* A chat picked by its id alone keeps what it was told in RAM, while it's the one. */
static void other_chat_in_ram(void)
{
    assert(muse_settings_init() == ESP_OK);
    assert(muse_settings_set_chat_sid(WORK));
    assert(muse_settings_chat_told(WORK) == -1);
    muse_settings_chat_set_told(WORK, 2);
    assert(muse_settings_chat_told(WORK) == 2);
    muse_chat_item_t items[MUSE_CHAT_ITEMS_MAX];
    int current;
    int n = muse_settings_chat_items(items, MUSE_CHAT_ITEMS_MAX, &current);
    assert(items[current].kind == MUSE_CHAT_CUSTOM && items[current].told_mode == 2);
    muse_settings_chat_set_told(HOME, 0);   /* another takes its place */
    assert(muse_settings_chat_told(WORK) == -1 && muse_settings_chat_told(HOME) == 0);
    muse_settings_chats_flush();
    assert(store_find("chats") < 0 && store_find("told_main") < 0);
    (void)n;
}

/* A blob in neither layout is ignored, not misread. */
static void unknown_blob_ignored(void)
{
    uint8_t junk[75] = { 0 };
    store_set("chats", junk, sizeof(junk));
    assert(muse_settings_init() == ESP_OK);
    expect_chats(0);
}

/* Told under older words (no "told_ver", or another): every chat hears the new ones, once. */
static void old_words_forgotten(void)
{
    assert(muse_settings_init() == ESP_OK);
    char work[MUSE_CHAT_SID_MAX + 1];
    assert(muse_settings_chat_add("Work", work) == ESP_OK);
    muse_settings_chat_set_told(work, 2);
    muse_settings_chat_set_told("", 1);
    muse_settings_chat_set_told(GADGET_SID, 0);
    muse_settings_chats_flush();
    /* An older build's flash: no version kept. */
    assert(nvs_erase_key(0, "told_ver") == ESP_OK);
    memset(s.chats, 0, sizeof(s.chats));
    s.chats_n = 0;
    assert(muse_settings_init() == ESP_OK);
    assert(muse_settings_chat_told(work) == -1 && muse_settings_chat_told("") == -1);
    assert(muse_settings_chat_told(GADGET_SID) == -1);
    muse_chat_entry_t saved[MUSE_CHATS_MAX];
    assert(saved_chats(saved) == 1 && saved[0].told_mode == -1);
    assert(store_find("told_main") < 0 && store_find("told_gadget") < 0);
    uint8_t v = 0;
    assert(nvs_get_u8(0, "told_ver", &v) == ESP_OK && v == MUSE_SETTINGS_CONTRACTS_VERSION);
    /* Told again: kept from now on. */
    muse_settings_chat_set_told(work, 0);
    muse_settings_chat_set_told("", 0);
    muse_settings_chats_flush();
    memset(s.chats, 0, sizeof(s.chats));
    s.chats_n = 0;
    s.told_main = -1;
    assert(muse_settings_init() == ESP_OK);
    assert(muse_settings_chat_told(work) == 0 && muse_settings_chat_told("") == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0: migrates_old_blob(); break;
    case 1: told_modes_persist(); break;
    case 2: new_chat_keeps_its_mode(); break;
    case 3: other_chat_in_ram(); break;
    case 4: unknown_blob_ignored(); break;
    case 5: old_words_forgotten(); break;
    default: return 2;
    }
    return 0;
}
