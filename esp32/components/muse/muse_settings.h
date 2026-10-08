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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * User settings, persisted in NVS. Setters save immediately and then notify the
 * change listener (from the caller's task), which applies the setting to the
 * hardware. Brightness, auto-sleep and the speaker are polled where they're used.
 */

#define MUSE_SSID_MAX 32
#define MUSE_PASS_MAX 64
#define MUSE_HOST_MAX 63
#define MUSE_VM_MAX 63
#define MUSE_TOKEN_MAX 1023
#define MUSE_CHAT_SID_MAX 36      /* a chat's session_id: a UUID, 8-4-4-4-12 hex digits */
#define MUSE_CHAT_NAME_MAX 32     /* a named chat's name, in bytes */
#define MUSE_CHATS_MAX 8          /* named chats kept on the device */

#define MUSE_MIC_GAIN_MAX 36      /* dB; ES7210 PGA, applied in 3 dB steps */

typedef enum {
    MUSE_SETTING_VOLUME,
    MUSE_SETTING_SPEAKER,
    MUSE_SETTING_MIC_GAIN,
    MUSE_SETTING_BRIGHTNESS,
    MUSE_SETTING_SLEEP,
    MUSE_SETTING_WIFI,          /* on/off or credentials */
    MUSE_SETTING_BLE,
    MUSE_SETTING_HATCH,
    MUSE_SETTING_GADGET_MODE,   /* the mode, its manual override or the home network */
    MUSE_SETTING_CHAT,          /* which Muse chat turns go to */
} muse_setting_t;

typedef void (*muse_setting_cb_t)(muse_setting_t what);

esp_err_t muse_settings_init(void);
void muse_settings_set_listener(muse_setting_cb_t cb);

int muse_settings_volume(void);         /* 0..100 */
bool muse_settings_speaker_on(void);    /* off: replies are shown, not played */
int muse_settings_mic_gain(void);       /* dB, 0..MUSE_MIC_GAIN_MAX */
int muse_settings_brightness(void);     /* 10..100 */
int muse_settings_sleep_s(void);        /* 0 = never */
bool muse_settings_wifi_on(void);
bool muse_settings_ble_on(void);
/* Gadget mode (muse_gadget_mode.h), 0..2, and whether it was picked by hand:
 * then *until is when the schedule takes over again (epoch seconds, 0 if the
 * clock wasn't set when it was picked). */
int muse_settings_gadget_mode(void);
bool muse_settings_mode_override(uint32_t *until);
/* The network that counts as home; empty if none is set. */
void muse_settings_home_ssid(char out[MUSE_SSID_MAX + 1]);
/* The network that means On-the-go (a phone hotspot); empty if none is set. */
void muse_settings_away_ssid(char out[MUSE_SSID_MAX + 1]);

void muse_settings_wifi(char ssid[MUSE_SSID_MAX + 1], char pass[MUSE_PASS_MAX + 1]);
void muse_settings_hatch_host(char out[MUSE_HOST_MAX + 1]);
void muse_settings_hatch_vm(char out[MUSE_VM_MAX + 1]);
void muse_settings_hatch_token(char out[MUSE_TOKEN_MAX + 1]);
size_t muse_settings_hatch_token_len(void);
/*
 * The Muse chat turns go to, as the session_id of POST /chat/stream: empty for
 * the main chat. An id the Muse hasn't seen starts a new side chat; Muse only
 * takes UUIDs, and has no way to list the side chats, so the named ones are
 * kept here (muse_settings_chats). The pick is kept in RAM only: every boot
 * starts in the main chat. The list is saved.
 *
 * All of these are thread-safe and quick (the LVGL task may call them); the
 * ones that change the pick notify MUSE_SETTING_CHAT from the caller's task.
 */
void muse_settings_chat_sid(char out[MUSE_CHAT_SID_MAX + 1]);
/* This gadget's own side chat: a UUID made from the Wi-Fi MAC, the same one
 * every time. */
void muse_settings_gadget_chat_sid(char out[MUSE_CHAT_SID_MAX + 1]);
/* A UUID: 8-4-4-4-12 hex digits, either case. */
bool muse_settings_chat_sid_valid(const char *sid);

/* A side chat kept on the device under a name of the user's. */
typedef struct {
    char name[MUSE_CHAT_NAME_MAX + 1];
    char sid[MUSE_CHAT_SID_MAX + 1];
    int8_t told_mode;   /* the gadget mode it last heard (muse_gadget_mode_t), -1 for none */
} muse_chat_entry_t;

/* The named chats, oldest first; returns how many (up to max). */
int muse_settings_chats(muse_chat_entry_t *out, int max);
/* The named chat called `name` (ignoring case and the spaces around it):
 * true and its id in sid_out (may be NULL). */
bool muse_settings_chat_find(const char *name, char sid_out[MUSE_CHAT_SID_MAX + 1]);
/* The name of the named chat with this id: false if it isn't one. */
bool muse_settings_chat_name(const char *sid, char name_out[MUSE_CHAT_NAME_MAX + 1]);
/*
 * Keeps a new named chat with a fresh random UUID (v4), and returns it in
 * sid_out; it isn't picked. ESP_ERR_INVALID_ARG for an empty name or one over
 * MUSE_CHAT_NAME_MAX bytes, ESP_ERR_INVALID_STATE if a chat has that name
 * already (sid_out is then that one's), ESP_ERR_NO_MEM with MUSE_CHATS_MAX kept.
 */
esp_err_t muse_settings_chat_add(const char *name, char sid_out[MUSE_CHAT_SID_MAX + 1]);
/* Forgets a named chat (Muse keeps it); if it's the one picked, the main chat
 * is picked instead. False if no named chat has this id. */
bool muse_settings_chat_forget(const char *sid);
/* muse_settings_chat_add, then picks the chat: the new one, or with
 * ESP_ERR_INVALID_STATE the one that already has the name. */
esp_err_t muse_settings_chat_new(const char *name, char sid_out[MUSE_CHAT_SID_MAX + 1]);
/*
 * Picks a new chat with nothing typed for it: a fresh id, kept in RAM only.
 * It starts on the Muse with the first message, which the Muse titles it by;
 * only then does it join the named chats, under that title
 * (muse_settings_chat_retitle). Picking another chat first drops it.
 * ESP_ERR_NO_MEM with MUSE_CHATS_MAX kept already.
 */
esp_err_t muse_settings_chat_pick_new(void);
/* The new chat picked by muse_settings_chat_pick_new(), not yet titled. */
bool muse_settings_chat_untitled(const char *sid);
/* The Muse's title for a chat: a named one's new name, or the new chat's,
 * which then joins the named chats (*started set). Kept in RAM at once, so
 * any task may call it; muse_settings_chats_flush() writes it to flash.
 * False if sid is neither. */
bool muse_settings_chat_retitle(const char *sid, const char *title, bool *started);
/* Writes retitled chats, and the modes chats were told, to flash; from a task
 * whose stack is internal RAM. */
void muse_settings_chats_flush(void);
/*
 * The gadget mode (muse_gadget_mode_t) a chat last heard, -1 if none: each
 * chat is told the mode once, in the first message it gets after a change
 * (muse_gadget_mode_context). Kept for the main chat, the gadget's and the
 * named ones (a new chat's carries over once it's titled); a chat picked by
 * its id alone keeps it in RAM only. Setting it is quick and safe from any
 * task; muse_settings_chats_flush() writes it to flash.
 */
int muse_settings_chat_told(const char *sid);
void muse_settings_chat_set_told(const char *sid, int mode);

/* Every chat to pick from, for a chat picker. */
typedef enum {
    MUSE_CHAT_MAIN,     /* sid "" */
    MUSE_CHAT_GADGET,   /* muse_settings_gadget_chat_sid */
    MUSE_CHAT_NAMED,    /* one of muse_settings_chats */
    MUSE_CHAT_CUSTOM,   /* picked by its id alone (set_chat, the console); listed only while picked */
    MUSE_CHAT_NEW,      /* "New chat": always listed last; sid set while it's picked (muse_settings_chat_pick_new) */
} muse_chat_kind_t;

typedef struct {
    muse_chat_kind_t kind;
    char name[MUSE_CHAT_NAME_MAX + 1];   /* "Main chat", "Gadget chat", the chat's own, or "Other chat" */
    char sid[MUSE_CHAT_SID_MAX + 1];
    int8_t told_mode;                    /* muse_settings_chat_told; -1 for none, or "New chat" unpicked */
} muse_chat_item_t;

/* Main, Gadget, the named chats oldest first, and a custom one while picked. */
#define MUSE_CHAT_ITEMS_MAX (MUSE_CHATS_MAX + 4)   /* main, gadget, custom and New chat too */
/*
 * Fills out with up to max items in that order and returns how many; *current
 * (may be NULL) is the index of the one picked, -1 if it didn't fit.
 */
int muse_settings_chat_items(muse_chat_item_t *out, int max, int *current);
/* Changes whenever the list or the pick does: rebuild a picker when it moves. */
uint32_t muse_settings_chats_gen(void);

void muse_settings_set_volume(int pct);
void muse_settings_set_speaker_on(bool on);
void muse_settings_set_mic_gain(int db);
void muse_settings_set_brightness(int pct);
void muse_settings_set_sleep_s(int secs);
void muse_settings_set_wifi_on(bool on);
void muse_settings_set_ble_on(bool on);
void muse_settings_set_gadget_mode(int mode);
void muse_settings_set_mode_override(bool on, uint32_t until);
void muse_settings_set_home_ssid(const char *ssid);
void muse_settings_set_away_ssid(const char *ssid);
/* A network name is remembered first among the saved ones and joined now;
 * an empty ssid forgets every saved network. */
void muse_settings_set_wifi(const char *ssid, const char *pass);
void muse_settings_set_hatch_host(const char *host);
void muse_settings_set_hatch_vm(const char *vm);
/* append=true adds to the stored token (for chunked BLE writes). */
esp_err_t muse_settings_set_hatch_token(const char *token, bool append);
/* NULL or empty picks the main chat; false (and nothing changed) for an id
 * that isn't a UUID. It is kept in lower case, in RAM only. */
bool muse_settings_set_chat_sid(const char *sid);
