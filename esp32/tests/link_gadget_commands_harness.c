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

int main(void) {
    test_show_text();
    test_set_mode();
    printf("gadget commands ok\n");
    return 0;
}
