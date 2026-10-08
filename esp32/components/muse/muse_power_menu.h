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
 * For boards whose side keys set the volume (MUSE_BTN_VOL_*): a volume bar
 * that shows for a moment after each step, and the power menu a long press
 * opens: Sleep, Power off, Restart, Cancel. The menu takes taps, or the keys
 * while it's open: volume down moves up, volume up moves down, talk selects.
 */

typedef enum {
    MUSE_POWER_MENU_OPEN,
    MUSE_POWER_MENU_UP,
    MUSE_POWER_MENU_DOWN,
    MUSE_POWER_MENU_SELECT,
    MUSE_POWER_MENU_CLOSE,
} muse_power_menu_key_t;

/* Safe from any task. */
void muse_power_menu_key(muse_power_menu_key_t key);
/* True from the moment OPEN is sent until the menu closes, so the next key
 * press goes to the menu even before it's drawn. */
bool muse_power_menu_is_open(void);
/* Shows the volume bar at pct (0..100). */
void muse_power_menu_show_volume(int pct);

/* LVGL task only: build onto the top layer, under the sleep cover. */
void muse_power_menu_build(lv_obj_t *layer, int w, int h);
/* The screen turned (muse_board_t.set_turn): the keys' hint follows them. */
void muse_power_menu_set_turn(int quarters);
/* Every frame, asleep or not: handles queued keys, times the volume bar out
 * and closes everything when the screen sleeps. */
void muse_power_menu_tick(float now);
