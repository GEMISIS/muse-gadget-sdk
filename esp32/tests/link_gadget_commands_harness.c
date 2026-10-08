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

// Host harness for show_text, set_mode, set_chat, list_chats and
// display.show_image: params in, command result and calls into the Muse UI out. The runner extracts the production command code
// from main/gadget_commands.c into gadget_commands.inc.
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "host_compat.h"

// ---- Fakes for muse_state.h and muse_gadget_mode.h ----

#define MUSE_CAPTION_MAX 400

typedef enum {
    MUSE_GADGET_DESK,
    MUSE_GADGET_NIGHT,
    MUSE_GADGET_ON_THE_GO,
    MUSE_GADGET_MODE_COUNT,
} muse_gadget_mode_t;

static char s_caption[1024];
static int s_captions, s_wakes, s_sleeps, s_pokes, s_picks;
static muse_gadget_mode_t s_picked;

static void muse_state_set_caption(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_caption, sizeof(s_caption), fmt, ap);
    va_end(ap);
    s_captions++;
}

static void muse_state_set_asleep(bool asleep) {
    if (asleep) {
        s_sleeps++;
    } else {
        s_wakes++;
    }
}

static void muse_state_poke(void) { s_pokes++; }

// The same names as muse_gadget_mode.c's; the runner checks they match.
static bool muse_gadget_mode_parse(const char *name, muse_gadget_mode_t *out) {
    static const char *const keys[] = {"desk", "night", "on_the_go"};
    for (int m = 0; m < MUSE_GADGET_MODE_COUNT; m++) {
        if (!strcmp(name, keys[m])) {
            *out = (muse_gadget_mode_t)m;
            return true;
        }
    }
    return false;
}

static void muse_gadget_mode_pick(muse_gadget_mode_t mode) {
    s_picked = mode;
    s_picks++;
}

static const char *muse_gadget_mode_key(muse_gadget_mode_t mode) {
    static const char *const keys[] = {"desk", "night", "on_the_go"};
    return mode < MUSE_GADGET_MODE_COUNT ? keys[mode] : NULL;
}

// ---- Fakes for muse_settings.h ----

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103

#define MUSE_CHAT_SID_MAX 36
#define MUSE_CHAT_NAME_MAX 32
#define MUSE_CHATS_MAX 8
#define GADGET_SID "6d757365-6761-4467-8000-0a1b2c3d4e5f"

typedef struct {
    char name[MUSE_CHAT_NAME_MAX + 1];
    char sid[MUSE_CHAT_SID_MAX + 1];
    int8_t told_mode;
} muse_chat_entry_t;

static char s_chat_sid[MUSE_CHAT_SID_MAX + 1];
static int s_main_told = -1;
static int s_chat_sets, s_chat_adds;
static muse_chat_entry_t s_chats[MUSE_CHATS_MAX];
static int s_chats_n;

static void muse_settings_chat_sid(char out[MUSE_CHAT_SID_MAX + 1]) {
    strcpy(out, s_chat_sid);
}

static void muse_settings_gadget_chat_sid(char out[MUSE_CHAT_SID_MAX + 1]) {
    strcpy(out, GADGET_SID);
}

// The firmware keeps ids in lower case; the command must hand them over so.
static bool muse_settings_set_chat_sid(const char *sid) {
    assert(!sid[0] || strlen(sid) == MUSE_CHAT_SID_MAX);
    for (const char *c = sid; *c; c++) {
        assert(!(*c >= 'A' && *c <= 'Z'));
    }
    strcpy(s_chat_sid, sid);
    s_chat_sets++;
    return true;
}

static int muse_settings_chats(muse_chat_entry_t *out, int max) {
    int n = s_chats_n < max ? s_chats_n : max;
    memcpy(out, s_chats, n * sizeof(s_chats[0]));
    return n;
}

// Exact names here; ignoring case and spaces is muse_settings.c's business.
static bool muse_settings_chat_find(const char *name, char sid_out[MUSE_CHAT_SID_MAX + 1]) {
    for (int i = 0; i < s_chats_n; i++) {
        if (!strcmp(s_chats[i].name, name)) {
            strcpy(sid_out, s_chats[i].sid);
            return true;
        }
    }
    return false;
}

static int muse_settings_chat_told(const char *sid) {
    if (!sid[0]) {
        return s_main_told;
    }
    for (int i = 0; i < s_chats_n; i++) {
        if (!strcmp(s_chats[i].sid, sid)) {
            return s_chats[i].told_mode;
        }
    }
    return -1;
}

static bool muse_settings_chat_untitled(const char *sid) {
    (void)sid;
    return false;
}

static bool muse_settings_chat_name(const char *sid, char name_out[MUSE_CHAT_NAME_MAX + 1]) {
    for (int i = 0; i < s_chats_n; i++) {
        if (!strcmp(s_chats[i].sid, sid)) {
            strcpy(name_out, s_chats[i].name);
            return true;
        }
    }
    return false;
}

static esp_err_t muse_settings_chat_add(const char *name, char sid_out[MUSE_CHAT_SID_MAX + 1]) {
    if (!name[0] || strlen(name) > MUSE_CHAT_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_chats_n == MUSE_CHATS_MAX) {
        return ESP_ERR_NO_MEM;
    }
    muse_chat_entry_t *c = &s_chats[s_chats_n++];
    c->told_mode = -1;
    strcpy(c->name, name);
    snprintf(c->sid, sizeof(c->sid), "00000000-0000-4000-8000-%012d", s_chats_n);
    strcpy(sid_out, c->sid);
    s_chat_adds++;
    return ESP_OK;
}

// ---- Fakes for esp_heap_caps.h, esp_timer.h and muse_present.h ----

#define CONFIG_MUSE_HATCH 1
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MUSE_PRESENT_MAX (512 * 1024)

static int s_reallocs;
static size_t s_last_alloc;
static bool s_fail_alloc;
static int64_t s_now_us = 1000000;
static uint8_t *s_presented;
static size_t s_presented_len;
static char s_presented_label[64];
static int s_presents;

// The image's buffer must be PSRAM's: internal RAM can't spare it.
static void *heap_caps_realloc(void *p, size_t n, uint32_t caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(n <= MUSE_PRESENT_MAX);
    if (s_fail_alloc) {
        return NULL;
    }
    s_reallocs++;
    s_last_alloc = n;
    return realloc(p, n);
}

static void heap_caps_free(void *p) { free(p); }

static int64_t esp_timer_get_time(void) { return s_now_us; }

static bool muse_present_bytes(uint8_t *data, size_t len, const char *label) {
    free(s_presented);
    s_presented = data;
    s_presented_len = len;
    strlcpy(s_presented_label, label, sizeof(s_presented_label));
    s_presents++;
    return true;
}

#include "gadget_commands.inc"

// ---- Checks ----

static cJSON *params(const char *json) {
    cJSON *p = cJSON_Parse(json);
    assert(p);
    return p;
}

static void expect_error(cJSON *result, const char *code) {
    assert(cJSON_IsFalse(cJSON_GetObjectItem(result, "ok")));
    cJSON *error = cJSON_GetObjectItem(result, "error");
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(error, "code")), code));
    assert(cJSON_IsString(cJSON_GetObjectItem(error, "message")));
    cJSON_Delete(result);
}

static void expect_ok(cJSON *result) {
    assert(cJSON_IsTrue(cJSON_GetObjectItem(result, "ok")));
    assert(!cJSON_GetObjectItem(result, "error"));
}

static void test_show_text(void) {
    // No params, a missing, wrong-typed, empty or too long text: nothing shown.
    expect_error(gadget_show_text_command(NULL), "missing_param");
    cJSON *p = params("{\"txt\":\"hi\"}");
    expect_error(gadget_show_text_command(p), "missing_param");
    cJSON_Delete(p);
    p = params("{\"text\":42}");
    expect_error(gadget_show_text_command(p), "missing_param");
    cJSON_Delete(p);
    p = params("{\"text\":\"\"}");
    expect_error(gadget_show_text_command(p), "invalid_param");
    cJSON_Delete(p);
    char long_text[MUSE_CAPTION_MAX + 2];
    memset(long_text, 'a', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "text", long_text);
    expect_error(gadget_show_text_command(p), "invalid_param");
    cJSON_Delete(p);
    assert(s_captions == 0 && s_wakes == 0 && s_pokes == 0);

    // Exactly the limit fits.
    long_text[MUSE_CAPTION_MAX] = '\0';
    p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "text", long_text);
    cJSON *result = gadget_show_text_command(p);
    expect_ok(result);
    cJSON_Delete(result);
    cJSON_Delete(p);
    assert(strlen(s_caption) == MUSE_CAPTION_MAX);

    // Shown as given (a % is text, not a format), and the screen wakes.
    p = params("{\"text\":\"Build 100% green\"}");
    result = gadget_show_text_command(p);
    expect_ok(result);
    cJSON_Delete(result);
    cJSON_Delete(p);
    assert(!strcmp(s_caption, "Build 100% green"));
    assert(s_captions == 2 && s_wakes == 2 && s_pokes == 2 && s_sleeps == 0);
}

static void test_set_mode(void) {
    expect_error(gadget_set_mode_command(NULL), "missing_param");
    cJSON *p = params("{\"mode\":true}");
    expect_error(gadget_set_mode_command(p), "missing_param");
    cJSON_Delete(p);
    const char *const bad[] = {"\"\"", "\"Night\"", "\"on-the-go\"", "\"sleep\""};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char json[64];
        snprintf(json, sizeof(json), "{\"mode\":%s}", bad[i]);
        p = params(json);
        expect_error(gadget_set_mode_command(p), "invalid_param");
        cJSON_Delete(p);
    }
    assert(s_picks == 0);

    const char *const good[] = {"desk", "night", "on_the_go"};
    for (int m = 0; m < MUSE_GADGET_MODE_COUNT; m++) {
        p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "mode", good[m]);
        cJSON *result = gadget_set_mode_command(p);
        expect_ok(result);
        cJSON *payload = cJSON_GetObjectItem(result, "payload");
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(payload, "mode")), good[m]));
        cJSON_Delete(result);
        cJSON_Delete(p);
        assert(s_picks == m + 1 && s_picked == (muse_gadget_mode_t)m);
    }
}

static cJSON *expect_chat(const char *json, const char *chat, const char *sid) {
    cJSON *p = json ? params(json) : NULL;
    cJSON *result = gadget_set_chat_command(p);
    expect_ok(result);
    cJSON *payload = cJSON_GetObjectItem(result, "payload");
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(payload, "chat")), chat));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(payload, "session_id")), sid));
    assert(!strcmp(s_chat_sid, sid));
    cJSON_Delete(p);
    return result;
}

static void expect_chat_only(const char *json, const char *chat, const char *sid) {
    cJSON *result = expect_chat(json, chat, sid);
    assert(!cJSON_GetObjectItem(cJSON_GetObjectItem(result, "payload"), "name"));
    cJSON_Delete(result);
}

#define UUID "7d3f2a10-5b6c-4e8d-9a1f-288485906f44"

static void test_set_chat(void) {
    // Wrong types, ids that aren't UUIDs (Muse answers those with HTTP 400),
    // and both a name and an id: nothing saved.
    const char *const bad[] = {
        "{\"session_id\":42}", "{\"session_id\":true}", "{\"session_id\":[\"a\"]}",
        "{\"session_id\":\"../x\"}", "{\"session_id\":\"side chat\"}",
        "{\"session_id\":\"gadget-288485906f44\"}", "{\"session_id\":\"Side-Chat-2\"}",
        "{\"session_id\":\"7d3f2a10-5b6c-4e8d-9a1f-288485906f4\"}",      // one digit short
        "{\"session_id\":\"7d3f2a10-5b6c-4e8d-9a1f-288485906f445\"}",    // one too many
        "{\"session_id\":\"7d3f2a105-b6c-4e8d-9a1f-288485906f44\"}",     // dash moved
        "{\"session_id\":\"7d3f2a10-5b6c-4e8d-9a1f-288485906g44\"}",     // not hex
        "{\"session_id\":\"{7d3f2a10-5b6c-4e8d-9a1f-288485906f4}\"}",
        "{\"session_id\":\"7d3f2a10\\u00e9b6c-4e8d-9a1f-288485906f44\"}",
        "{\"name\":42}", "{\"name\":false}",
        "{\"name\":\"Work\",\"session_id\":\"" UUID "\"}",
        "{\"name\":\"\"}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        cJSON *p = params(bad[i]);
        cJSON *result = gadget_set_chat_command(p);
        expect_error(result, "invalid_param");
        cJSON_Delete(p);
    }
    char long_name[MUSE_CHAT_NAME_MAX + 2];
    memset(long_name, 'n', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "name", long_name);
    expect_error(gadget_set_chat_command(p), "invalid_param");
    cJSON_Delete(p);
    assert(s_chat_sets == 0 && s_chat_adds == 0);

    // A UUID is taken in either case and kept in lower case.
    expect_chat_only("{\"session_id\":\"" UUID "\"}", "custom", UUID);
    expect_chat_only("{\"session_id\":\"7D3F2A10-5B6C-4E8D-9A1F-288485906F44\"}", "custom", UUID);

    // The gadget's own chat, by name or by its id.
    expect_chat_only("{\"session_id\":\"gadget\"}", "gadget", GADGET_SID);
    expect_chat_only("{\"session_id\":\"" GADGET_SID "\"}", "gadget", GADGET_SID);

    // The main chat: no params, no session_id, null, empty or "main".
    expect_chat_only(NULL, "main", "");
    expect_chat_only("{\"session_id\":\"" UUID "\"}", "custom", UUID);
    expect_chat_only("{}", "main", "");
    expect_chat_only("{\"session_id\":null}", "main", "");
    expect_chat_only("{\"session_id\":\"\"}", "main", "");
    expect_chat_only("{\"session_id\":\"main\"}", "main", "");
    assert(s_chat_sets == 10 && s_chat_adds == 0);

    // A name new to the gadget makes that chat; the same name picks it again.
    cJSON *result = expect_chat("{\"name\":\"Work\"}", "named", "00000000-0000-4000-8000-000000000001");
    cJSON *payload = cJSON_GetObjectItem(result, "payload");
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(payload, "name")), "Work"));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(payload, "created")));
    cJSON_Delete(result);
    expect_chat_only("{\"session_id\":\"main\"}", "main", "");
    result = expect_chat("{\"name\":\"Work\",\"session_id\":null}", "named",
                         "00000000-0000-4000-8000-000000000001");
    payload = cJSON_GetObjectItem(result, "payload");
    assert(cJSON_IsFalse(cJSON_GetObjectItem(payload, "created")));
    cJSON_Delete(result);
    assert(s_chat_adds == 1);

    // Its id names it too, and as a named chat.
    result = expect_chat("{\"session_id\":\"00000000-0000-4000-8000-000000000001\"}", "named",
                         "00000000-0000-4000-8000-000000000001");
    payload = cJSON_GetObjectItem(result, "payload");
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(payload, "name")), "Work"));
    assert(!cJSON_GetObjectItem(payload, "created"));
    cJSON_Delete(result);

    // Exactly the longest name fits; past MUSE_CHATS_MAX nothing more is kept.
    long_name[MUSE_CHAT_NAME_MAX] = '\0';
    p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "name", long_name);
    result = gadget_set_chat_command(p);
    expect_ok(result);
    cJSON_Delete(result);
    cJSON_Delete(p);
    for (int i = s_chats_n; i < MUSE_CHATS_MAX; i++) {
        char json[64];
        snprintf(json, sizeof(json), "{\"name\":\"Chat %d\"}", i);
        p = params(json);
        result = gadget_set_chat_command(p);
        expect_ok(result);
        cJSON_Delete(result);
        cJSON_Delete(p);
    }
    int sets = s_chat_sets;
    p = params("{\"name\":\"One too many\"}");
    expect_error(gadget_set_chat_command(p), "limit_reached");
    cJSON_Delete(p);
    assert(s_chat_sets == sets && s_chats_n == MUSE_CHATS_MAX);
}

static void test_list_chats(void) {
    s_chats_n = 0;
    expect_chat_only(NULL, "main", "");
    cJSON *result = gadget_list_chats_command(NULL);
    expect_ok(result);
    cJSON *payload = cJSON_GetObjectItem(result, "payload");
    cJSON *current = cJSON_GetObjectItem(payload, "current");
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(current, "chat")), "main"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(current, "session_id")), ""));
    assert(cJSON_IsNull(cJSON_GetObjectItem(current, "told_mode")));   // never told one
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(payload, "chats")) == 0);
    cJSON_Delete(result);
    s_main_told = MUSE_GADGET_NIGHT;
    result = gadget_list_chats_command(NULL);
    current = cJSON_GetObjectItem(cJSON_GetObjectItem(result, "payload"), "current");
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(current, "told_mode")), "night"));
    cJSON_Delete(result);

    cJSON_Delete(expect_chat("{\"name\":\"Work\"}", "named", "00000000-0000-4000-8000-000000000001"));
    cJSON_Delete(expect_chat("{\"name\":\"Home\"}", "named", "00000000-0000-4000-8000-000000000002"));
    s_chats[0].told_mode = MUSE_GADGET_ON_THE_GO;
    s_chats[1].told_mode = MUSE_GADGET_DESK;
    result = gadget_list_chats_command(NULL);
    expect_ok(result);
    payload = cJSON_GetObjectItem(result, "payload");
    current = cJSON_GetObjectItem(payload, "current");
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(current, "chat")), "named"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(current, "name")), "Home"));
    cJSON *chats = cJSON_GetObjectItem(payload, "chats");
    assert(cJSON_GetArraySize(chats) == 2);
    cJSON *first = cJSON_GetArrayItem(chats, 0);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(first, "name")), "Work"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(first, "session_id")),
                   "00000000-0000-4000-8000-000000000001"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(first, "told_mode")), "on_the_go"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(current, "told_mode")), "desk"));
    cJSON_Delete(result);
}

// ---- display.show_image ----

static char *b64(const uint8_t *d, size_t n) {
    static const char *abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = malloc((n + 2) / 3 * 4 + 1), *o = out;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        *o++ = abc[v >> 18 & 63];
        *o++ = abc[v >> 12 & 63];
        *o++ = i + 1 < n ? abc[v >> 6 & 63] : '=';
        *o++ = i + 2 < n ? abc[v & 63] : '=';
    }
    *o = '\0';
    return out;
}

// A baseline (or progressive) JPEG's markers, then `body` bytes of scan.
static uint8_t *jpeg(size_t body, bool progressive, size_t *len) {
    static const uint8_t head[] = {
        0xFF, 0xD8,
        0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0,
        0xFF, 0xC0, 0x00, 0x0B, 8, 0, 16, 0, 16, 1, 1, 0x11, 0,
        0xFF, 0xDA, 0x00, 0x08, 1, 1, 0, 0, 0x3F, 0,
    };
    *len = sizeof(head) + body + 2;
    uint8_t *d = malloc(*len);
    memcpy(d, head, sizeof(head));
    if (progressive) {
        d[21] = 0xC2;
    }
    for (size_t i = 0; i < body; i++) {
        d[sizeof(head) + i] = (uint8_t)(i * 7 + 3);
    }
    d[*len - 2] = 0xFF;
    d[*len - 1] = 0xD9;
    return d;
}

// Sends d[offset, offset + n) as one chunk, with any extra params.
static cJSON *chunk(const uint8_t *d, size_t offset, size_t n, bool final, const char *extra) {
    char *data = b64(d + offset, n);
    size_t cap = strlen(data) + 256;
    char *json = malloc(cap);
    snprintf(json, cap, "{\"data_b64\":\"%s\",\"offset\":%u,\"final\":%s%s%s}", data, (unsigned)offset,
             final ? "true" : "false", extra ? "," : "", extra ? extra : "");
    cJSON *p = params(json);
    cJSON *result = gadget_show_image_command(p);
    cJSON_Delete(p);
    free(json);
    free(data);
    return result;
}

static void expect_progress(cJSON *result, size_t received, bool complete) {
    expect_ok(result);
    cJSON *payload = cJSON_GetObjectItem(result, "payload");
    assert(cJSON_GetObjectItem(payload, "received")->valuedouble == (double)received);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(payload, "complete")) == complete);
    cJSON_Delete(result);
}

static void expect_message(cJSON *result, const char *code, const char *message) {
    cJSON *error = cJSON_GetObjectItem(result, "error");
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(error, "message")), message));
    expect_error(result, code);
}

// Sends the whole image in chunks of `step`; returns the final result.
static cJSON *push(const uint8_t *d, size_t len, size_t step, const char *extra) {
    for (size_t off = 0;; off += step) {
        bool final = off + step >= len;
        cJSON *result = chunk(d, off, final ? len - off : step, final, off ? NULL : extra);
        if (final) {
            return result;
        }
        expect_progress(result, off + step, false);
    }
}

static void test_show_image_params(void) {
    expect_error(gadget_show_image_command(NULL), "missing_param");
    cJSON *p = params("{\"data_b64\":42}");
    expect_error(gadget_show_image_command(p), "missing_param");
    cJSON_Delete(p);
    const char *const bad[] = {
        "{\"data_b64\":\"/9j/\",\"offset\":-1}",
        "{\"data_b64\":\"/9j/\",\"offset\":1.5}",
        "{\"data_b64\":\"/9j/\",\"offset\":\"0\"}",
        "{\"data_b64\":\"/9j/\",\"final\":1}",
        "{\"data_b64\":\"/9j/\",\"label\":7}",
        "{\"data_b64\":\"/9j/\",\"mime\":\"image/gif\"}",
        "{\"data_b64\":\"/9j/\",\"size\":0}",
        // Not base64: a bad character, data after the padding, a lone last
        // character, too much padding.
        "{\"data_b64\":\"/9j@\"}",
        "{\"data_b64\":\"/9==/9j/\"}",
        "{\"data_b64\":\"/9j/4\"}",
        "{\"data_b64\":\"/9===\"}",
        "{\"data_b64\":\"\"}",                          // empty, and not the last
        "{\"data_b64\":\"aGVsbG8=\"}",                  // "hello": not a JPEG
        "{\"data_b64\":\"/9j/\",\"offset\":4}",        // nothing under way: 0 comes first
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        p = params(bad[i]);
        cJSON *result = gadget_show_image_command(p);
        expect_error(result, "invalid_param");
        cJSON_Delete(p);
    }
    p = params("{\"data_b64\":\"/9j/\",\"offset\":4}");
    expect_message(gadget_show_image_command(p), "invalid_param", "expected offset 0");
    cJSON_Delete(p);
    // PNG, by its mime or its bytes: Muse is told to send a JPEG instead.
    p = params("{\"data_b64\":\"/9j/\",\"mime\":\"image/png\"}");
    expect_message(gadget_show_image_command(p), "unsupported", "PNG isn't supported: send a baseline JPEG");
    cJSON_Delete(p);
    p = params("{\"data_b64\":\"iVBORw0KGgo=\"}");
    expect_error(gadget_show_image_command(p), "unsupported");
    cJSON_Delete(p);
    p = params("{\"data_b64\":\"/9j/\",\"size\":600000}");
    expect_error(gadget_show_image_command(p), "too_large");
    cJSON_Delete(p);
    assert(s_presents == 0 && !s_push.buf);
}

static void test_show_image_chunks(void) {
    size_t len;
    uint8_t *img = jpeg(40000, false, &len);

    // In order, the last marked final: shown whole, with its label.
    cJSON *result = push(img, len, 16384, "\"label\":\"red panda\",\"mime\":\"image/jpeg\"");
    expect_ok(result);
    cJSON *payload = cJSON_GetObjectItem(result, "payload");
    assert(cJSON_GetObjectItem(payload, "received")->valuedouble == (double)len);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(payload, "complete")));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(payload, "shown")));
    cJSON_Delete(result);
    assert(s_presents == 1 && s_presented_len == len && !memcmp(s_presented, img, len));
    assert(!strcmp(s_presented_label, "red panda"));
    assert(!s_push.buf && !s_push.len);   // handed over, not kept

    // The same image again within two minutes isn't shown twice; after, it is.
    s_now_us += 60 * 1000000LL;
    result = push(img, len, 30000, NULL);
    payload = cJSON_GetObjectItem(result, "payload");
    assert(cJSON_IsTrue(cJSON_GetObjectItem(payload, "complete")));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(payload, "shown")));
    cJSON_Delete(result);
    assert(s_presents == 1 && !s_push.buf);
    s_now_us += 61 * 1000000LL;
    result = push(img, len, len, NULL);   // all in one chunk
    expect_ok(result);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(result, "payload"), "shown")));
    cJSON_Delete(result);
    assert(s_presents == 2 && !strcmp(s_presented_label, "image"));

    // A chunk at the wrong offset is refused, saying which comes next, and
    // what came before is kept: the right one carries on.
    img[100] ^= 0x55;   // another image
    expect_progress(chunk(img, 0, 10000, false, NULL), 10000, false);
    expect_message(chunk(img, 20000, 10000, false, NULL), "invalid_param", "expected offset 10000");
    expect_message(chunk(img, 5000, 10000, false, NULL), "invalid_param", "expected offset 10000");
    expect_progress(chunk(img, 10000, 10000, false, NULL), 20000, false);
    // Bad base64 mid-image is refused without losing the rest.
    cJSON *p = params("{\"data_b64\":\"!!!!\",\"offset\":20000}");
    expect_error(gadget_show_image_command(p), "invalid_param");
    cJSON_Delete(p);
    result = chunk(img, 20000, len - 20000, true, NULL);
    expect_ok(result);
    cJSON_Delete(result);
    assert(s_presents == 3 && s_presented_len == len && !memcmp(s_presented, img, len));

    // Offset 0 starts over, dropping a half-sent one.
    img[100] ^= 0x0F;
    expect_progress(chunk(img, 0, 10000, false, NULL), 10000, false);
    expect_progress(chunk(img, 0, 5000, false, NULL), 5000, false);
    result = chunk(img, 5000, len - 5000, true, NULL);
    expect_ok(result);
    cJSON_Delete(result);
    assert(s_presents == 4 && !memcmp(s_presented, img, len));

    // Base64 with line breaks and the URL-safe alphabet, as tools write it.
    img[100] ^= 0x33;
    char *data = b64(img, len);
    size_t n = strlen(data);
    char *wrapped = malloc(n + n / 76 * 2 + 1), *w = wrapped;
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        *w++ = c == '+' ? '-' : c == '/' ? '_' : c;
        if (i % 76 == 75) {
            *w++ = '\r';
            *w++ = '\n';
        }
    }
    *w = '\0';
    p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "data_b64", wrapped);
    cJSON_AddBoolToObject(p, "final", true);
    result = gadget_show_image_command(p);
    expect_ok(result);
    cJSON_Delete(result);
    cJSON_Delete(p);
    free(wrapped);
    free(data);
    assert(s_presents == 5 && s_presented_len == len && !memcmp(s_presented, img, len));

    // size sets the buffer's size up front: one allocation for the lot.
    img[100] ^= 0x44;
    char extra[32];
    snprintf(extra, sizeof(extra), "\"size\":%u", (unsigned)len);
    s_reallocs = 0;
    result = push(img, len, 8192, extra);
    expect_ok(result);
    cJSON_Delete(result);
    assert(s_reallocs == 1 && s_last_alloc == len && s_presents == 6);

    // Out of memory: refused, and it starts again.
    s_fail_alloc = true;
    expect_error(chunk(img, 0, 1000, false, NULL), "out_of_memory");
    s_fail_alloc = false;
    assert(!s_push.buf && !s_push.len);
    free(img);

    // Over 512 KB, however it's sent: refused, and it starts again.
    img = jpeg(MUSE_PRESENT_MAX, false, &len);
    expect_progress(chunk(img, 0, 400000, false, NULL), 400000, false);
    expect_error(chunk(img, 400000, len - 400000, true, NULL), "too_large");
    assert(!s_push.buf && !s_push.len);
    expect_message(chunk(img, 400000, 1000, false, NULL), "invalid_param", "expected offset 0");
    free(img);

    // Progressive JPEG, which the decoder can't take: refused at the end.
    img = jpeg(3000, true, &len);
    expect_error(push(img, len, 2000, NULL), "unsupported");
    assert(s_presents == 6 && !s_push.buf);
    free(img);
}

int main(void) {
    test_show_text();
    test_set_mode();
    test_set_chat();
    test_list_chats();
    test_show_image_params();
    test_show_image_chunks();
    printf("gadget commands ok\n");
    return 0;
}
