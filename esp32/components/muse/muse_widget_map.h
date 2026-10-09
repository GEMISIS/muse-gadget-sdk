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
 * A map widget's map in the sheet (muse_widget_ui.c), and the pictures of
 * products' cards. The map is fitted to the places (and the user, from
 * muse_where.h, if they're within 15 km: else an arrow at its edge says
 * which way and how far): drawn at once as a plain street-plan look, then
 * the real thing when its tiles come: OpenStreetMap's own (free, no key,
 * for light use by an app that names itself), fetched over HTTPS on
 * muse_present's task, two to six 256 px PNG tiles, and turned dark here to
 * sit in the sheet (land dark, roads lighter, words light). The last views
 * and tiles are kept, so a view seen again costs nothing. Pins are numbered
 * as the list under it is, the user a pulsing dot (its halo as wide as the
 * fix is rough).
 *
 * All in PSRAM. The LVGL task, but for the fetches.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "muse_widget.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*muse_map_pin_t)(int row);

/*
 * The widget's places in the order to list them: nearest first when where
 * the user is is known, else as they came. Their count; rows with no place
 * aren't in it.
 */
int muse_widget_map_order(const muse_widget_t *w, uint8_t *order);

/* "0.4 mi NW" from the user to a place's row; false if either's not known. */
bool muse_widget_map_distance(const muse_widget_row_t *r, char *dst, size_t cap);

/* The user's location is a city's at best: say the distances are rough. */
bool muse_widget_map_rough(void);

/*
 * The map, `width` x `height`, in `parent`: the places in `order` (n of
 * them, numbered 1..n), `selected` (a row, or -1) picked out, the others
 * dimmed. A pin's tap calls `on_pin` with its row.
 */
lv_obj_t *muse_widget_map_build(lv_obj_t *parent, const muse_widget_t *w, const uint8_t *order, int n, int width,
                                int height, int selected, muse_map_pin_t on_pin);

/* Every frame: tiles come in. */
void muse_widget_map_tick(void);

/* A product's picture, at most w x h: asked for (once), then had, as a source for an lv_image. */
void muse_widget_pic_want(const char *url, int w, int h);
const lv_image_dsc_t *muse_widget_pic(const char *url);

#ifdef __cplusplus
}
#endif
