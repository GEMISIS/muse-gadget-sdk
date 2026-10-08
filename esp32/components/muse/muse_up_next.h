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

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * "Up next" on the Waveshare 2.16 (CONFIG_MUSE_GADGET_UP_NEXT): one dim line
 * on the face, above the battery, saying what to get ready for next today.
 * The Muse writes it, asked in the background (muse_chat_bg_ask) in a hidden
 * chat of the gadget's own, which the chat list doesn't show: never the
 * picked chat, the face's captions or the speaker. It asks once an hour at
 * most while idle and in reach, and again the first time the screen is on
 * each morning (once Night has ended, before noon), never during a turn. The
 * answer is kept in RAM, and shown until it's three hours old.
 */
#if CONFIG_MUSE_GADGET_UP_NEXT

/* Once a second, from the extras task: asks when it's due, and takes the reply. */
void muse_up_next_tick(void);
/* The line to show, in the face's fonts' characters: false (and out empty)
 * with none, or one over three hours old. Any task. */
bool muse_up_next_line(char *out, size_t cap);
/* The whole reply the line came from (the kept line itself after a restart). */
bool muse_up_next_full(char *out, size_t cap);
/* The serial console's ">brief": asks now, schedule or not (a turn still
 * goes first). Any task. */
void muse_up_next_refresh(void);
/* ">brief?": prints the line kept, its age and whether it's asking, as
 * "@brief {...}". */
void muse_up_next_print(void);

#else

static inline bool muse_up_next_line(char *out, size_t cap)
{
    if (cap) {
        out[0] = '\0';
    }
    return false;
}

static inline bool muse_up_next_full(char *out, size_t cap)
{
    return muse_up_next_line(out, cap);
}

#endif

#ifdef __cplusplus
}
#endif
