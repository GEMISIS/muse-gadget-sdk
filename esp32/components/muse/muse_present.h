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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Images Muse shows the user: pushed whole over Link's private session with
 * display.show_image (main/gadget_commands.c), decoded on a task of their
 * own, saved to the microSD card like display.draw_url's (muse_sd.h), and
 * handed to the face, where Muse takes the photo out of his pocket and holds
 * it up (muse_ui_present). Baseline JPEG only, up to MUSE_PRESENT_MAX bytes;
 * anything else is logged and dropped. One image at a time, a newer one
 * waiting behind it.
 *
 * An image in a chat reply (a delta.presentation event) only names a file in
 * Muse's workspace, which the VM won't serve to the gadget, so the chat
 * session asks for it to be pushed (muse_present_ask).
 */

#define MUSE_PRESENT_MAX (512 * 1024)

/* The image's bytes, all here: takes `data` (heap_caps_malloc'd) whatever
 * happens. False if it was dropped unshown (busy, or out of memory). Any task. */
bool muse_present_bytes(uint8_t *data, size_t len, const char *label);

/*
 * Asks Muse, in the background (muse_chat_bg_ask_for, in a chat of its own),
 * to push the image at `path` (a workspace file, or an http(s) URL Muse
 * downloads) with display.show_image, unless Muse pushes one by itself first:
 * every mode's contract asks it to, in the same turn (muse_gadget_mode.c).
 * So the request waits for the reply's turn to be over (muse_present_turn_over),
 * then MUSE_PRESENT_PUSH_WAIT_US more; any image pushed meanwhile
 * (muse_present_bytes) calls it off. Then it goes once no turn runs and no
 * other background request is under way, giving up after two minutes. A
 * newer image replaces one still waiting; one asked for in the last ten
 * minutes isn't asked for again. Nothing shows until the push is all here.
 * Any task.
 */
#define MUSE_PRESENT_PUSH_WAIT_US (15 * 1000000LL)
void muse_present_ask(const char *path, const char *label);

/* The reply's turn is over (or there's none): an image muse_present_ask
 * holds back is asked for MUSE_PRESENT_PUSH_WAIT_US from now, unless one is
 * pushed meanwhile. Any task. */
void muse_present_turn_over(void);

/* Counts the pushed images handled: shown, or dropped as unshowable. Moves
 * once the face has the image (muse_ui_present), which then takes it out of
 * Muse's pocket. Any task. */
uint32_t muse_present_seq(void);

#ifdef __cplusplus
}
#endif
