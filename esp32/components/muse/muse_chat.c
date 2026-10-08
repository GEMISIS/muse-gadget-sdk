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

#include "muse_chat_priv.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

#include "muse_link.h"
#include "muse_settings.h"
#include "muse_wifi.h"

/* Connection state as last reported by the session task (muse_chat_session.cpp). */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static muse_hatch_state_t s_result = MUSE_HATCH_UNTESTED;
static char s_detail[48];

void muse_hatch_report(muse_hatch_state_t state, const char *detail)
{
    portENTER_CRITICAL(&s_lock);
    s_result = state;
    strlcpy(s_detail, detail, sizeof(s_detail));
    portEXIT_CRITICAL(&s_lock);
}

bool muse_hatch_configured(void)
{
    return muse_settings_hatch_token_len() > 0 || muse_link_hatch_linked();
}

void muse_hatch_test(void)
{
    if (!muse_hatch_configured() || !muse_wifi_connected()) {
        return;
    }
    muse_hatch_report(MUSE_HATCH_TESTING, "Connecting...");
    muse_hatch_chat_connect();
}

void muse_hatch_config_changed(void)
{
    muse_hatch_report(MUSE_HATCH_UNTESTED, "");
    muse_hatch_chat_forget();
}

void muse_hatch_status(muse_hatch_status_t *out)
{
    if (!muse_hatch_configured()) {
        out->state = MUSE_HATCH_NOT_SET;
        strlcpy(out->detail, "Pair in the Muse app", sizeof(out->detail));
        return;
    }
    portENTER_CRITICAL(&s_lock);
    out->state = s_result;
    strlcpy(out->detail, s_detail, sizeof(out->detail));
    portEXIT_CRITICAL(&s_lock);
    if (!muse_wifi_connected() && out->state != MUSE_HATCH_TESTING) {
        out->state = MUSE_HATCH_OFFLINE;
        strlcpy(out->detail, "Waiting for Wi-Fi", sizeof(out->detail));
    } else if (out->state == MUSE_HATCH_UNTESTED && !out->detail[0]) {
        strlcpy(out->detail, "Connects when you talk", sizeof(out->detail));
    }
}

const char *muse_hatch_state_name(muse_hatch_state_t state)
{
    switch (state) {
    case MUSE_HATCH_NOT_SET: return "Not set up";
    case MUSE_HATCH_OFFLINE: return "Offline";
    case MUSE_HATCH_UNTESTED: return "Saved";
    case MUSE_HATCH_TESTING: return "Connecting";
    case MUSE_HATCH_REACHABLE: return "Connected";
    case MUSE_HATCH_UNREACHABLE: return "Can't connect";
    }
    return "";
}

/* ---- Background requests, by asker ---- */

static portMUX_TYPE s_bg_lock = portMUX_INITIALIZER_UNLOCKED;
static muse_chat_bg_for_t s_bg_for;   /* whose the request is, until its result is taken (with s_bg_lock) */

static void bg_release(void)
{
    portENTER_CRITICAL(&s_bg_lock);
    s_bg_for = MUSE_CHAT_BG_FOR_NONE;
    portEXIT_CRITICAL(&s_bg_lock);
}

bool muse_chat_bg_ask_for(muse_chat_bg_for_t who, const char *sid, const char *message)
{
    if (who == MUSE_CHAT_BG_FOR_NONE) {
        return false;
    }
    portENTER_CRITICAL(&s_bg_lock);
    bool free = s_bg_for == MUSE_CHAT_BG_FOR_NONE;
    if (free) {
        s_bg_for = who;
    }
    portEXIT_CRITICAL(&s_bg_lock);
    if (!free) {
        return false;
    }
    if (!(who == MUSE_CHAT_BG_FOR_IMAGE ? muse_chat_bg_ask_now : muse_chat_bg_ask)(sid, message)) {
        bg_release();
        return false;
    }
    return true;
}

void muse_chat_bg_abandon(muse_chat_bg_for_t who)
{
    portENTER_CRITICAL(&s_bg_lock);
    bool mine = who != MUSE_CHAT_BG_FOR_NONE && s_bg_for == who;
    portEXIT_CRITICAL(&s_bg_lock);
    if (mine) {
        muse_chat_bg_forget();   /* the next request replaces it, whatever it's doing */
        bg_release();
    }
}

muse_chat_bg_state_t muse_chat_bg_result_for(muse_chat_bg_for_t who, char *out, size_t cap)
{
    portENTER_CRITICAL(&s_bg_lock);
    bool mine = who != MUSE_CHAT_BG_FOR_NONE && s_bg_for == who;
    portEXIT_CRITICAL(&s_bg_lock);
    if (!mine) {
        return MUSE_CHAT_BG_NONE;
    }
    muse_chat_bg_state_t st = muse_chat_bg_result(out, cap);
    if (st == MUSE_CHAT_BG_NONE) {
        st = MUSE_CHAT_BG_FAILED;   /* asked, yet nothing's under way: it's gone */
    }
    if (st != MUSE_CHAT_BG_BUSY) {
        bg_release();
    }
    return st;
}
