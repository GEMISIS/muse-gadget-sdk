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

#include "muse_tts.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    char *out;
    size_t cap, n;
    bool full;   /* something didn't fit: the text ends there */
} sink_t;

static void put(sink_t *s, const char *str, size_t len)
{
    if (s->n + len < s->cap) {
        memcpy(s->out + s->n, str, len);
        s->n += len;
    } else {
        s->full = true;
    }
}

static char last(const sink_t *s)
{
    return s->n ? s->out[s->n - 1] : '\0';
}

static void space(sink_t *s)
{
    char c = last(s);
    if (c && c != ' ') {
        put(s, " ", 1);
    }
}

/* A line break: a pause, so list items and headings aren't run together. */
static void pause(sink_t *s)
{
    while (last(s) == ' ') {
        s->n--;
    }
    char c = last(s);
    if (c && !strchr(".,;:!?", c)) {
        put(s, ".", 1);
    }
    space(s);
}

/* Past a line's indent and Markdown list marker. */
static const char *line_start(const char *p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if ((*p == '-' || *p == '+' || *p == '*') && p[1] == ' ') {
        p += 2;
    }
    return p;
}

static bool starts(const char *p, const char *prefix)
{
    return strncmp(p, prefix, strlen(prefix)) == 0;
}

/* The length of the UTF-8 sequence at p, or 1 for a stray byte. */
static size_t seq_len(const unsigned char *p)
{
    size_t n = *p >= 0xF0 ? 4 : *p >= 0xE0 ? 3 : *p >= 0xC0 ? 2 : 1;
    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            return 1;
        }
    }
    return n;
}

/* Where a Markdown image, `![alt](target)` on one line, ends (past its ')'); NULL if p isn't one. */
static const char *image_end(const char *p)
{
    if (p[0] != '!' || p[1] != '[') {
        return NULL;
    }
    const char *q = p + 2;
    while (*q && *q != ']' && *q != '\n') {
        q++;
    }
    if (q[0] != ']' || q[1] != '(') {
        return NULL;
    }
    for (q += 2; *q && *q != '\n'; q++) {
        if (*q == ')') {
            return q + 1;
        }
    }
    return NULL;
}

/*
 * A file in Muse's workspace, named on its own: sandbox://..., or a word
 * like workspace/dir/file.jpg (a "/" or "." past "workspace/", so prose
 * such as "workspace/desk" is still said).
 */
static bool workspace_path(const char *in, const char *p)
{
    if (starts(p, "sandbox://")) {
        return true;
    }
    if (p > in && !strchr(" \t\n(`\"'", p[-1])) {
        return false;
    }
    const char *q = starts(p, "workspace/") ? p + 10 : starts(p, "/workspace/") ? p + 11 : NULL;
    for (; q && *q && *q != ' ' && *q != '\n' && *q != '`' && *q != ')'; q++) {
        if ((*q == '/' || *q == '.') && q[1] && q[1] != ' ' && q[1] != '\n') {
            return true;
        }
    }
    return false;
}

size_t muse_tts_clean(const char *in, char *out, size_t cap)
{
    if (!cap) {
        return 0;
    }
    sink_t s = { out, cap, 0, false };
    const char *p = line_start(in);
    while (*p && !s.full) {
        unsigned char c = (unsigned char)*p;
        const char *image = image_end(p);
        if (image) {
            /* An image Muse shows: nothing to say. */
            p = image;
            space(&s);
            continue;
        }
        if (workspace_path(in, p)) {
            while (*p && *p != ' ' && *p != '\n' && *p != ')' && *p != '`') {
                p++;
            }
            space(&s);
            continue;
        }
        if (starts(p, "http://") || starts(p, "https://") || starts(p, "www.")) {
            while (*p && *p != ' ' && *p != '\n' && *p != ')') {
                p++;
            }
            space(&s);
            put(&s, "link", 4);
            continue;
        }
        if (c == ']' && p[1] == '(') {
            /* [text](url): just the text. */
            const char *end = strchr(p, ')');
            p = end ? end + 1 : p + 1;
            continue;
        }
        if (c == '\n' || c == '\r') {
            pause(&s);
            p = line_start(p + 1);
            continue;
        }
        if (c < 0x80) {
            if (strchr("*_`#~|>[]{}\\^", c)) {
                if (c == '_' || c == '|') {
                    space(&s);
                }
            } else if (c == ' ' || c == '\t') {
                space(&s);
            } else if (c >= 0x20) {
                char ch = (char)c;
                put(&s, &ch, 1);
            }
            p++;
            continue;
        }
        size_t n = seq_len((const unsigned char *)p);
        uint32_t cp = n == 2 ? ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu)
                    : n == 3 ? ((c & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu)
                             : 0;
        if (cp == 0x2018 || cp == 0x2019) {
            put(&s, "'", 1);
        } else if (cp == 0x201C || cp == 0x201D) {
            put(&s, "\"", 1);
        } else if (cp == 0x2013 || cp == 0x2014) {
            while (last(&s) == ' ') {
                s.n--;
            }
            put(&s, ",", 1);
            space(&s);
        } else if (cp == 0x2026) {
            put(&s, "...", 3);
        } else if (cp == 0xA0 || cp == 0x2022) {
            space(&s);
        } else if (cp >= 0xC0 && cp <= 0x24F) {
            put(&s, p, n);   /* accented Latin letters: Pico reads these */
        }
        /* Anything else (emoji, symbols, other scripts) Pico can't say. */
        p += n;
    }
    while (last(&s) == ' ') {
        s.n--;
    }
    out[s.n] = '\0';
    return s.n;
}
