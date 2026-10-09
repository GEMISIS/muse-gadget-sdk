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

/*
 * The passcode's rules, with nothing of the device in them (muse_lock.c keeps
 * them in NVS and runs them; tests/test_muse_lock.py runs them on the host).
 *
 * Two rounds of MUSE_LOCK_TRIES wrong attempts. The first round's last one
 * locks the keypad for MUSE_LOCK_LOCKOUT_S; once that's up, the second round
 * starts, and its last wrong attempt erases the device. The right passcode
 * starts over from the first round, at any point before that.
 */

#define MUSE_LOCK_TRIES 3
#define MUSE_LOCK_LOCKOUT_S 3600
#define MUSE_LOCK_PIN_LEN 6

typedef struct {
    uint8_t failed;      /* wrong attempts in this round */
    uint8_t round;       /* 0, or 1 after the lockout: the round that erases */
    uint32_t lockout_s;  /* left of the lockout; 0 for none */
} muse_lock_policy_t;

typedef enum {
    MUSE_LOCK_RIGHT,     /* unlocked; the counts start over */
    MUSE_LOCK_WRONG,     /* try again: muse_lock_policy_left() more this round */
    MUSE_LOCK_LOCKOUT,   /* that was the round's last: no tries for lockout_s */
    MUSE_LOCK_WIPE,      /* the last of the final round: erase the device */
    MUSE_LOCK_REFUSED,   /* locked out (or erasing): not tried, not counted */
} muse_lock_result_t;

void muse_lock_policy_reset(muse_lock_policy_t *p);
/* Keeps a state read back from storage within its bounds; false if it had to. */
bool muse_lock_policy_sane(muse_lock_policy_t *p);
/* An attempt may be made: not locked out, nor due to be erased. */
bool muse_lock_policy_can_try(const muse_lock_policy_t *p);
/* A wrong attempt: what follows from it. REFUSED, unchanged, if one can't be made. */
muse_lock_result_t muse_lock_policy_wrong(muse_lock_policy_t *p);
/* Wrong attempts left before the next lockout, or the erase. */
int muse_lock_policy_left(const muse_lock_policy_t *p);
/* In the final round: the next MUSE_LOCK_TRIES wrong ones erase it. */
bool muse_lock_policy_final(const muse_lock_policy_t *p);
/* The final round is used up: erase (as on a boot after a power cut). */
bool muse_lock_policy_wipe_due(const muse_lock_policy_t *p);
/* `secs` of the lockout gone by; true when that ended it. */
bool muse_lock_policy_elapse(muse_lock_policy_t *p, uint32_t secs);

/*
 * After how long asleep the passcode is asked for again: a choice from
 * MUSE_LOCK_AFTER_S, one for asleep on battery and one on a charger.
 * MUSE_LOCK_NEVER is "Never".
 */
#define MUSE_LOCK_NEVER UINT32_MAX
#define MUSE_LOCK_AFTER_COUNT 7
#define MUSE_LOCK_AFTER_BATTERY_DEFAULT 2   /* 5 minutes */
#define MUSE_LOCK_AFTER_CHARGER_DEFAULT 4   /* 1 hour */
extern const uint32_t MUSE_LOCK_AFTER_S[MUSE_LOCK_AFTER_COUNT];
extern const char *const MUSE_LOCK_AFTER_NAMES[MUSE_LOCK_AFTER_COUNT];

/* The choice index `i` as seconds; an unknown one as the battery default's. */
uint32_t muse_lock_policy_after_s(int i);
/*
 * Whether a sleep of `slept_s` asks for the passcode on waking. `on_battery`
 * and `on_charger`: whether any of it was spent on each. Some of each (it was
 * unplugged, or plugged in, partway) takes the stricter of the two.
 */
bool muse_lock_policy_sleep_locks(uint32_t slept_s, bool on_battery, bool on_charger, int battery_choice,
                                  int charger_choice);

/* A command from the Muse (or over the serial console) that shows or changes
 * the owner's things, and so is refused while locked. */
bool muse_lock_policy_blocks_remote(const char *command);
bool muse_lock_policy_blocks_console(const char *line);
