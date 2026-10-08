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
 * Deleting a chat from Muse (muse_chat_delete.h): the waiting requests, the
 * asking, and what came of it for the Chats screen.
 */
#include "muse_chat_delete.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "muse_chat.h"
#include "muse_settings.h"
#include "muse_text.h"

static const char *TAG = "chat_delete";

#define WAITING_MAX 4
#define STATUS_MAX 96
#define REASON_MAX 48   /* of Muse's reply, when it says why not */

typedef struct {
    char sid[MUSE_CHAT_SID_MAX + 1];
    char title[MUSE_CHAT_NAME_MAX + 1];
} request_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static request_t s_waiting[WAITING_MAX];   /* oldest first (with s_lock) */
static int s_waiting_n;
static char s_status[STATUS_MAX];          /* with s_lock */
static uint32_t s_status_gen;

/* The tick's task only. */
static bool s_asking;
static char s_asked_sid[MUSE_CHAT_SID_MAX + 1];

static void set_status(const char *text)
{
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_status, text, sizeof(s_status));
    s_status_gen++;
    portEXIT_CRITICAL(&s_lock);
}

bool muse_chat_delete(const char *sid, const char *title)
{
    if (!muse_settings_chat_sid_valid(sid)) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    bool room = s_waiting_n < WAITING_MAX;
    if (room) {
        request_t *r = &s_waiting[s_waiting_n++];
        strlcpy(r->sid, sid, sizeof(r->sid));
        strlcpy(r->title, title ? title : "", sizeof(r->title));
    }
    portEXIT_CRITICAL(&s_lock);
    set_status(room ? "Deleting from Muse..." : "Couldn't delete from Muse: too many waiting.");
    return room;
}

uint32_t muse_chat_delete_status(char *out, size_t cap)
{
    portENTER_CRITICAL(&s_lock);
    if (cap) {
        strlcpy(out, s_status, cap);
    }
    uint32_t gen = s_status_gen;
    portEXIT_CRITICAL(&s_lock);
    return gen;
}

/* "done", maybe in bold or quotes, with a full stop: Muse deleted it. */
static bool said_done(const char *reply)
{
    while (*reply && strchr(" \t\r\n*_\"'`", *reply)) {
        reply++;
    }
    return strncasecmp(reply, "done", 4) == 0 && !isalnum((unsigned char)reply[4]);
}

/* Why not, from the reply: its first line with text, short, in the fonts' characters. */
static void reason(const char *reply, char *out, size_t cap)
{
    while (*reply && strchr(" \t\r\n*_\"'`-", *reply)) {
        reply++;
    }
    size_t n = strcspn(reply, "\r\n");
    n = n < cap - 1 ? n : cap - 1;
    memcpy(out, reply, n);
    out[n] = '\0';
    muse_text_to_ascii(out, cap);
    while (n && strchr(" \t*_\"'`.", out[n - 1])) {
        out[--n] = '\0';
    }
    if (!out[0]) {
        strlcpy(out, "no answer", cap);
    }
}

static void take_reply(void)
{
    char reply[256];
    muse_chat_bg_state_t st = muse_chat_bg_result_for(MUSE_CHAT_BG_FOR_DELETE, reply, sizeof(reply));
    if (st == MUSE_CHAT_BG_BUSY) {
        return;
    }
    s_asking = false;
    if (st == MUSE_CHAT_BG_DONE && said_done(reply)) {
        ESP_LOGI(TAG, "Muse deleted chat %s", s_asked_sid);
        set_status("Deleted from Muse.");
        return;
    }
    char why[REASON_MAX], line[STATUS_MAX];
    if (st == MUSE_CHAT_BG_DONE) {
        reason(reply, why, sizeof(why));
    } else {
        strlcpy(why, "no answer", sizeof(why));
    }
    ESP_LOGW(TAG, "Muse didn't delete chat %s: %s", s_asked_sid, why);
    snprintf(line, sizeof(line), "Couldn't delete from Muse: %s.", why);
    set_status(line);
}

void muse_chat_delete_tick(void)
{
    if (s_asking) {
        take_reply();
        return;
    }
    if (!muse_hatch_ready()) {
        return;   /* out of reach: they wait */
    }
    request_t r;
    portENTER_CRITICAL(&s_lock);
    bool any = s_waiting_n > 0;
    if (any) {
        r = s_waiting[0];
    }
    portEXIT_CRITICAL(&s_lock);
    if (!any) {
        return;
    }
    /* Quotes in the title would end it early. */
    for (char *c = r.title; *c; c++) {
        if (*c == '"') {
            *c = '\'';
        }
    }
    char msg[200], gadget[MUSE_CHAT_SID_MAX + 1];
    if (r.title[0]) {
        snprintf(msg, sizeof(msg),
                 "Please delete the chat with session id %s (titled \"%s\"). Reply with just: done, or why not.",
                 r.sid, r.title);
    } else {
        snprintf(msg, sizeof(msg), "Please delete the chat with session id %s. Reply with just: done, or why not.",
                 r.sid);
    }
    muse_settings_gadget_chat_sid(gadget);
    if (!muse_chat_bg_ask_for(MUSE_CHAT_BG_FOR_DELETE, gadget, msg)) {
        return;   /* someone else's request is under way: next tick */
    }
    s_asking = true;
    strlcpy(s_asked_sid, r.sid, sizeof(s_asked_sid));
    portENTER_CRITICAL(&s_lock);
    s_waiting_n--;
    memmove(&s_waiting[0], &s_waiting[1], s_waiting_n * sizeof(s_waiting[0]));
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "asking Muse to delete chat %s", r.sid);
}
