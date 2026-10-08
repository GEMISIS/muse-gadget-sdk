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

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Deleting a chat from Muse, for the Chats screen (CONFIG_MUSE_GADGET_CHATS).
 * Muse has no call for it, and won't delete the chat it's replying in, so
 * the gadget asks it in another: its own (muse_settings_gadget_chat_sid), in
 * the background (muse_chat_bg_ask_for), which Muse does by the chat's id.
 * Requests wait in RAM, a few at most, while the background request is
 * someone else's or Muse is out of reach; a restart drops them.
 */
#if CONFIG_MUSE_HATCH && CONFIG_MUSE_GADGET_CHATS

/* Queues asking Muse to delete chat `sid`, called `title` there (or NULL);
 * false if too many are waiting. Any task. */
bool muse_chat_delete(const char *sid, const char *title);
/* Asks for the next one once it can, and takes the reply: from one task
 * only, about once a second (the extras task, or the Chats screen's tick in
 * a build without it). */
void muse_chat_delete_tick(void);
/* How the latest one is going, as a line for the Chats screen, in `out`.
 * Returns a count that changes with it, 0 before the first. Any task. */
uint32_t muse_chat_delete_status(char *out, size_t cap);

#endif

#ifdef __cplusplus
}
#endif
