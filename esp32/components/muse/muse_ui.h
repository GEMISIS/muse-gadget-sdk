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
#include <stdint.h>

#include "esp_err.h"

/*
 * Bring up the display and build the UI: the avatar on the first tile,
 * settings one swipe to the left. Also owns screen sleep and brightness.
 */
esp_err_t muse_ui_start(void);

/* From any task: the screen has gone dark for sleep (and not yet woken). */
bool muse_ui_dark(void);

/* From any task (the IMU's shake): an earthquake on the face, if it's on
 * screen and idle at the next frame. Muse jitters about for 1.5 s, settling,
 * and is dizzy for a moment after. */
void muse_ui_quake(void);

/* From any task (just plugged in to charge, muse_input.c): Muse cheers for
 * about two seconds (muse_pose_t.plugged), if the face is drawn and idle
 * within a moment, the screen waking meanwhile; otherwise it's dropped. */
void muse_ui_plugged(void);

/* From any task (bench, ">batt="): the face shows this battery level, 0..100,
 * charging or not, in place of the board's reading; -1 goes back to it.
 * Only the face: the settings, power saving and Muse see the real one. */
void muse_ui_fake_battery(int pct, bool charging);

/* The functions below run in the LVGL task (or with the display lock held). */

/* Slide back to the face (e.g. when a talk starts). */
void muse_ui_show_face(void);
/* Settings sub-pages turn off the tile swipe so they can use horizontal gestures. */
void muse_ui_set_swipe_enabled(bool enabled);
/* Temporarily applies a brightness while a slider is dragged. */
void muse_ui_preview_brightness(int pct);

/*
 * display.draw_url, from any task. An image covers the face until a tap, a
 * talk, the menu or muse_ui_image_hide(). Pixels are RGB565, high byte first.
 * The size is false without PSRAM for the image or before the UI is up.
 */
bool muse_ui_image_size(int *w, int *h);
bool muse_ui_image_draw(int x, int y, int w, int h, const uint16_t *pixels);
void muse_ui_image_hide(void);
/*
 * An image from a chat reply (muse_present.h), from any task: Muse takes it
 * out of his pocket and holds it up over his head, a tap shows it full size,
 * and he puts it back at the next talk, the power menu, or a minute after
 * the reply. sizes() gives the screen and the side of the square the held
 * copy must fit (0 on compact layouts, which only show it full size); false
 * without PSRAM or before the UI is up. present() takes both RGB565 buffers
 * (heap_caps_malloc'd) when it returns true: `full` fits the screen.
 */
bool muse_ui_present_sizes(int *screen_w, int *screen_h, int *photo_px);
bool muse_ui_present(uint16_t *full, int fw, int fh, uint16_t *held, int hw, int hh);
/* Watcher camera mode: shows an on-screen shutter hint over the live image. */
void muse_ui_camera_hint(bool visible);

/* Bench testing, from any task: streams the screen over USB serial. */
void muse_ui_request_snapshot(void);
