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
 * Typing an answer on the 2.16 (a widget's text field, muse_widget_ui.c):
 * the whole screen, Cancel at the top left and the answer's button (Send,
 * Done) at the top right, the question, the field, and a phone's keyboard
 * across the bottom: ten keys a row, dark keys, the return key in the accent
 * colour, a shift that starts each answer with a capital, numbers and
 * symbols a key away. It slides up, and back down when done. LVGL task.
 */

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* `text` the answer (trimmed, not empty); NULL if cancelled. */
typedef void (*muse_keys_done_t)(const char *text, void *ctx);

/* Over `parent` (the face), filling it. `send` names the return key and the top button. */
void muse_widget_keys_open(lv_obj_t *parent, const char *prompt, const char *placeholder, const char *initial,
                           const char *send, muse_keys_done_t done, void *ctx);
/* It's up. */
bool muse_widget_keys_up(void);
/* Gone at once, unanswered, with no call (a new turn). */
void muse_widget_keys_close(void);

#ifdef __cplusplus
}
#endif
