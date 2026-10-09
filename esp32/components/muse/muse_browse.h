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

/*
 * Muse using its browser in a turn: the "browser_task" presentations that
 * stream in as it works (a few a second: what it's at, the site and page,
 * Starting, Working, Completed), kept as the turn's history of steps rather
 * than shown as widgets. The face acts it out (MUSE_ACT_BROWSE: Muse at a
 * computer, the site's colour on its screen) while it runs, and a tap on him
 * (or the chip after) opens "What Muse did" (muse_widget_ui.h).
 *
 * Updates for one task come many times over; the same step again only
 * freshens it. A task first seen in an earlier turn, or already finished
 * when first seen (an old one replayed), isn't the turn's and is passed
 * over. A new turn forgets the history (muse_browse_turn).
 *
 * The model (muse_browse_apply) is plain C and cJSON, for host tests; the
 * turn's copy and its lock are in PSRAM.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_BROWSE_STEPS 24
#define MUSE_BROWSE_TASKS 8        /* task ids remembered, to pass over older turns' */
#define MUSE_BROWSE_ID 72
#define MUSE_BROWSE_WHAT 40
#define MUSE_BROWSE_SITE 48
#define MUSE_BROWSE_TITLE 120
#define MUSE_BROWSE_URL 200

typedef struct {
    char what[MUSE_BROWSE_WHAT];          /* "Reading page"; "" for none said */
    char site[MUSE_BROWSE_SITE];          /* "rei.com": no "www." */
    char title[MUSE_BROWSE_TITLE];        /* the page's */
    char url[MUSE_BROWSE_URL];
    int64_t at_ms;                        /* when it began */
} muse_browse_step_t;

typedef struct {
    uint32_t seq;                         /* bumped by every change */
    uint32_t turn;                        /* muse_browse_turn's count */
    char task[MUSE_BROWSE_ID];            /* the turn's task ("" none yet) */
    bool done;                            /* it's finished (completed, failed, stopped) */
    bool failed;
    int64_t started_ms, done_ms;
    int count;
    muse_browse_step_t steps[MUSE_BROWSE_STEPS];   /* oldest first; the last is where it is */
    struct {
        char id[MUSE_BROWSE_ID];
        uint32_t turn;                    /* the turn it was first seen in */
        bool stale;                       /* finished when first seen */
    } known[MUSE_BROWSE_TASKS];
    int known_next;
} muse_browse_t;

/* A new turn: no task, no history (the tasks seen are remembered). */
void muse_browse_reset(muse_browse_t *b);
/* A browser_task presentation's payload (or its "data") at now_ms: true if it changed *b. */
bool muse_browse_apply(muse_browse_t *b, const cJSON *payload, int64_t now_ms);
/* Running: the turn's task is under way. */
static inline bool muse_browse_running(const muse_browse_t *b)
{
    return b->task[0] && !b->done;
}
/* A site's colour (0xRRGGBB): bright enough on dark, from its name. */
uint32_t muse_browse_color(const char *site);

/* ---- The turn's: the chat session updates, the face takes. Any task. ---- */

void muse_browse_init(void);
/* A new turn starts (or the chat changed). */
void muse_browse_turn(void);
/* The session: a browser_task payload for the turn's chat. */
void muse_browse_update(const cJSON *payload);
uint32_t muse_browse_seq(void);
/* Copies it; false if there's none (no PSRAM). */
bool muse_browse_get(muse_browse_t *out);
/* Bench (">widget=browser"): a made-up task, its steps fed over ~8 s by
 * muse_browse_bench_tick (the face's, every frame). */
void muse_browse_bench(void);
void muse_browse_bench_tick(void);

#ifdef __cplusplus
}
#endif
