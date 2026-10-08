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
 * Markdown images in a reply's text: `![red panda](sandbox://workspace/...)`.
 * Muse sometimes writes the image he's showing into the reply itself. The
 * caption has no use for it (the image shows on its own, muse_present.h), so
 * the chat session takes it out of the text as it arrives, keeping where it
 * pointed. A reply streams in pieces, so an image can be half here: the
 * caption stops short of it until the rest comes (muse_chat_shown_len).
 *
 * Header-only, for both the session (C++) and the caption code (C), and
 * their host tests.
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longer than this, an unfinished `![...` is just text. */
#define MUSE_CHAT_MD_PARTIAL_MAX 400

/* A reply image's workspace file and its alt text. */
typedef struct {
    char path[160];
    char label[48];
} muse_chat_image_t;

/* The workspace file a reference names: past "sandbox://", or past
 * "/media/raw/" in the URL the VM serves it at. Otherwise an http(s) URL
 * on the web, whole (Muse fetches it to push it: muse_present_ask). NULL
 * if none of those. */
static inline const char *muse_chat_image_file(const char *ref)
{
    if (!ref) {
        return NULL;
    }
    if (!strncmp(ref, "sandbox://", 10)) {
        return ref[10] ? ref + 10 : NULL;
    }
    const char *raw = strstr(ref, "/media/raw/");
    if (raw) {
        return raw[11] ? raw + 11 : NULL;
    }
    if ((!strncmp(ref, "https://", 8) && ref[8]) || (!strncmp(ref, "http://", 7) && ref[7])) {
        return ref;
    }
    return NULL;
}

/*
 * At p, a '!' before a '[': where a whole `![alt](target)` ends (past its
 * ')'), else NULL, with *partial set if the text ends inside what could
 * still become one. Neither part spans a line.
 */
static inline const char *muse_chat_md_image_end(const char *p, bool *partial)
{
    *partial = false;
    const char *q = p + 2;
    while (*q && *q != ']' && *q != '\n') {
        q++;
    }
    if (*q == '\n') {
        return NULL;
    }
    if (!*q || !q[1]) {
        *partial = q - p < MUSE_CHAT_MD_PARTIAL_MAX;
        return NULL;
    }
    if (q[1] != '(') {
        return NULL;
    }
    const char *r = q + 2;
    while (*r && *r != ')' && *r != '\n') {
        r++;
    }
    if (*r != ')') {
        *partial = !*r && r - p < MUSE_CHAT_MD_PARTIAL_MAX;
        return NULL;
    }
    return r + 1;
}

/* The image `![alt](target)` from p to end, in *img if its target names a workspace file. */
static inline bool muse_chat_md_image_take(const char *p, const char *end, muse_chat_image_t *img)
{
    const char *alt = p + 2, *close = strchr(alt, ']');
    const char *target = close + 2, *stop = end - 1;   /* the ')' */
    while (target < stop && (*target == ' ' || *target == '<')) {
        target++;
    }
    size_t n = 0;
    while (target + n < stop && target[n] != ' ' && target[n] != '>') {
        n++;   /* up to a title or the closing '>' */
    }
    char ref[sizeof(img->path) + 96];   /* room for a URL's scheme and host */
    if (n >= sizeof(ref)) {
        return false;
    }
    memcpy(ref, target, n);
    ref[n] = '\0';
    const char *file = muse_chat_image_file(ref);
    size_t f = file ? strlen(file) : sizeof(img->path);
    if (f >= sizeof(img->path)) {
        return false;   /* none, or cut short: no use */
    }
    memcpy(img->path, file, f + 1);
    size_t a = (size_t)(close - alt);
    if (!a) {
        strcpy(img->label, "image");
        return true;
    }
    if (a >= sizeof(img->label)) {
        a = sizeof(img->label) - 1;
        while (a && ((unsigned char)alt[a] & 0xC0) == 0x80) {
            a--;   /* whole characters only */
        }
    }
    memcpy(img->label, alt, a);
    img->label[a] = '\0';
    return true;
}

/* How much of `text` to show: all of it, or up to an image still arriving at its end. */
static inline size_t muse_chat_shown_len(const char *text)
{
    for (const char *p = strstr(text, "!["); p; p = strstr(p + 1, "![")) {
        bool partial;
        const char *end = muse_chat_md_image_end(p, &partial);
        if (partial) {
            return (size_t)(p - text);
        }
        if (end) {
            p = end - 1;
        }
    }
    return strlen(text);
}

/* The first whole image in `text` whose target is a workspace file, in *img. */
static inline bool muse_chat_first_image(const char *text, muse_chat_image_t *img)
{
    for (const char *p = strstr(text, "!["); p; p = strstr(p + 1, "![")) {
        bool partial;
        const char *end = muse_chat_md_image_end(p, &partial);
        if (end && muse_chat_md_image_take(p, end, img)) {
            return true;
        }
    }
    return false;
}

/*
 * Takes the whole images out of `text`, in place, with the doubled space or
 * the empty line each leaves. An image's gap can come in a later piece than
 * the image, so no run of spaces (past an indent) or of blank lines is left
 * anywhere, which captions and speech don't show anyway. The first image
 * naming a workspace file goes in *first, if given and still empty. An image
 * still arriving stays, for the rest to complete. Returns how many bytes went.
 */
static inline size_t muse_chat_strip_images(char *text, muse_chat_image_t *first)
{
    char *o = text;
    const char *p = text;
    while (*p) {
        bool partial = false;
        const char *end = p[0] == '!' && p[1] == '[' ? muse_chat_md_image_end(p, &partial) : NULL;
        if (end) {
            if (first && !first->path[0]) {
                muse_chat_md_image_take(p, end, first);
            }
            bool line_start = o == text || o[-1] == '\n';
            p = end;
            if (line_start || o[-1] == ' ') {
                while (*p == ' ') {
                    p++;
                }
            }
            if (line_start && *p == '\n') {
                p++;
            }
            continue;
        }
        bool blank_line = *p == '\n' && o - text >= 2 && o[-1] == '\n' && o[-2] == '\n';
        bool double_space = *p == ' ' && o - text >= 2 && o[-1] == ' ' && o[-2] != '\n';   /* not an indent */
        if (blank_line || double_space) {
            p++;   /* one an image left, or that arrived after it went */
            continue;
        }
        *o++ = *p++;
    }
    *o = '\0';
    return (size_t)(p - o);
}

#ifdef __cplusplus
}
#endif
