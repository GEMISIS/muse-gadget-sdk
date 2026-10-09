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
 * The JPEGs the ROM's baseline decoder (tjpgd) refuses: progressive ones,
 * common on the web, and extended sequential ones, or odd chroma sampling.
 * A progressive JPEG has to be held whole as DCT coefficients until its last
 * scan, so this keeps only the ones the scale it decodes at needs: at N/8
 * the top-left N x N of each block's 64 (an N-point IDCT, as libjpeg's
 * scaled ones), all of them at 8/8, and at 1/8 just the DC: one pixel a
 * block, its AC scans skipped altogether. It picks the
 * sharpest scale the screen can use that fits the memory it's given, and
 * averages the result down into the box a row at a time, so the image never
 * sits whole at full size. All of it from PSRAM (MUSE_BIG_CAPS); no task of
 * its own, under 1 KB of stack. Arithmetic-coded, lossless, 12-bit and CMYK
 * JPEGs are refused.
 *
 * The host tests build it with MUSE_JPEG_HOST (malloc).
 */

#define MUSE_JPEG_MAX_SIDE 8192

typedef struct {
    int w, h;          /* the image's own */
    int comps;         /* 1 (grey) or 3 */
    int sof;           /* 0xC0 baseline, 0xC1 extended, 0xC2 progressive, ... */
    bool progressive;
} muse_jpeg_info_t;

/* Its frame header, without decoding. False if it isn't a JPEG with one. */
bool muse_jpeg_info(const uint8_t *data, size_t len, muse_jpeg_info_t *info);

/* Whether it's one for the ROM's decoder: baseline, 1 or 3 components. */
bool muse_jpeg_is_baseline(const muse_jpeg_info_t *info);

typedef struct {
    uint16_t *px;      /* w x h RGB565, as LVGL takes it (heap_caps_free it) */
    int w, h;          /* fitting the box asked for, or as decoded if that's smaller */
    int src_w, src_h;
    int eighths;       /* decoded at this many eighths of its size: 8 all of it, 1 the DC alone */
    bool progressive;
    size_t need;       /* the most memory it took, in bytes */
} muse_jpeg_t;

/*
 * Decodes it to fit fit_w x fit_h (keeping its shape, never enlarged), in no
 * more than `budget` bytes, at max_eighths/8 of its size or less (8
 * normally; the tests ask for less). False with why in err (a short phrase)
 * if it couldn't be: "out of memory" if even 1/8 needs more than the budget.
 */
bool muse_jpeg_decode(const uint8_t *data, size_t len, int fit_w, int fit_h, size_t budget, int max_eighths,
                      muse_jpeg_t *out, char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif
