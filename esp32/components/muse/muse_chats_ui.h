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

/*
 * The Chats screen (CONFIG_MUSE_GADGET_CHATS), left of Muse: which Muse chat
 * the conversation goes to. All of it runs in the LVGL task (muse_ui.c).
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void muse_chats_ui_build(lv_obj_t *tile);
/* Every settings tick; visible while the screen is at least partly showing. */
void muse_chats_ui_tick(bool visible);
/* Something's open over the list (the help card): the page dots hide and
 * swiping away is held off. */
bool muse_chats_ui_typing(void);

#ifdef __cplusplus
}
#endif
