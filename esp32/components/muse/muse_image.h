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
 * Images in formats other than baseline JPEG (muse_present.c decodes those
 * with the ROM's decoder), for Muse to hold up: PNG (libpng) and WebP
 * (components/libwebp, decoding only), decoded straight to RGB565 in PSRAM,
 * shrunk on the way to fit a box (the screen) so a big one never sits whole
 * in memory. Transparency goes over white, like the photo's card. No task of
 * its own, no internal RAM: everything it takes comes from PSRAM.
 */

typedef enum {
    MUSE_IMAGE_UNKNOWN,
    MUSE_IMAGE_JPEG,
    MUSE_IMAGE_PNG,
    MUSE_IMAGE_WEBP,
    MUSE_IMAGE_GIF,
} muse_image_kind_t;

/* What the bytes are, by their signature. */
muse_image_kind_t muse_image_kind(const uint8_t *data, size_t len);
const char *muse_image_kind_name(muse_image_kind_t kind);   /* "JPEG", "PNG"... */
const char *muse_image_kind_ext(muse_image_kind_t kind);    /* "jpg", "png"... for the microSD copy */

#define MUSE_IMAGE_MAX_SIDE 8192               /* wider or taller isn't decoded */
#define MUSE_IMAGE_MAX_PIXELS (24 * 1024 * 1024)
#define MUSE_IMAGE_INTERLACED_MAX (1024 * 1024)   /* an interlaced PNG is read whole: this many pixels at most */

typedef struct {
    uint16_t *px;     /* w x h RGB565, as LVGL takes it (heap_caps_free it) */
    int w, h;         /* fitting the box asked for, or the image's own size if smaller */
    int src_w, src_h; /* the image's own */
} muse_image_t;

/*
 * Decodes a PNG or WebP, fitting it inside fit_w x fit_h (keeping its shape;
 * never enlarged). False with why in err (a short phrase) if it couldn't be.
 */
bool muse_image_decode(const uint8_t *data, size_t len, int fit_w, int fit_h, muse_image_t *out, char *err,
                       size_t err_cap);

#ifdef __cplusplus
}
#endif
