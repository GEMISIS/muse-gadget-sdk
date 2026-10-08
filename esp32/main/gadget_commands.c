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

// A chat's session_id: a UUID, 8-4-4-4-12 hex digits in either case (as
// muse_settings_chat_sid_valid; Muse refuses anything else with HTTP 400).
static bool chat_sid_valid(const char *sid) {
    if (strlen(sid) != MUSE_CHAT_SID_MAX) {
        return false;
    }
    for (int i = 0; i < MUSE_CHAT_SID_MAX; i++) {
        char c = sid[i];
        bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F');
        if (dash ? c != '-' : !hex) {
            return false;
        }
    }
    return true;
}

// {chat, session_id, name?} for a chat's id: "" the main chat, the gadget's
// own, one of the named chats kept on the device, or any other.
static cJSON *chat_json(const char *sid) {
    char gadget[MUSE_CHAT_SID_MAX + 1], name[MUSE_CHAT_NAME_MAX + 1];
    muse_settings_gadget_chat_sid(gadget);
    bool named = sid[0] && muse_settings_chat_name(sid, name);
    cJSON *chat = cJSON_CreateObject();
    cJSON_AddStringToObject(chat, "chat", !sid[0]                 ? "main"
                                          : !strcmp(sid, gadget) ? "gadget"
                                          : named                ? "named"
                                          : muse_settings_chat_untitled(sid) ? "new"
                                                                 : "custom");
    cJSON_AddStringToObject(chat, "session_id", sid);
    if (named) {
        cJSON_AddStringToObject(chat, "name", name);
    }
    return chat;
}

// The param `key`: false (and *error set) if it's there but not a string;
// *out is NULL when it's missing or null.
static bool string_param(const cJSON *params, const char *key, const char **out,
                         cJSON **error) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(params, key);
    *out = NULL;
    if (!item || cJSON_IsNull(item)) {
        return true;
    }
    if (!cJSON_IsString(item) || !item->valuestring) {
        *error = gadget_error("invalid_param", !strcmp(key, "name")
                                                   ? "name must be a string"
                                                   : "session_id must be a string");
        return false;
    }
    *out = item->valuestring;
    return true;
}

cJSON *gadget_set_chat_command(const cJSON *params) {
    const char *sid, *name;
    cJSON *error = NULL;
    if (!string_param(params, "session_id", &sid, &error) ||
        !string_param(params, "name", &name, &error)) {
        return error;
    }
    if (sid && name) {
        return gadget_error("invalid_param", "give session_id or name, not both");
    }
    char want[MUSE_CHAT_SID_MAX + 1] = "";
    bool created = false;
    if (name) {
        if (!muse_settings_chat_find(name, want)) {
            esp_err_t err = muse_settings_chat_add(name, want);
            if (err == ESP_ERR_NO_MEM) {
                return gadget_error("limit_reached",
                                    "8 named chats are kept already; forget one on the device");
            }
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                return gadget_error("invalid_param",
                                    "name must be 1 to 32 bytes, without control characters");
            }
            created = err == ESP_OK;
        }
    } else if (sid && sid[0] && strcmp(sid, "main")) {
        if (!strcmp(sid, "gadget")) {
            muse_settings_gadget_chat_sid(want);
        } else if (!chat_sid_valid(sid)) {
            return gadget_error("invalid_param",
                                "session_id must be main, gadget or a UUID "
                                "(8-4-4-4-12 hex digits)");
        } else {
            for (int i = 0; sid[i]; i++) {   // kept, and so compared, in lower case
                want[i] = sid[i] >= 'A' && sid[i] <= 'F' ? sid[i] + ('a' - 'A') : sid[i];
            }
            want[MUSE_CHAT_SID_MAX] = '\0';
        }
    }
    if (!muse_settings_set_chat_sid(want)) {
        return gadget_error("invalid_param", "session_id was refused");
    }
    cJSON *payload = chat_json(want);
    if (name) {
        cJSON_AddBoolToObject(payload, "created", created);
    }
    return gadget_ok(payload);
}

cJSON *gadget_list_chats_command(const cJSON *params) {
    (void)params;
    char sid[MUSE_CHAT_SID_MAX + 1];
    muse_settings_chat_sid(sid);
    muse_chat_entry_t chats[MUSE_CHATS_MAX];
    int n = muse_settings_chats(chats, MUSE_CHATS_MAX);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddItemToObject(payload, "current", chat_json(sid));
    cJSON *list = cJSON_AddArrayToObject(payload, "chats");
    for (int i = 0; i < n; i++) {
        cJSON *chat = cJSON_CreateObject();
        cJSON_AddStringToObject(chat, "name", chats[i].name);
        cJSON_AddStringToObject(chat, "session_id", chats[i].sid);
        cJSON_AddItemToArray(list, chat);
    }
    return gadget_ok(payload);
}
