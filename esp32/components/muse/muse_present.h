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
 * Asks Muse, in the background (muse_chat_bg_ask_for, in this gadget's own
 * chat), to push the workspace file at `path` with display.show_image: once
 * no turn runs and no other background request is under way, giving up after
 * two minutes. A newer image replaces one still waiting; one asked for in the
 * last ten minutes isn't asked for again. Nothing shows until the push is
 * all here. Any task.
 */
void muse_present_ask(const char *path, const char *label);

#ifdef __cplusplus
}
#endif
