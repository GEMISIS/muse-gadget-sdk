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
