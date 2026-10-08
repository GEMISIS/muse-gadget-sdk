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
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "muse_gadget_mode.h"
#include "muse_settings.h"
#include "muse_state.h"
#if CONFIG_MUSE_HATCH
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "muse_present.h"
#endif

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

// told_mode: the gadget mode a chat last heard ("desk", "night" or
// "on_the_go"), or null if it hasn't heard one.
static void add_told_mode(cJSON *chat, int mode) {
    const char *key = mode >= 0 ? muse_gadget_mode_key((muse_gadget_mode_t)mode) : NULL;
    if (key) {
        cJSON_AddStringToObject(chat, "told_mode", key);
    } else {
        cJSON_AddNullToObject(chat, "told_mode");
    }
}

// {chat, session_id, name?, told_mode} for a chat's id: "" the main chat, the
// gadget's own, one of the named chats kept on the device, or any other.
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
    add_told_mode(chat, muse_settings_chat_told(sid));
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
        add_told_mode(chat, chats[i].told_mode);
        cJSON_AddItemToArray(list, chat);
    }
    return gadget_ok(payload);
}

#if CONFIG_MUSE_HATCH
// ---- display.show_image ------------------------------------------------------

#define SHOW_IMAGE_LABEL_MAX 47
// The same image again this soon isn't shown again: Muse asked to push one
// (muse_present_ask) may push it twice, or after showing it some other way.
#define SHOW_IMAGE_REPEAT_US (120 * 1000000LL)

// The image being pushed. Only the Noise session's task runs commands, so it
// needs no lock. It and its bytes are in PSRAM: internal RAM is short.
EXT_RAM_BSS_ATTR static struct {
    uint8_t *buf;
    size_t len, cap;
    char label[SHOW_IMAGE_LABEL_MAX + 1];
    bool shown;              // the last image shown, and when
    uint32_t shown_hash;
    int64_t shown_us;
} s_push;

static void push_reset(void) {
    heap_caps_free(s_push.buf);
    s_push.buf = NULL;
    s_push.len = s_push.cap = 0;
}

// A base64 character's value: 0-63, -2 for padding, -3 for whitespace, -1
// for anything else. The URL-safe alphabet's - and _ count too.
static int b64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    if (c == '=') return -2;
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') return -3;
    return -1;
}

// How many bytes `in` decodes to, or -1 if it isn't base64: padding only at
// the end, and no lone last character.
static long b64_decoded_len(const char *in) {
    size_t digits = 0, pad = 0;
    for (const unsigned char *c = (const unsigned char *)in; *c; c++) {
        int v = b64_value(*c);
        if (v == -1 || (v >= 0 && pad)) {
            return -1;
        }
        pad += v == -2;
        digits += v >= 0;
    }
    if (pad > 2 || digits % 4 == 1) {
        return -1;
    }
    return (long)(digits / 4 * 3 + (digits % 4 ? digits % 4 - 1 : 0));
}

// Decodes `in`, which b64_decoded_len has passed, into out.
static void b64_decode(const char *in, uint8_t *out) {
    uint32_t acc = 0;
    int bits = 0;
    for (const unsigned char *c = (const unsigned char *)in; *c; c++) {
        int v = b64_value(*c);
        if (v < 0) {
            continue;
        }
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            *out++ = (uint8_t)(acc >> bits);
        }
    }
}

// Whether a JPEG's frame is progressive (or another kind the ROM decoder
// can't take), from its markers up to the scan.
static bool jpeg_progressive(const uint8_t *d, size_t len) {
    size_t i = 2;
    while (i + 4 <= len && d[i] == 0xFF) {
        uint8_t m = d[i + 1];
        if (m == 0xFF) {
            i++;   // fill
        } else if (m == 0xDA || m == 0xD9) {
            return false;   // the scan, after a baseline frame
        } else if (m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE) {
            return true;
        } else {
            i += 2 + ((size_t)d[i + 2] << 8 | d[i + 3]);
        }
    }
    return false;
}

// Whether a JPEG holds together as the decoder will walk it: header segments
// that chain to the scan, a baseline frame header among them, and the end
// marker near the end. Muse writes the base64 out itself, and now and then a
// mistyped character spoils an image the ROM decoder then can't open.
static bool jpeg_intact(const uint8_t *d, size_t len) {
    bool frame = false;
    size_t i = 2;
    for (;;) {
        if (i + 4 > len || d[i] != 0xFF) {
            return false;
        }
        uint8_t m = d[i + 1];
        if (m == 0xFF) {
            i++;   // fill
            continue;
        }
        if (m == 0xDA) {
            break;   // the scan
        }
        if (m == 0xC0 || m == 0xC1) {
            frame = true;
        }
        size_t seg = (size_t)d[i + 2] << 8 | d[i + 3];
        if (seg < 2 || i + 2 + seg > len) {
            return false;
        }
        i += 2 + seg;
    }
    if (!frame) {
        return false;
    }
    for (size_t k = len; k >= 2 && len - k < 64; k--) {   // trailing bytes after it are allowed
        if (d[k - 2] == 0xFF && d[k - 1] == 0xD9) {
            return true;
        }
    }
    return false;
}

static uint32_t fnv1a(const uint8_t *d, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ d[i]) * 16777619u;
    }
    return h;
}

static cJSON *push_progress(bool complete, bool shown) {
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "received", (double)s_push.len);
    cJSON_AddBoolToObject(payload, "complete", complete);
    if (complete) {
        cJSON_AddBoolToObject(payload, "shown", shown);
    }
    return gadget_ok(payload);
}

// A failure that drops what has come so far: the image starts again at 0.
static cJSON *push_error(const char *code, const char *message) {
    push_reset();
    return gadget_error(code, message);
}

cJSON *gadget_show_image_command(const cJSON *params) {
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(params, "data_b64");
    const cJSON *offset_j = cJSON_GetObjectItemCaseSensitive(params, "offset");
    const cJSON *final_j = cJSON_GetObjectItemCaseSensitive(params, "final");
    const cJSON *label = cJSON_GetObjectItemCaseSensitive(params, "label");
    const cJSON *mime = cJSON_GetObjectItemCaseSensitive(params, "mime");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(params, "size");
    if (!cJSON_IsString(data) || !data->valuestring) {
        return gadget_error("missing_param", "data_b64 is required");
    }
    size_t offset = 0;
    if (offset_j && !cJSON_IsNull(offset_j)) {
        double v = cJSON_IsNumber(offset_j) ? offset_j->valuedouble : -1;
        if (v < 0 || v > MUSE_PRESENT_MAX || v != (double)(size_t)v) {
            return gadget_error("invalid_param", "offset must be a whole number of bytes");
        }
        offset = (size_t)v;
    }
    if (final_j && !cJSON_IsNull(final_j) && !cJSON_IsBool(final_j)) {
        return gadget_error("invalid_param", "final must be true or false");
    }
    bool final = cJSON_IsTrue(final_j);
    if (label && !cJSON_IsNull(label) && !cJSON_IsString(label)) {
        return gadget_error("invalid_param", "label must be a string");
    }
    if (mime && !cJSON_IsNull(mime)) {
        const char *m = cJSON_GetStringValue(mime);
        if (m && !strcmp(m, "image/png")) {
            return push_error("unsupported", "PNG isn't supported: send a baseline JPEG");
        }
        if (!m || (strcmp(m, "image/jpeg") && strcmp(m, "image/jpg"))) {
            return gadget_error("invalid_param", "mime must be image/jpeg");
        }
    }
    size_t size_hint = 0;
    if (size && !cJSON_IsNull(size)) {
        if (!cJSON_IsNumber(size) || size->valuedouble < 1) {
            return gadget_error("invalid_param", "size must be the image's length in bytes");
        }
        if (size->valuedouble > MUSE_PRESENT_MAX) {
            return push_error("too_large", "images over 512 KB aren't shown: send a smaller JPEG");
        }
        size_hint = (size_t)size->valuedouble;
    }
    long n = b64_decoded_len(data->valuestring);
    if (n < 0) {
        return gadget_error("invalid_param", "data_b64 isn't valid base64");
    }
    if (offset == 0) {
        push_reset();   // a new image, or this one from the start again
    } else if (offset != s_push.len) {
        char message[48];
        snprintf(message, sizeof(message), "expected offset %u", (unsigned)s_push.len);
        return gadget_error("invalid_param", message);
    }
    if (n == 0 && !final) {
        return gadget_error("invalid_param", "data_b64 is empty");
    }
    size_t want = offset + (size_t)n;
    if (want > MUSE_PRESENT_MAX) {
        return push_error("too_large", "images over 512 KB aren't shown: send a smaller JPEG");
    }
    if (want > s_push.cap) {
        size_t cap = s_push.cap * 2 > want ? s_push.cap * 2 : want;
        cap = size_hint > cap ? size_hint : cap;
        cap = cap < MUSE_PRESENT_MAX ? cap : MUSE_PRESENT_MAX;
        uint8_t *grown = heap_caps_realloc(s_push.buf, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!grown) {
            return push_error("out_of_memory", "no room for the image");
        }
        s_push.buf = grown;
        s_push.cap = cap;
    }
    b64_decode(data->valuestring, s_push.buf + s_push.len);
    if (s_push.len < 2 && want >= 2 && (s_push.buf[0] != 0xFF || s_push.buf[1] != 0xD8)) {
        bool png = want >= 4 && !memcmp(s_push.buf, "\x89PNG", 4);
        return push_error(png ? "unsupported" : "invalid_param",
                          png ? "PNG isn't supported: send a baseline JPEG" : "not a JPEG");
    }
    s_push.len = want;
    const char *l = cJSON_GetStringValue(label);
    if (offset == 0 || (l && l[0])) {
        strlcpy(s_push.label, l && l[0] ? l : "image", sizeof(s_push.label));
    }
    if (!final) {
        return push_progress(false, false);
    }
    if (s_push.len < 2) {
        return push_error("invalid_param", "not a JPEG");
    }
    if (jpeg_progressive(s_push.buf, s_push.len)) {
        return push_error("unsupported", "progressive JPEG isn't supported: send a baseline one");
    }
    if (!jpeg_intact(s_push.buf, s_push.len)) {
        return push_error("invalid_param", "the JPEG arrived damaged: send it again from offset 0");
    }
    uint32_t hash = fnv1a(s_push.buf, s_push.len);
    int64_t now = esp_timer_get_time();
    if (s_push.shown && hash == s_push.shown_hash && now - s_push.shown_us < SHOW_IMAGE_REPEAT_US) {
        cJSON *result = push_progress(true, false);   // it's on the screen already
        push_reset();
        return result;
    }
    s_push.shown = true;
    s_push.shown_hash = hash;
    s_push.shown_us = now;
    cJSON *result = push_progress(true, true);
    muse_present_bytes(s_push.buf, s_push.len, s_push.label);   // it takes the buffer
    s_push.buf = NULL;
    s_push.len = s_push.cap = 0;
    return result;
}
#endif
