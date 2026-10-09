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

/* The passcode's rules (muse_lock_policy.h): plain C, run on the host too. */
#include "muse_lock_policy.h"

#include <string.h>

const uint32_t MUSE_LOCK_AFTER_S[MUSE_LOCK_AFTER_COUNT] = { 0, 60, 300, 900, 3600, 4 * 3600, MUSE_LOCK_NEVER };
const char *const MUSE_LOCK_AFTER_NAMES[MUSE_LOCK_AFTER_COUNT] = {
    "Immediately", "1 minute", "5 minutes", "15 minutes", "1 hour", "4 hours", "Never",
};

void muse_lock_policy_reset(muse_lock_policy_t *p)
{
    memset(p, 0, sizeof(*p));
}

bool muse_lock_policy_sane(muse_lock_policy_t *p)
{
    bool ok = true;
    if (p->round > 1) {
        p->round = 1;   /* the stricter */
        ok = false;
    }
    if (p->failed > MUSE_LOCK_TRIES) {
        p->failed = MUSE_LOCK_TRIES;
        ok = false;
    }
    if (p->lockout_s > MUSE_LOCK_LOCKOUT_S) {
        p->lockout_s = MUSE_LOCK_LOCKOUT_S;
        ok = false;
    }
    /* A first round used up without its lockout: the lockout, whole. */
    if (p->round == 0 && p->failed >= MUSE_LOCK_TRIES) {
        p->round = 1;
        p->failed = 0;
        p->lockout_s = MUSE_LOCK_LOCKOUT_S;
        ok = false;
    }
    return ok;
}

bool muse_lock_policy_wipe_due(const muse_lock_policy_t *p)
{
    return p->round >= 1 && p->failed >= MUSE_LOCK_TRIES;
}

bool muse_lock_policy_can_try(const muse_lock_policy_t *p)
{
    return p->lockout_s == 0 && !muse_lock_policy_wipe_due(p);
}

muse_lock_result_t muse_lock_policy_wrong(muse_lock_policy_t *p)
{
    if (!muse_lock_policy_can_try(p)) {
        return MUSE_LOCK_REFUSED;
    }
    if (++p->failed < MUSE_LOCK_TRIES) {
        return MUSE_LOCK_WRONG;
    }
    if (p->round == 0) {
        p->round = 1;
        p->failed = 0;
        p->lockout_s = MUSE_LOCK_LOCKOUT_S;
        return MUSE_LOCK_LOCKOUT;
    }
    return MUSE_LOCK_WIPE;
}

int muse_lock_policy_left(const muse_lock_policy_t *p)
{
    return p->failed >= MUSE_LOCK_TRIES ? 0 : MUSE_LOCK_TRIES - p->failed;
}

bool muse_lock_policy_final(const muse_lock_policy_t *p)
{
    return p->round >= 1;
}

bool muse_lock_policy_elapse(muse_lock_policy_t *p, uint32_t secs)
{
    if (!p->lockout_s) {
        return false;
    }
    p->lockout_s = secs >= p->lockout_s ? 0 : p->lockout_s - secs;
    return p->lockout_s == 0;
}

uint32_t muse_lock_policy_after_s(int i)
{
    if (i < 0 || i >= MUSE_LOCK_AFTER_COUNT) {
        i = MUSE_LOCK_AFTER_BATTERY_DEFAULT;
    }
    return MUSE_LOCK_AFTER_S[i];
}

bool muse_lock_policy_sleep_locks(uint32_t slept_s, bool on_battery, bool on_charger, int battery_choice,
                                  int charger_choice)
{
    uint32_t after;
    if (on_battery && on_charger) {
        uint32_t b = muse_lock_policy_after_s(battery_choice), c = muse_lock_policy_after_s(charger_choice);
        after = b < c ? b : c;
    } else if (on_charger) {
        after = muse_lock_policy_after_s(charger_choice);
    } else {
        after = muse_lock_policy_after_s(battery_choice);   /* or not known: as on battery */
    }
    return after != MUSE_LOCK_NEVER && slept_s >= after;
}

/* `s` is `name`, or starts with it and then '=' (or `name` ends in '='). */
static bool is(const char *s, const char *name)
{
    size_t n = strlen(name);
    if (strncmp(s, name, n) != 0) {
        return false;
    }
    return s[n] == '\0' || s[n] == '=' || name[n - 1] == '=';
}

bool muse_lock_policy_blocks_remote(const char *command)
{
    static const char *const BLOCKED[] = {
        "show_text", "set_mode", "set_chat", "list_chats", "display.show_image", "display.draw_url",
        "display.show_animation", "camera.capture", "bug.report",
    };
    for (size_t i = 0; i < sizeof(BLOCKED) / sizeof(BLOCKED[0]); i++) {
        if (!strcmp(command, BLOCKED[i])) {
            return true;
        }
    }
    return false;
}

bool muse_lock_policy_blocks_console(const char *line)
{
    /* What stays: the lock's own, the device's bare state, the battery
     * meter, sleep and wake, and what shakes or cheers Muse on the face
     * (which the lock covers). Anything else, setup commands included, waits. */
    static const char *const ALLOWED[] = {
        "lock", "lockstate", "pin=", "status", "power", "power.reset", "nap", "crash", "batt=", "quake", "charge",
    };
    for (size_t i = 0; i < sizeof(ALLOWED) / sizeof(ALLOWED[0]); i++) {
        if (is(line, ALLOWED[i])) {
            return false;
        }
    }
    return true;
}
