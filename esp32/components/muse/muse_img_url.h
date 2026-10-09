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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A web image's smaller copy, for an image CDN that makes one from the URL
 * (muse_present.c fetches it first, the original if it fails): a full-size
 * photo is 200-800 KB at ~1 Mbit/s and seconds to decode, where a ~640 px
 * one is 30-80 KB. Known: WordPress (i0-i3.wp.com, *.files.wordpress.com,
 * /wp-content/uploads/), Wikimedia (/thumb/, 500px: one of its standard
 * steps), Unsplash and imgix, Cloudinary, Google (lh*.googleusercontent.com,
 * Blogger, ggpht), Shopify, Squarespace, Pinterest, Medium, Amazon, Fandom
 * and Imgur. Pure C, no allocation: tests/test_muse_img_url.py.
 */

#define MUSE_IMG_URL_PX 640   /* the width asked for: the 480 px screen, with room for a crop */

/* The smaller copy's URL in out (cap bytes) and true; false (out untouched)
 * if the host isn't one of those, the URL already asks for that little, or
 * it wouldn't fit. */
bool muse_img_url_smaller(const char *url, int px, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
