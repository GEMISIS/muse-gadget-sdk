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
 * PNG and WebP for muse_present.c (muse_image.h). A PNG is read a row at a
 * time and averaged down into the box as it comes, so a 4000 px one takes a
 * row's worth of memory, not the image's; an interlaced one (rare) has to be
 * read whole, so it's capped. A WebP is scaled by libwebp itself while it
 * decodes, straight to RGB565 unless it has transparency.
 *
 * The host tests build this with MUSE_IMAGE_HOST (malloc, and libpng if the
 * host has it: MUSE_IMAGE_WITH_PNG).
 */
#include "muse_image.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MUSE_IMAGE_HOST
#define big_alloc(n) malloc(n)
#define big_calloc(n, s) calloc((n), (s))
#define big_free(p) free(p)
#ifndef MUSE_IMAGE_WITH_PNG
#define MUSE_IMAGE_WITH_PNG 1
#endif
#else
#include "esp_heap_caps.h"
#include "muse_mem.h"
#define big_alloc(n) heap_caps_malloc((n), MUSE_BIG_CAPS)
#define big_calloc(n, s) heap_caps_calloc((n), (s), MUSE_BIG_CAPS)
#define big_free(p) heap_caps_free(p)
#define MUSE_IMAGE_WITH_PNG 1
#endif

#if MUSE_IMAGE_WITH_PNG
#include "png.h"
#endif
#include "src/webp/decode.h"

muse_image_kind_t muse_image_kind(const uint8_t *d, size_t len)
{
    if (len > 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) {
        return MUSE_IMAGE_JPEG;
    }
    if (len > 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) {
        return MUSE_IMAGE_PNG;
    }
    if (len > 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) {
        return MUSE_IMAGE_WEBP;
    }
    if (len > 6 && (!memcmp(d, "GIF87a", 6) || !memcmp(d, "GIF89a", 6))) {
        return MUSE_IMAGE_GIF;
    }
    return MUSE_IMAGE_UNKNOWN;
}

const char *muse_image_kind_name(muse_image_kind_t kind)
{
    static const char *const NAMES[] = { "not an image", "JPEG", "PNG", "WebP", "GIF" };
    return kind <= MUSE_IMAGE_GIF ? NAMES[kind] : NAMES[0];
}

const char *muse_image_kind_ext(muse_image_kind_t kind)
{
    static const char *const EXTS[] = { "bin", "jpg", "png", "webp", "gif" };
    return kind <= MUSE_IMAGE_GIF ? EXTS[kind] : EXTS[0];
}

static void say(char *err, size_t cap, const char *why)
{
    if (err && cap) {
        snprintf(err, cap, "%s", why);
    }
}

/* Fits w x h inside bw x bh, keeping its shape; never bigger than it is. */
static void fit_in(int w, int h, int bw, int bh, int *ow, int *oh)
{
    if (w <= bw && h <= bh) {
        *ow = w;
        *oh = h;
        return;
    }
    if ((int64_t)w * bh > (int64_t)h * bw) {
        *ow = bw;
        *oh = (int)((int64_t)h * bw / w);
    } else {
        *oh = bh;
        *ow = (int)((int64_t)w * bh / h);
    }
    *ow = *ow > 0 ? *ow : 1;
    *oh = *oh > 0 ? *oh : 1;
}

static inline uint16_t rgb565(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)((r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3);
}

/* ---- Averaging rows down into the box ------------------------------------ */

typedef struct {
    int sw, sh, ow, oh;
    uint32_t *acc;       /* ow x 3: the output row being filled, summed */
    uint32_t *cols;      /* ow: source columns in each output column */
    int oy, rows;        /* the output row, and the source rows in it so far */
    uint16_t *out;
} shrink_t;

static bool shrink_init(shrink_t *s, int sw, int sh, int ow, int oh)
{
    memset(s, 0, sizeof(*s));
    s->sw = sw;
    s->sh = sh;
    s->ow = ow;
    s->oh = oh;
    s->acc = big_calloc((size_t)ow * 3, sizeof(uint32_t));
    s->cols = big_calloc((size_t)ow, sizeof(uint32_t));
    s->out = big_alloc((size_t)ow * oh * sizeof(uint16_t));
    if (!s->acc || !s->cols || !s->out) {
        return false;
    }
    for (int x = 0; x < sw; x++) {
        s->cols[(int64_t)x * ow / sw]++;
    }
    return true;
}

static void shrink_flush(shrink_t *s)
{
    if (!s->rows) {
        return;
    }
    uint16_t *o = s->out + (size_t)s->oy * s->ow;
    for (int x = 0; x < s->ow; x++) {
        uint32_t n = s->cols[x] * (uint32_t)s->rows;
        uint32_t *a = s->acc + x * 3;
        o[x] = n ? rgb565((a[0] + n / 2) / n, (a[1] + n / 2) / n, (a[2] + n / 2) / n) : 0xFFFF;
        a[0] = a[1] = a[2] = 0;
    }
    s->rows = 0;
}

/* One source row, RGBA, its transparency over white. */
static void shrink_row(shrink_t *s, const uint8_t *rgba, int sy)
{
    int oy = (int)((int64_t)sy * s->oh / s->sh);
    if (oy != s->oy) {
        shrink_flush(s);
        s->oy = oy;
    }
    for (int x = 0; x < s->sw; x++, rgba += 4) {
        uint32_t *a = s->acc + (size_t)((int64_t)x * s->ow / s->sw) * 3;
        uint32_t al = rgba[3], white = 255 * (255 - al);
        a[0] += (rgba[0] * al + white) / 255;
        a[1] += (rgba[1] * al + white) / 255;
        a[2] += (rgba[2] * al + white) / 255;
    }
    s->rows++;
}

static void shrink_free(shrink_t *s, bool keep_out)
{
    big_free(s->acc);
    big_free(s->cols);
    if (!keep_out) {
        big_free(s->out);
    }
    s->acc = NULL;
    s->cols = NULL;
}

/* ---- PNG ------------------------------------------------------------------ */

#if MUSE_IMAGE_WITH_PNG
typedef struct {
    const uint8_t *data;
    size_t len, pos;
    char *err;
    size_t err_cap;
} png_src_t;

static void png_in(png_structp png, png_bytep out, size_t n)
{
    png_src_t *src = png_get_io_ptr(png);
    if (n > src->len - src->pos) {
        png_error(png, "it's cut short");
    }
    memcpy(out, src->data + src->pos, n);
    src->pos += n;
}

static void png_fail(png_structp png, png_const_charp msg)
{
    png_src_t *src = png_get_error_ptr(png);
    say(src->err, src->err_cap, msg);
    png_longjmp(png, 1);
}

static void png_warn(png_structp png, png_const_charp msg)
{
    (void)png;
    (void)msg;
}

static png_voidp png_mem(png_structp png, png_alloc_size_t n)
{
    (void)png;
    return big_alloc(n);
}

static void png_mem_free(png_structp png, png_voidp p)
{
    (void)png;
    big_free(p);
}

static bool decode_png(const uint8_t *data, size_t len, int fit_w, int fit_h, muse_image_t *out, char *err,
                       size_t err_cap)
{
    png_src_t src = { .data = data, .len = len, .err = err, .err_cap = err_cap };
    png_structp png = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, &src, png_fail, png_warn, NULL, png_mem,
                                               png_mem_free);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        say(err, err_cap, "out of memory");
        return false;
    }
    /* What setjmp's return leaves to free: volatile, so a longjmp keeps them. */
    uint8_t *volatile row = NULL;
    uint8_t *volatile whole = NULL;
    png_bytep *volatile rows = NULL;
    shrink_t s = { 0 };
    if (setjmp(png_jmpbuf(png))) {
        big_free(row);
        big_free(whole);
        big_free(rows);
        shrink_free(&s, false);
        png_destroy_read_struct(&png, &info, NULL);
        return false;
    }
    png_set_read_fn(png, &src, png_in);
    png_set_user_limits(png, MUSE_IMAGE_MAX_SIDE, MUSE_IMAGE_MAX_SIDE);
    png_read_info(png, info);
    int w = (int)png_get_image_width(png, info), h = (int)png_get_image_height(png, info);
    int type = png_get_color_type(png, info);
    if ((int64_t)w * h > MUSE_IMAGE_MAX_PIXELS) {
        png_error(png, "too many pixels");
    }
    /* Everything to 8-bit RGBA. */
    png_set_expand(png);
    png_set_strip_16(png);
    png_set_gray_to_rgb(png);
    if (!(type & PNG_COLOR_MASK_ALPHA) && !png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    }
    int passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);
    if (png_get_rowbytes(png, info) != (size_t)w * 4) {
        png_error(png, "unexpected row layout");
    }
    int ow, oh;
    fit_in(w, h, fit_w, fit_h, &ow, &oh);
    if (!shrink_init(&s, w, h, ow, oh)) {
        png_error(png, "out of memory");
    }
    if (passes > 1) {
        /* Interlaced: each pass fills in rows the last left, so all of it at once. */
        if ((int64_t)w * h > MUSE_IMAGE_INTERLACED_MAX) {
            png_error(png, "interlaced and too big");
        }
        whole = big_alloc((size_t)w * h * 4);
        rows = big_alloc((size_t)h * sizeof(png_bytep));
        if (!whole || !rows) {
            png_error(png, "out of memory");
        }
        for (int y = 0; y < h; y++) {
            rows[y] = whole + (size_t)y * w * 4;
        }
        png_read_image(png, rows);
        for (int y = 0; y < h; y++) {
            shrink_row(&s, rows[y], y);
        }
    } else {
        row = big_alloc((size_t)w * 4);
        if (!row) {
            png_error(png, "out of memory");
        }
        for (int y = 0; y < h; y++) {
            png_read_row(png, row, NULL);
            shrink_row(&s, row, y);
        }
    }
    shrink_flush(&s);
    big_free(row);
    big_free(whole);
    big_free(rows);
    shrink_free(&s, true);
    png_destroy_read_struct(&png, &info, NULL);
    *out = (muse_image_t){ .px = s.out, .w = ow, .h = oh, .src_w = w, .src_h = h };
    return true;
}
#endif

/* ---- WebP ----------------------------------------------------------------- */

static bool decode_webp(const uint8_t *data, size_t len, int fit_w, int fit_h, muse_image_t *out, char *err,
                        size_t err_cap)
{
    WebPDecoderConfig cfg;
    if (!WebPInitDecoderConfig(&cfg)) {
        say(err, err_cap, "WebP decoder version mismatch");
        return false;
    }
    if (WebPGetFeatures(data, len, &cfg.input) != VP8_STATUS_OK) {
        say(err, err_cap, "not a WebP it can read");
        return false;
    }
    int w = cfg.input.width, h = cfg.input.height;
    if (cfg.input.has_animation) {
        say(err, err_cap, "an animated WebP");
        return false;
    }
    if (w > MUSE_IMAGE_MAX_SIDE || h > MUSE_IMAGE_MAX_SIDE || (int64_t)w * h > MUSE_IMAGE_MAX_PIXELS) {
        say(err, err_cap, "too many pixels");
        return false;
    }
    int ow, oh;
    fit_in(w, h, fit_w, fit_h, &ow, &oh);
    if (ow != w || oh != h) {
        cfg.options.use_scaling = 1;   /* scaled while it decodes: never whole at full size */
        cfg.options.scaled_width = ow;
        cfg.options.scaled_height = oh;
    }
    uint16_t *px = big_alloc((size_t)ow * oh * sizeof(uint16_t));
    uint8_t *rgba = cfg.input.has_alpha ? big_alloc((size_t)ow * oh * 4) : NULL;
    if (!px || (cfg.input.has_alpha && !rgba)) {
        big_free(px);
        big_free(rgba);
        say(err, err_cap, "out of memory");
        return false;
    }
    cfg.output.is_external_memory = 1;
    if (rgba) {
        cfg.output.colorspace = MODE_RGBA;
        cfg.output.u.RGBA.rgba = rgba;
        cfg.output.u.RGBA.stride = ow * 4;
        cfg.output.u.RGBA.size = (size_t)ow * oh * 4;
    } else {
        /* Straight to RGB565, in LVGL's byte order (WEBP_SWAP_16BIT_CSP, components/libwebp). */
        cfg.output.colorspace = MODE_RGB_565;
        cfg.output.u.RGBA.rgba = (uint8_t *)px;
        cfg.output.u.RGBA.stride = ow * 2;
        cfg.output.u.RGBA.size = (size_t)ow * oh * 2;
    }
    VP8StatusCode st = WebPDecode(data, len, &cfg);
    WebPFreeDecBuffer(&cfg.output);
    if (st != VP8_STATUS_OK) {
        static const char *const WHY[] = { "ok", "out of memory", "bad parameter", "damaged",
                                           "unsupported WebP", "suspended", "cancelled", "cut short" };
        say(err, err_cap, (unsigned)st < sizeof(WHY) / sizeof(WHY[0]) ? WHY[st] : "WebP decode failed");
        big_free(px);
        big_free(rgba);
        return false;
    }
    if (rgba) {
        for (size_t i = 0; i < (size_t)ow * oh; i++) {
            const uint8_t *p = rgba + i * 4;
            uint32_t al = p[3], white = 255 * (255 - al);
            px[i] = rgb565((p[0] * al + white) / 255, (p[1] * al + white) / 255, (p[2] * al + white) / 255);
        }
        big_free(rgba);
    }
    *out = (muse_image_t){ .px = px, .w = ow, .h = oh, .src_w = w, .src_h = h };
    return true;
}

bool muse_image_decode(const uint8_t *data, size_t len, int fit_w, int fit_h, muse_image_t *out, char *err,
                       size_t err_cap)
{
    memset(out, 0, sizeof(*out));
    if (fit_w <= 0 || fit_h <= 0) {
        say(err, err_cap, "no box to fit it in");
        return false;
    }
    switch (muse_image_kind(data, len)) {
#if MUSE_IMAGE_WITH_PNG
    case MUSE_IMAGE_PNG:
        return decode_png(data, len, fit_w, fit_h, out, err, err_cap);
#endif
    case MUSE_IMAGE_WEBP:
        return decode_webp(data, len, fit_w, fit_h, out, err, err_cap);
    default:
        say(err, err_cap, "a format this can't decode");
        return false;
    }
}
