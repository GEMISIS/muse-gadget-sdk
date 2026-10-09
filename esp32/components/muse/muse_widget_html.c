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
 * An HTML widget's page read for its controls (muse_widget_html, in
 * muse_widget.h). Not a browser: one pass over the tags, in order, for
 *
 *   - the question: the first words on the page ahead of any control;
 *   - text fields: <input> of a text-like type, <textarea>, each with its
 *     label (<label for>, aria-label, or the words just before it) and
 *     placeholder;
 *   - choices: <input type=checkbox|radio> with the words after them (or
 *     their value), <select>'s <option>s;
 *   - its button: the first <button> (or submit input) with words on it.
 *
 * A page whose choices are made by its script (Muse's usually are) has them
 * in a list of words in the script: the first array of two or more strings
 * (or of objects with a label, name, title or text) is taken, as checkboxes
 * if the script makes those, else as one-of options. The state bridge's own
 * scripts (data-hatch-widget-state-hydration) are passed over. Plain C, no
 * allocation, its working state in PSRAM: the chat session's task runs it,
 * one page at a time.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "muse_widget.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

#define FIELDS_MAX 6
#define WORDS 200            /* a run of the page's words, decoded */
#define SCRIPTS_MAX 6

typedef struct {
    char label[WORDS];
    char placeholder[MUSE_WIDGET_ROW_SUB];
    char id[32];
} field_t;

typedef struct {
    const char *at;
    size_t len;
} span_t;

typedef struct {
    muse_widget_t *w;
    char prompt[WORDS];
    char before[WORDS];          /* the words since the last control: the next field's label */
    char button[MUSE_WIDGET_BUTTON];
    field_t fields[FIELDS_MAX];
    int nfields;
    int checks, radios;          /* choices of each kind, in w->rows */
    int awaiting;                /* a choice's row whose words come next (-1 none) */
    span_t scripts[SCRIPTS_MAX];
    int nscripts;
    struct {                     /* <label for=...>'s words, for the field with that id */
        char id[32];
        char text[WORDS];
    } labels[4];
    int nlabels;
} page_t;

/* ---- Words ---- */

static void put_utf8(char *dst, size_t cap, size_t *n, unsigned cp)
{
    char b[4];
    int k;
    if (cp < 0x80) {
        b[0] = (char)cp;
        k = 1;
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | cp >> 6);
        b[1] = (char)(0x80 | (cp & 0x3F));
        k = 2;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | cp >> 12);
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        k = 3;
    } else {
        b[0] = (char)(0xF0 | cp >> 18);
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        k = 4;
    }
    if (*n + k < cap) {
        memcpy(dst + *n, b, k);
        *n += k;
    }
}

/* An entity at p ("&amp;"...): its character into dst, and how much of p it was (0: not one). */
static size_t entity(const char *p, const char *end, char *dst, size_t cap, size_t *n)
{
    static const struct {
        const char *name;
        unsigned cp;
    } NAMED[] = {
        { "amp", '&' },      { "lt", '<' },       { "gt", '>' },       { "quot", '"' },
        { "apos", '\'' },    { "nbsp", ' ' },     { "mdash", 0x2014 }, { "ndash", 0x2013 },
        { "hellip", 0x2026 }, { "rsquo", 0x2019 }, { "lsquo", 0x2018 }, { "ldquo", 0x201C },
        { "rdquo", 0x201D }, { "middot", 0xB7 },
    };
    const char *semi = memchr(p, ';', end - p < 12 ? end - p : 12);
    if (!semi) {
        return 0;
    }
    if (p[1] == '#') {
        unsigned cp = 0;
        bool hex = p[2] == 'x' || p[2] == 'X';
        for (const char *q = p + (hex ? 3 : 2); q < semi; q++) {
            int d = isdigit((unsigned char)*q) ? *q - '0' : hex && isxdigit((unsigned char)*q) ? tolower(*q) - 'a' + 10 : -1;
            if (d < 0) {
                return 0;
            }
            cp = cp * (hex ? 16 : 10) + (unsigned)d;
        }
        if (!cp || cp > 0x10FFFF) {
            return 0;
        }
        put_utf8(dst, cap, n, cp);
        return (size_t)(semi - p) + 1;
    }
    for (size_t i = 0; i < sizeof(NAMED) / sizeof(NAMED[0]); i++) {
        size_t k = strlen(NAMED[i].name);
        if ((size_t)(semi - p - 1) == k && !strncmp(p + 1, NAMED[i].name, k)) {
            put_utf8(dst, cap, n, NAMED[i].cp);
            return k + 2;
        }
    }
    return 0;
}

/* The words of p..end, entities decoded, spaces collapsed, trimmed, appended to dst. */
static void words_add(char *dst, size_t cap, const char *p, const char *end)
{
    size_t n = strlen(dst);
    bool space = n > 0;
    for (; p < end && n + 1 < cap; p++) {
        char c = *p;
        if (isspace((unsigned char)c)) {
            space = n > 0;
            continue;
        }
        if (space && n + 2 < cap) {
            dst[n++] = ' ';
        }
        space = false;
        size_t used = c == '&' ? entity(p, end, dst, cap, &n) : 0;
        if (used) {
            p += used - 1;
            if (n && dst[n - 1] == ' ') {
                n--;
                space = true;   /* an &nbsp; */
            }
            continue;
        }
        dst[n++] = c;
    }
    dst[n] = '\0';
}

static bool has_letter(const char *s)
{
    for (; *s; s++) {
        if (isalnum((unsigned char)*s) || (unsigned char)*s >= 0x80) {
            return true;
        }
    }
    return false;
}

/* ---- Tags ---- */

/* The value of attribute `name` in a tag's attributes (a..end) into dst; false if it hasn't one. */
static bool attr(const char *a, const char *end, const char *name, char *dst, size_t cap)
{
    size_t k = strlen(name);
    while (a < end) {
        while (a < end && (isspace((unsigned char)*a) || *a == '/')) {
            a++;
        }
        const char *n0 = a;
        while (a < end && !isspace((unsigned char)*a) && *a != '=' && *a != '>' && *a != '/') {
            a++;
        }
        size_t nlen = (size_t)(a - n0);
        if (!nlen) {
            a++;
            continue;
        }
        while (a < end && isspace((unsigned char)*a)) {
            a++;
        }
        const char *v0 = a, *v1 = a;
        if (a < end && *a == '=') {
            a++;
            while (a < end && isspace((unsigned char)*a)) {
                a++;
            }
            if (a < end && (*a == '"' || *a == '\'')) {
                char q = *a++;
                v0 = a;
                while (a < end && *a != q) {
                    a++;
                }
                v1 = a;
                a += a < end;
            } else {
                v0 = a;
                while (a < end && !isspace((unsigned char)*a) && *a != '>') {
                    a++;
                }
                v1 = a;
            }
        }
        if (nlen == k && !strncasecmp(n0, name, k)) {
            if (cap) {
                dst[0] = '\0';
                words_add(dst, cap, v0, v1);
            }
            return true;
        }
    }
    return false;
}

/* Past the closing tag `name` (e.g. "</script"), from p; end if there's none. */
static const char *past_close(const char *p, const char *end, const char *close)
{
    size_t k = strlen(close);
    for (; p + k <= end; p++) {
        if (*p == '<' && !strncasecmp(p, close, k)) {
            const char *gt = memchr(p, '>', end - p);
            return gt ? gt + 1 : end;
        }
    }
    return end;
}

/* The inner words of an element running from p to its closing `close` into dst; returns past it. */
static const char *inner(const char *p, const char *end, const char *close, char *dst, size_t cap)
{
    size_t k = strlen(close);
    const char *q = p;
    dst[0] = '\0';
    while (q < end) {
        const char *lt = memchr(q, '<', end - q);
        const char *stop = lt ? lt : end;
        words_add(dst, cap, q, stop);
        if (!lt) {
            return end;
        }
        if (!strncasecmp(lt, close, k)) {
            const char *gt = memchr(lt, '>', end - lt);
            return gt ? gt + 1 : end;
        }
        const char *gt = memchr(lt, '>', end - lt);
        if (!gt) {
            return end;
        }
        if (dst[0]) {
            words_add(dst, cap, " ", " ");
        }
        q = gt + 1;
    }
    return end;
}

static const char *const TEXT_TYPES[] = { "text", "search", "email", "number", "tel", "url",
                                          "date", "time", "datetime-local", "month", "week", NULL };

static bool text_type(const char *type)
{
    for (int i = 0; TEXT_TYPES[i]; i++) {
        if (!strcasecmp(type, TEXT_TYPES[i])) {
            return true;
        }
    }
    return false;
}

static void add_field(page_t *pg, const char *a, const char *end)
{
    if (pg->nfields >= FIELDS_MAX) {
        return;
    }
    field_t *f = &pg->fields[pg->nfields++];
    memset(f, 0, sizeof(*f));
    attr(a, end, "id", f->id, sizeof(f->id));
    attr(a, end, "placeholder", f->placeholder, sizeof(f->placeholder));
    if (!attr(a, end, "aria-label", f->label, sizeof(f->label)) || !f->label[0]) {
        for (int i = 0; i < pg->nlabels; i++) {
            if (f->id[0] && !strcmp(pg->labels[i].id, f->id)) {
                strlcpy(f->label, pg->labels[i].text, sizeof(f->label));
            }
        }
    }
    if (!f->label[0] && strcmp(pg->before, pg->prompt) != 0) {
        strlcpy(f->label, pg->before, sizeof(f->label));
    }
    if (!f->label[0]) {
        attr(a, end, "name", f->label, sizeof(f->label));
    }
    pg->before[0] = '\0';
}

/* A choice, its words so far (a checkbox's value, an option's text). */
static int add_choice(page_t *pg, bool check, const char *words)
{
    muse_widget_t *w = pg->w;
    if (w->count >= MUSE_WIDGET_ROWS_MAX) {
        return -1;
    }
    muse_widget_row_t *r = &w->rows[w->count];
    memset(r, 0, sizeof(*r));
    r->type = check ? MUSE_WIDGET_ROW_CHECK : MUSE_WIDGET_ROW_OPTION;
    strlcpy(r->send, words ? words : "", sizeof(r->send));
    *(check ? &pg->checks : &pg->radios) += 1;
    pg->before[0] = '\0';
    return w->count++;
}

/* Words on the page, outside any control. */
static void on_words(page_t *pg, const char *p, const char *end, bool in_label)
{
    char t[WORDS] = "";
    words_add(t, sizeof(t), p, end);
    if (!t[0] || !has_letter(t)) {
        return;
    }
    if (pg->awaiting >= 0) {
        /* A checkbox's or radio's words: after it, in its label. */
        muse_widget_row_t *r = &pg->w->rows[pg->awaiting];
        bool was_value = r->title[0] == '\0';
        if (was_value) {
            strlcpy(r->send, t, sizeof(r->send));
            strlcpy(r->title, "x", sizeof(r->title));   /* taken: the label's, not the value */
        }
        if (!in_label) {
            pg->awaiting = -1;
        }
        return;
    }
    if (!pg->prompt[0] && !pg->nfields && !pg->w->count) {
        strlcpy(pg->prompt, t, sizeof(pg->prompt));
    }
    strlcpy(pg->before, t, sizeof(pg->before));
}

static void read_page(page_t *pg, const char *html)
{
    const char *p = html, *end = html + strlen(html);
    bool in_label = false;
    char label_for[32] = "";
    while (p < end) {
        const char *lt = memchr(p, '<', end - p);
        if (!lt) {
            on_words(pg, p, end, in_label);
            break;
        }
        if (lt > p) {
            if (in_label && label_for[0] && pg->awaiting < 0 && pg->nlabels < 4) {
                char t[WORDS] = "";
                words_add(t, sizeof(t), p, lt);
                if (t[0]) {
                    strlcpy(pg->labels[pg->nlabels].id, label_for, sizeof(pg->labels[0].id));
                    strlcpy(pg->labels[pg->nlabels].text, t, sizeof(pg->labels[0].text));
                    pg->nlabels++;
                    label_for[0] = '\0';
                }
            }
            on_words(pg, p, lt, in_label);
        }
        if (!strncmp(lt, "<!--", 4)) {
            const char *c = strstr(lt + 4, "-->");
            p = c ? c + 3 : end;
            continue;
        }
        const char *gt = memchr(lt, '>', end - lt);
        if (!gt) {
            break;
        }
        const char *n0 = lt + 1;
        bool closing = *n0 == '/';
        n0 += closing;
        const char *n1 = n0;
        while (n1 < gt && (isalnum((unsigned char)*n1) || *n1 == '-')) {
            n1++;
        }
        char name[16] = "";
        size_t nl = (size_t)(n1 - n0) < sizeof(name) - 1 ? (size_t)(n1 - n0) : sizeof(name) - 1;
        for (size_t i = 0; i < nl; i++) {
            name[i] = (char)tolower((unsigned char)n0[i]);
        }
        p = gt + 1;
        if (closing) {
            if (!strcmp(name, "label")) {
                in_label = false;
                pg->awaiting = -1;
            }
            continue;
        }
        if (!strcmp(name, "script")) {
            const char *body = p;
            p = past_close(p, end, "</script");
            bool bridge = attr(n1, gt, "data-hatch-widget-state-hydration", NULL, 0);
            if (!bridge && pg->nscripts < SCRIPTS_MAX) {
                pg->scripts[pg->nscripts++] = (span_t){ body, (size_t)(p - body) };
            }
        } else if (!strcmp(name, "style") || !strcmp(name, "svg") || !strcmp(name, "noscript")) {
            char close[16];
            snprintf(close, sizeof(close), "</%s", name);
            p = past_close(p, end, close);
        } else if (!strcmp(name, "label")) {
            in_label = true;
            label_for[0] = '\0';
            attr(n1, gt, "for", label_for, sizeof(label_for));
        } else if (!strcmp(name, "input")) {
            char type[24] = "text";
            attr(n1, gt, "type", type, sizeof(type));
            if (text_type(type)) {
                add_field(pg, n1, gt);
            } else if (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio")) {
                char value[WORDS] = "";
                attr(n1, gt, "value", value, sizeof(value));
                if (!strcmp(value, "on")) {
                    value[0] = '\0';
                }
                pg->awaiting = add_choice(pg, !strcasecmp(type, "checkbox"), value);
            } else if ((!strcasecmp(type, "submit") || !strcasecmp(type, "button")) && !pg->button[0]) {
                attr(n1, gt, "value", pg->button, sizeof(pg->button));
            }
        } else if (!strcmp(name, "textarea")) {
            add_field(pg, n1, gt);
            p = past_close(p, end, "</textarea");
        } else if (!strcmp(name, "option")) {
            char t[WORDS];
            p = inner(p, end, "</option", t, sizeof(t));
            if (t[0] && has_letter(t)) {
                add_choice(pg, false, t);
            }
        } else if (!strcmp(name, "button")) {
            char t[WORDS];
            p = inner(p, end, "</button", t, sizeof(t));
            if (!pg->button[0] && t[0] && has_letter(t)) {
                strlcpy(pg->button, t, sizeof(pg->button));
            }
        }
    }
}

/* ---- The script's list of words ---- */

/* A JS string literal at p (a quote): its text into dst (escapes undone); returns past it, or NULL. */
static const char *js_string(const char *p, const char *end, char *dst, size_t cap)
{
    char q = *p++;
    size_t n = 0;
    while (p < end && *p != q) {
        if (*p == '\n') {
            return NULL;
        }
        if (*p == '\\' && p + 1 < end) {
            p++;
            unsigned cp = (unsigned char)*p;
            if (*p == 'n' || *p == 't') {
                cp = ' ';
            } else if (*p == 'u' && p + 4 < end) {
                char hex[5] = { p[1], p[2], p[3], p[4], 0 };
                cp = (unsigned)strtoul(hex, NULL, 16);
                p += 4;
            }
            put_utf8(dst, cap, &n, cp ? cp : ' ');
            p++;
            continue;
        }
        if (n + 1 < cap) {
            dst[n++] = *p;
        }
        p++;
    }
    dst[n < cap ? n : cap - 1] = '\0';
    return p < end ? p + 1 : NULL;
}

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

/* An object literal at p ("{"): its label, name, title or text string into dst; returns past it. */
static const char *js_object(const char *p, const char *end, char *dst, size_t cap)
{
    static const char *const KEYS[] = { "label", "name", "title", "text", NULL };
    int depth = 0, best = 99;
    dst[0] = '\0';
    for (; p < end; p++) {
        if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            if (--depth == 0) {
                return p + 1;
            }
        } else if (depth == 1 && (isalpha((unsigned char)*p) || *p == '"' || *p == '\'')) {
            /* key: value */
            char key[16] = "";
            const char *k = p;
            if (*p == '"' || *p == '\'') {
                k = js_string(p, end, key, sizeof(key));
                if (!k) {
                    return NULL;
                }
            } else {
                size_t n = 0;
                while (k < end && (isalnum((unsigned char)*k) || *k == '_') && n + 1 < sizeof(key)) {
                    key[n++] = *k++;
                }
                key[n] = '\0';
                while (k < end && (isalnum((unsigned char)*k) || *k == '_')) {
                    k++;
                }
            }
            k = skip_ws(k, end);
            if (k >= end || *k != ':') {
                p = k - 1 > p ? k - 1 : p;
                continue;
            }
            k = skip_ws(k + 1, end);
            if (k < end && (*k == '"' || *k == '\'')) {
                char v[WORDS];
                const char *after = js_string(k, end, v, sizeof(v));
                if (!after) {
                    return NULL;
                }
                for (int i = 0; KEYS[i]; i++) {
                    if (!strcmp(key, KEYS[i]) && i < best && v[0]) {
                        best = i;
                        strlcpy(dst, v, cap);
                    }
                }
                p = after - 1;
            } else {
                p = k - 1;
            }
        } else if (*p == '"' || *p == '\'') {
            char skip[8];
            const char *after = js_string(p, end, skip, sizeof(skip));
            if (!after) {
                return NULL;
            }
            p = after - 1;
        }
    }
    return NULL;
}

/* The first array of two or more strings (or labelled objects) in the script, into rows. */
static bool script_list(page_t *pg, const span_t *s, bool check)
{
    const char *end = s->at + s->len;
    for (const char *p = s->at; p < end; p++) {
        if (*p == '"' || *p == '\'') {
            char skip[8];
            const char *after = js_string(p, end, skip, sizeof(skip));
            p = after ? after - 1 : p;
            continue;
        }
        if (*p != '[') {
            continue;
        }
        const char *q = skip_ws(p + 1, end);
        if (q >= end || (*q != '"' && *q != '\'' && *q != '{')) {
            continue;
        }
        muse_widget_t *w = pg->w;
        int first = w->count;
        bool ok = true;
        while (ok && q < end) {
            char t[WORDS];
            const char *after = *q == '{' ? js_object(q, end, t, sizeof(t))
                                : (*q == '"' || *q == '\'') ? js_string(q, end, t, sizeof(t)) : NULL;
            if (!after || !t[0]) {
                ok = false;
                break;
            }
            add_choice(pg, check, t);
            q = skip_ws(after, end);
            if (q < end && *q == ',') {
                q = skip_ws(q + 1, end);
                if (q < end && *q == ']') {
                    break;   /* a trailing comma */
                }
                continue;
            }
            ok = q < end && *q == ']';
            break;
        }
        if (ok && w->count - first >= 2) {
            return true;
        }
        /* Not a list of words after all: none of it. */
        *(check ? &pg->checks : &pg->radios) -= w->count - first;
        w->count = (uint8_t)first;
    }
    return false;
}

static bool script_says(const page_t *pg, const char *what)
{
    for (int i = 0; i < pg->nscripts; i++) {
        size_t k = strlen(what);
        for (const char *p = pg->scripts[i].at; p + k <= pg->scripts[i].at + pg->scripts[i].len; p++) {
            if (!strncasecmp(p, what, k)) {
                return true;
            }
        }
    }
    return false;
}

/* ---- The answer's lead-in ---- */

static bool starts(const char *s, const char *with)
{
    return !strncasecmp(s, with, strlen(with));
}

void muse_widget_lead(char *dst, size_t cap, const char *prompt)
{
    char t[WORDS] = "";
    if (cap) {
        dst[0] = '\0';
    }
    if (!prompt || cap < 2) {
        return;
    }
    strlcpy(t, prompt, sizeof(t));
    char *paren = strchr(t, '(');
    if (paren) {
        *paren = '\0';
    }
    size_t n = strlen(t);
    while (n && (isspace((unsigned char)t[n - 1]) || strchr(":.!,", t[n - 1]))) {
        t[--n] = '\0';
    }
    if (!n || t[n - 1] == '?') {
        return;   /* a question: the choices answer it by themselves */
    }
    const char *s = t;
    static const char *const VERBS[] = { "please ", "pick ", "choose ", "select ", "tick ", "check ", "mark ",
                                         "tap ", "add ", NULL };
    static const char *const DETS[] = { "your ", "the ", "any ", "some ", "all ", "which ", NULL };
    bool verb = false;
    for (int again = 0; again < 2; again++) {
        for (int i = 0; VERBS[i]; i++) {
            if (starts(s, VERBS[i])) {
                s += strlen(VERBS[i]);
                verb = true;
            }
        }
    }
    if (!verb) {
        return;   /* not "pick your ...": no telling what it names */
    }
    for (int i = 0; DETS[i]; i++) {
        if (starts(s, DETS[i])) {
            s += strlen(DETS[i]);
        }
    }
    static const char *const TAILS[] = { " you want", " you like", " you'd like", " you would like", NULL };
    char out[WORDS];
    strlcpy(out, s, sizeof(out));
    n = strlen(out);
    for (int i = 0; TAILS[i]; i++) {
        size_t k = strlen(TAILS[i]);
        if (n > k && !strcasecmp(out + n - k, TAILS[i])) {
            out[n -= k] = '\0';
        }
    }
    if (!n || n >= cap || !has_letter(out)) {
        return;
    }
    out[0] = (char)toupper((unsigned char)out[0]);
    strlcpy(dst, out, cap);
}

/* ---- The page into the widget ---- */

EXT_RAM_BSS_ATTR static page_t pg;   /* ~3.5 KB: off the caller's stack, and out of internal RAM */

bool muse_widget_html(const char *html, muse_widget_t *w)
{
    if (!html || !w) {
        return false;
    }
    memset(&pg, 0, sizeof(pg));
    pg.w = w;
    pg.awaiting = -1;
    w->count = 0;
    read_page(&pg, html);
    /* A choice's row holds its exact words in send; its title, filled below. */
    for (int i = 0; i < w->count; i++) {
        w->rows[i].title[0] = '\0';
    }
    int keep = w->count;
    for (int i = 0, o = 0; i < keep; i++) {
        if (w->rows[i].send[0]) {
            w->rows[o++] = w->rows[i];
        } else {
            w->count--;
        }
    }
    if (!pg.nfields && !w->count) {
        bool check = script_says(&pg, "checkbox");
        for (int i = 0; i < pg.nscripts && !script_list(&pg, &pg.scripts[i], check); i++) {
        }
    }
    const char *prompt = pg.prompt[0] ? pg.prompt : pg.nfields ? pg.fields[0].label : "";
    muse_widget_text(w->title, sizeof(w->title), prompt, false);
    if (pg.nfields) {
        /* Text fields: the choices beside them (rare) aren't answered here. */
        w->kind = MUSE_WIDGET_FORM;
        w->count = 0;
        for (int i = 0; i < pg.nfields; i++) {
            muse_widget_row_t *r = &w->rows[w->count++];
            memset(r, 0, sizeof(*r));
            r->type = MUSE_WIDGET_ROW_FIELD;
            bool same = !strcmp(pg.fields[i].label, prompt);
            muse_widget_text(r->title, sizeof(r->title), same && pg.nfields == 1 ? "" : pg.fields[i].label, false);
            muse_widget_text(r->sub, sizeof(r->sub), pg.fields[i].placeholder, false);
            strlcpy(r->send, pg.fields[i].label, sizeof(r->send));   /* "Label: value" with more than one */
        }
        muse_widget_text(w->action, sizeof(w->action), pg.button[0] ? pg.button : "Send", false);
        return true;
    }
    if (!w->count) {
        return false;
    }
    bool checks = pg.checks > 0;
    w->kind = checks ? MUSE_WIDGET_CHECKS : MUSE_WIDGET_OPTIONS;
    for (int i = 0; i < w->count; i++) {
        muse_widget_row_t *r = &w->rows[i];
        r->type = checks ? MUSE_WIDGET_ROW_CHECK : MUSE_WIDGET_ROW_OPTION;
        muse_widget_text(r->title, sizeof(r->title), r->send, false);
    }
    if (checks) {
        muse_widget_text(w->action, sizeof(w->action), pg.button[0] ? pg.button : "Done", false);
        muse_widget_lead(w->lead, sizeof(w->lead), pg.prompt);
    }
    return true;
}
