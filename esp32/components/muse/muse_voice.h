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

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/*
 * Push-to-talk turn loop: hold -> stream speech to Hatch, release -> think -> speak.
 * Out of Hatch's reach (Wi-Fi down, say) a note is saved to PSRAM and goes once
 * it's back, on boards with PSRAM. Consumes muse_input_event_t from `queue` and
 * drives muse_state for the UI.
 */
esp_err_t muse_voice_start(QueueHandle_t queue);

/* While on (settings' Sound page), idle mic audio feeds muse_voice_monitor_db(). */
void muse_voice_set_monitor(bool on);
/* Smoothed mic level in dBFS (fast attack, slow release). */
float muse_voice_monitor_db(void);

/* Plays a short chirp at the current volume (when idle). */
void muse_voice_request_chirp(void);

/*
 * Earcons: short, quiet sounds (80 ms at most, but for the charge jingle)
 * that say a key, the screen, or the charger, did something.
 * None with the speaker off (the taps none with Touch sounds off either), and
 * at half the level in Night mode. The talk
 * key's pair (listening starts, and stops) the voice task plays itself; the
 * rest are asked for here, from any task, and play once it's idle, the
 * newest replacing one not yet played (but a tap never one that isn't a
 * tap). One asked for while a turn is under way is dropped rather than played
 * late: no tick over Muse listening or speaking.
 */
typedef enum {
    MUSE_EARCON_TICK,     /* a volume step, at the new volume */
    MUSE_EARCON_LISTEN,   /* listening starts: a soft rising ping */
    MUSE_EARCON_STOP,     /* listening stops: a lower, falling one */
    MUSE_EARCON_CLICK,    /* the power menu opens */
    MUSE_EARCON_CHARGE,   /* plugged in to charge: a little rising arpeggio, half a second */
    MUSE_EARCON_TAP,      /* the screen touched (muse_style_click): a soft tick, 10 ms */
    MUSE_EARCON_TAP_PRIMARY,   /* the same for Send, Done, OK: a lower, rounder tock */
} muse_earcon_t;

void muse_voice_earcon(muse_earcon_t which);

/* Runs muse_audio_loopback_test() at the current volume (when idle); results go to the log. */
void muse_voice_request_loopback(void);

/* Bench test: decodes and plays a built-in MP3 reply. */
void muse_voice_request_mp3test(void);

/*
 * Bench test: `text` (NULL for a sample) as a reply's captions, without
 * asking Muse: said over `secs` (0: at speech pace), Muse's mouth moving
 * and the lyrics following, or `muted`, shown to be read at the face's own
 * pace. Silent either way. A press stops it, as it would a reply.
 */
void muse_voice_bench_caption(const char *text, float secs, bool muted);

/* Asleep with nothing to play: codecs off, Wi-Fi dozing. */
bool muse_voice_resting(void);

/* Voice notes recorded out of Hatch's reach wait to go, the oldest from the
 * last half hour: worth keeping Wi-Fi up for. */
bool muse_voice_notes_waiting(void);
