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

// Host harness for show_text and set_mode: params in, command result and
// calls into the Muse UI out. The runner extracts the production command code
// from main/gadget_commands.c into gadget_commands.inc.
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

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
} muse_chat_entry_t;

static char s_chat_sid[MUSE_CHAT_SID_MAX + 1];
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
    strcpy(c->name, name);
    snprintf(c->sid, sizeof(c->sid), "00000000-0000-4000-8000-%012d", s_chats_n);
    strcpy(sid_out, c->sid);
    s_chat_adds++;
    return ESP_OK;
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
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(payload, "chats")) == 0);
    cJSON_Delete(result);

    cJSON_Delete(expect_chat("{\"name\":\"Work\"}", "named", "00000000-0000-4000-8000-000000000001"));
    cJSON_Delete(expect_chat("{\"name\":\"Home\"}", "named", "00000000-0000-4000-8000-000000000002"));
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
    cJSON_Delete(result);
}

int main(void) {
    test_show_text();
    test_set_mode();
    test_set_chat();
    test_list_chats();
    printf("gadget commands ok\n");
    return 0;
}
