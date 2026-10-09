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
#include <stdint.h>

#include "muse_lock_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The passcode: a 6-digit PIN that keeps what's on the gadget to its owner
 * when it's out of their hands. Set in Settings > General > Passcode, it's
 * asked for at every start, after the screen's been off a while (one time on
 * battery, another on a charger), on "Lock now" (the power menu, or two taps
 * of the talk button), and on entering a mode that asks for it. The rules for
 * wrong ones are muse_lock_policy.h's.
 *
 * Locked, the face shows only Muse and the clock, and the keypad over it
 * (muse_lock_ui.h); nothing that shows or changes the owner's things answers:
 * the talk button, the Muse's pushes, the console's and BLE's commands.
 *
 * Kept in NVS ("muse_lock"): the PIN's salted hash (muse_lock_hash.h), never
 * the PIN, and the wrong-attempt counts and the lockout's time left, written
 * before an attempt's result shows, so pulling the power can't undo one.
 * NVS isn't encrypted on this build (that burns an eFuse), so whoever reads
 * the flash out reads the gadget's data with it: the passcode guards everyday
 * use, and its hash only makes the digits themselves slow to find.
 */

/* After muse_settings_init (NVS up): locked from the start if there's a PIN. */
void muse_lock_init(void);

/* Any task. */
bool muse_lock_locked(void);
bool muse_lock_has_pin(void);
/* Locks now, if there's a PIN; the face follows on its next frame. */
void muse_lock_now(const char *why);
/* Bumped on every lock and unlock (and PIN change), for the UI to follow. */
uint32_t muse_lock_gen(void);

/* The screen going dark and lighting again (muse_state_set_asleep), and the
 * power while it's dark (muse_state_set_power): whether waking asks for it. */
void muse_lock_note_asleep(bool asleep);
void muse_lock_note_power(bool on_battery);

/* Modes that ask for the passcode on the way in (muse_gadget_mode.h), all off
 * until the modes' own settings say otherwise. Setting one writes NVS. */
bool muse_lock_mode_requires(int mode);
void muse_lock_set_mode_requires(int mode, bool on);
/* The mode task, on a switch: locks if that mode asks for it. */
void muse_lock_mode_entered(int mode);

/* The "Require after sleep" choices (MUSE_LOCK_AFTER_S), on battery or a charger. */
int muse_lock_after(bool charger);
void muse_lock_set_after(bool charger, int choice);

/* ---- LVGL task (NVS writes, and the slow hash) ---- */

/* An attempt at the PIN, counted (and saved) before it's checked. RIGHT
 * unlocks; a LOCKOUT or WIPE locks, if it wasn't. */
muse_lock_result_t muse_lock_try(const char *pin);
/* A new PIN, replacing any; the counts start over. False if it couldn't be kept. */
bool muse_lock_set_pin(const char *pin);
/* No PIN: unlocked, for good. */
void muse_lock_clear_pin(void);
void muse_lock_state(muse_lock_policy_t *out);
/* Each frame: the lockout's countdown, saved now and then. */
void muse_lock_tick(void);
/* The final round's used up: erasing is due (muse_lock_wipe). */
bool muse_lock_wipe_due(void);
/* Erases everything: the SD card's Muse folder, then all of NVS (Wi-Fi,
 * pairing, chats, settings, the PIN), and restarts into setup. */
void muse_lock_wipe(void);

/* ---- Bench (the serial console) ---- */

/* "@lock {...}" */
void muse_lock_print(void);
/* ">pin=": an attempt, as from the keypad, on the LVGL task; "@pin {...}". */
void muse_lock_bench_pin(const char *pin);
/* The LVGL task: an attempt from ">pin=" waiting, copied out (and cleared). */
bool muse_lock_bench_take(char pin[MUSE_LOCK_PIN_LEN + 1]);

#ifdef __cplusplus
}
#endif
