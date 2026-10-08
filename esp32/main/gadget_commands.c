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

#include "gadget_commands.h"

#include <stdbool.h>
#include <string.h>

#include "muse_gadget_mode.h"
#include "muse_settings.h"
#include "muse_state.h"

// ---- Commands (host-tested) -------------------------------------------------

// Longest text show_text takes: what the caption holds.
#define SHOW_TEXT_MAX MUSE_CAPTION_MAX

static cJSON *gadget_error(const char *code, const char *message) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(result, "error", error);
    return result;
}

static cJSON *gadget_ok(cJSON *payload) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    if (payload) {
        cJSON_AddItemToObject(result, "payload", payload);
    }
    return result;
}

cJSON *gadget_show_text_command(const cJSON *params) {
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(params, "text");
    if (!cJSON_IsString(text) || !text->valuestring) {
        return gadget_error("missing_param", "text is required");
    }
    size_t len = strlen(text->valuestring);
    if (len == 0) {
        return gadget_error("invalid_param", "text is empty");
    }
    if (len > SHOW_TEXT_MAX) {
        return gadget_error("invalid_param", "text is longer than 400 bytes");
    }
    muse_state_set_caption("%s", text->valuestring);
    muse_state_set_asleep(false);
    muse_state_poke();   // and keep it awake a while to be read
    return gadget_ok(NULL);
}

cJSON *gadget_set_mode_command(const cJSON *params) {
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(params, "mode");
    if (!cJSON_IsString(mode) || !mode->valuestring) {
        return gadget_error("missing_param", "mode is required");
    }
    muse_gadget_mode_t m;
    if (!muse_gadget_mode_parse(mode->valuestring, &m)) {
        return gadget_error("invalid_param", "mode must be desk, night or on_the_go");
    }
    muse_gadget_mode_pick(m);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "mode", mode->valuestring);
    return gadget_ok(payload);
}

// A chat's session_id: 1 to MUSE_CHAT_SID_MAX letters, digits and dashes.
static bool chat_sid_valid(const char *sid) {
    size_t n = 0;
    for (; sid[n]; n++) {
        char c = sid[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return n >= 1 && n <= MUSE_CHAT_SID_MAX;
}

cJSON *gadget_set_chat_command(const cJSON *params) {
    const cJSON *sid = cJSON_GetObjectItemCaseSensitive(params, "session_id");
    const char *want = "";
    if (sid && !cJSON_IsNull(sid)) {
        if (!cJSON_IsString(sid) || !sid->valuestring) {
            return gadget_error("invalid_param", "session_id must be a string");
        }
        want = sid->valuestring;
    }
    char gadget[MUSE_CHAT_SID_MAX + 1];
    muse_settings_gadget_chat_sid(gadget);
    const char *chat = "custom";
    if (!want[0] || !strcmp(want, "main")) {
        want = "";
        chat = "main";
    } else if (!strcmp(want, "gadget") || !strcmp(want, gadget)) {
        want = gadget;
        chat = "gadget";
    } else if (!chat_sid_valid(want)) {
        return gadget_error("invalid_param",
                            "session_id must be 1 to 64 letters, digits and dashes");
    }
    if (!muse_settings_set_chat_sid(want)) {
        return gadget_error("invalid_param", "session_id was refused");
    }
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "chat", chat);
    cJSON_AddStringToObject(payload, "session_id", want);
    return gadget_ok(payload);
}
