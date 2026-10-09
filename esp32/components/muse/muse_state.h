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

#include "muse_activity.h"

/*
 * Shared state between the voice pipeline (writer) and the UI (reader).
 * Scalars are plain word-sized stores; the caption is guarded by a spinlock.
 */

typedef enum {
    MUSE_MODE_BOOT = 0,
    MUSE_MODE_IDLE,
    MUSE_MODE_LISTENING,
    MUSE_MODE_THINKING,
    MUSE_MODE_SPEAKING,
    MUSE_MODE_ERROR,
    MUSE_MODE_OFF,       /* powering down */
    MUSE_MODE_COUNT,
} muse_mode_t;

typedef struct {
    int battery_pct;   /* -1 when no battery is connected */
    int battery_mv;    /* 0 when the board can't measure it */
    bool charging;
    bool usb;
} muse_power_t;

void muse_state_init(void);

void muse_state_set_mode(muse_mode_t mode);
muse_mode_t muse_state_mode(float *secs_in_mode);

/* How far a turn waiting on Muse (THINKING) has got, for the face to show in
 * place of a caption (muse_pixel.h's acts): the note going up, there (sent,
 * or its words heard), then the answer's words in. Each change of mode
 * starts it over at SENDING. */
typedef enum {
    MUSE_TURN_SENDING,
    MUSE_TURN_SENT,
    MUSE_TURN_ANSWERED,
} muse_turn_t;

void muse_state_set_turn(muse_turn_t turn);
muse_turn_t muse_state_turn(void);

/* What Muse says he's at work on in the turn (muse_activity.h), for the face
 * to act out: set by the chat session from agent.status, NONE between
 * turns. Any task. */
void muse_state_set_activity(muse_activity_t activity);
muse_activity_t muse_state_activity(void);

/* Live audio level (mic while listening, playback while speaking), 0..1. */
void muse_state_set_level(float level);
float muse_state_level(void);

/* Progress of the current phase (recording length or playback), 0..1. */
void muse_state_set_progress(float progress);
float muse_state_progress(void);

/* Room for a page of reply text; longer captions are cut short. */
#define MUSE_CAPTION_MAX 400

void muse_state_set_caption(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Copies the caption if it changed since *version; returns true on change. */
bool muse_state_caption(char *out, size_t out_len, uint32_t *version);

/*
 * The reply being answered with, whole (up to MUSE_REPLY_MAX, in PSRAM), and
 * how far its speech has got (`at`, bytes of it said), for the full layout's
 * captions to follow line by line (muse_lyrics_ui.h); set with its `page`,
 * the caption, which everything else shows. `spoken`: there's speech to
 * follow, not silence at reading pace. It's the caption's: once any other
 * caption is set, it's stale until set again. Without PSRAM only the page is.
 */
#define MUSE_REPLY_MAX 4096

void muse_state_set_reply(const char *page, const char *text, size_t at, bool spoken);
/* True while the reply is the caption; copies its text (with the caption's
 * ASCII stand-ins, `at` counted in them) if it changed since *version. */
bool muse_state_reply(char *out, size_t out_len, uint32_t *version, size_t *at, bool *spoken);

/* How much reply text the screen shows at once, set by the UI: lines of up to
 * `cols` characters. The replies are wrapped and paged to fit. A screen that
 * draws CJK larger than its other caption text sets a page for replies with CJK
 * in them too; without one they get the usual page. */
void muse_state_set_page(int cols, int lines);
void muse_state_set_cjk_page(int cols, int lines);
void muse_state_page(bool cjk, int *cols, int *lines);

void muse_state_set_power(const muse_power_t *power);
muse_power_t muse_state_power(void);
/* A battery and no USB power (charger or computer). Power saving asleep (the
 * voice task resting, the display paused, the Wi-Fi nap) is only for then;
 * a change nudges (muse_state_wait_awake). */
bool muse_state_on_battery(void);
/* Bench tests over USB (">nap"): count as on battery until turned off. */
void muse_state_set_as_if_battery(bool on);

/* Any user interaction: keeps Muse awake. */
void muse_state_poke(void);
float muse_state_idle_secs(void);

/* Screen sleep (display dark, rendering paused). Waking also pokes. */
void muse_state_set_asleep(bool asleep);
bool muse_state_asleep(void);
/* For a task with nothing to do asleep: returns once awake, nudged
 * (muse_state_nudge, which doesn't wake the screen) or timeout_ms passes. */
void muse_state_wait_awake(uint32_t timeout_ms);
void muse_state_nudge(void);

/* Touch/pet reaction. */
void muse_state_make_happy(void);
float muse_state_happiness(void);
