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

#include "lvgl.h"

/*
 * The settings tile (swipe left from Muse): Modes, General (Wi-Fi, Display,
 * Sound and Passcode pages), Advanced (the Muse connection, Bluetooth,
 * Battery, About and Reset device pages) and Power off. Runs entirely in the LVGL task;
 * hardware state is polled from the owning modules.
 */

void muse_settings_ui_build(lv_obj_t *tile);

/* Call periodically from the LVGL task; `visible` = settings tile is showing. */
void muse_settings_ui_tick(bool visible);

/* True when a sub-page or a dialog (muse_dialog.h) is open: the tileview
 * must not steal horizontal swipes. */
bool muse_settings_ui_in_subpage(void);

/* Back to the first page, from wherever it is (locking, muse_lock.h): what was
 * typed on the way is dropped. */
void muse_settings_ui_go_home(void);
