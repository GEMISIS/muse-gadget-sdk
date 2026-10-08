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
 * Images Muse shows in a chat reply (a delta.presentation event, which the
 * chat session fetches): decoded on a task of their own, saved to the microSD
 * card like display.draw_url's (muse_sd.h), and handed to the face, where
 * Muse takes the photo out of his pocket and holds it up (muse_ui_present).
 * Baseline JPEG only, up to MUSE_PRESENT_MAX bytes; anything else is logged
 * and dropped. Any task; one image at a time, a newer one waiting behind it.
 */

#define MUSE_PRESENT_MAX (512 * 1024)

/* The image's bytes, downloaded already: takes `data` (heap_caps_malloc'd)
 * whatever happens. */
void muse_present_bytes(uint8_t *data, size_t len, const char *label);

/* Fetches the image from `url` over HTTPS first, with `token` as its bearer
 * token (NULL for none). byte_len, if known, sizes the buffer. */
void muse_present_fetch(const char *url, const char *token, const char *label, size_t byte_len);

#ifdef __cplusplus
}
#endif
