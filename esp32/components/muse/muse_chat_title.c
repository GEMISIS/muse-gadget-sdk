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

/* A new chat's title: a real one, or a stand-in (muse_chat_title.h). */
#include "muse_chat_title.h"

#include <string.h>

#define NORM_MAX 96   /* longer than any name kept (MUSE_CHAT_NAME_MAX), so a long one is still read whole enough */

/* The whole title, any of these: what the Muse calls a chat it hasn't titled yet. */
static const char *const EXACT[] = {
    "new chat", "new conversation", "new session", "new thread", "chat", "conversation", "session", "thread",
    "untitled", "no title", "title", "voice note", "voice message", "voice memo", "voice recording", "audio",
    "audio file", "audio message", "audio note", "audio clip", "audio recording", "recording", "file", "attachment",
    "image", "photo", "generating", "loading", "thinking", "pending", "waiting", "transcription",
};

/* Anywhere in it: words about the voice note or the file, not what was asked. */
static const char *const WITHIN[] = {
    "processing audio", "processing the audio", "processing file", "processing the file", "processing voice",
    "processing your", "processing image", "processing attachment", "processing recording", "audio file",
    "voice note", "voice message", "voice memo", "voice recording", "audio message", "audio note", "audio clip",
    "audio recording", "audio transcription", "transcribe audio", "transcribe the audio", "transcribe voice",
    "transcribe recording", "transcribing audio", "analyzing audio", "analyzing the audio", "analyzing file",
    "uploaded file", "file upload", "session title", "untitled", "generating title", "generating a title",
    "title generating", "generate title", "generate session title",
};

/* At its start: the Muse at work, not a title. */
static const char *const STARTS[] = { "processing", "transcribing", "uploading", "generating title" };

/* A file's name: the voice note's, an upload's. */
static const char *const EXTS[] = {
    "wav", "mp3", "m4a", "aac", "ogg", "oga", "opus", "webm", "flac", "pcm", "amr", "caf", "3gp",
    "mp4", "mov", "jpg", "jpeg", "png", "gif", "webp", "heic", "pdf", "txt", "json", "bin",
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

/* Ends of a title that say nothing: spaces, quotes, brackets, dots ("Processing..."). */
static bool trim_char(const char *p)
{
    return strchr(" \t\r\n\"'.,:;!?()[]{}<>*_-", *p) != NULL;
}

/* An 8-4-4-4-12 UUID anywhere in s. */
static bool has_uuid(const char *s)
{
    static const int GROUPS[] = { 8, 4, 4, 4, 12 };
    for (; *s; s++) {
        const char *p = s;
        bool ok = true;
        for (int g = 0; ok && g < 5; g++) {
            for (int i = 0; ok && i < GROUPS[g]; i++) {
                ok = hex(*p++);
            }
            ok = ok && (g == 4 || *p++ == '-');
        }
        if (ok) {
            return true;
        }
    }
    return false;
}

/* One word, of letters, digits, '-' and '_', with digits all through it: an id
 * or a made-up file name ("audio_1712345678", "rec-20261010-0915"). */
static bool id_like(const char *s, size_t n)
{
    if (n < 12) {
        return false;
    }
    int digits = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            digits++;
        } else if (!(c >= 'a' && c <= 'z') && c != '-' && c != '_') {
            return false;
        }
    }
    return digits >= 6;
}

/* Ends in ".wav" and the like. */
static bool file_name(const char *s, size_t n)
{
    const char *dot = NULL;
    for (size_t i = 0; i < n; i++) {
        dot = s[i] == '.' ? s + i : s[i] == ' ' ? NULL : dot;
    }
    if (!dot || dot == s) {
        return false;
    }
    size_t ext = (size_t)(s + n - dot - 1);
    for (size_t i = 0; i < COUNT(EXTS); i++) {
        if (strlen(EXTS[i]) == ext && !strncmp(dot + 1, EXTS[i], ext)) {
            return true;
        }
    }
    return false;
}

bool muse_chat_title_placeholder(const char *title)
{
    if (!title) {
        return true;
    }
    /* Lower case, every run of spaces one space; "…" (U+2026) is dots. */
    char norm[NORM_MAX];
    size_t n = 0;
    for (const char *p = title; *p && n < sizeof(norm) - 1; p++) {
        if (!strncmp(p, "\xe2\x80\xa6", 3)) {
            norm[n++] = '.';
            p += 2;
            continue;
        }
        char c = *p == '\t' || *p == '\r' || *p == '\n' ? ' ' : lower(*p);
        if (c == ' ' && (n == 0 || norm[n - 1] == ' ')) {
            continue;
        }
        norm[n++] = c;
    }
    norm[n] = '\0';
    /* A file's name before its dot goes with the ends. */
    while (n && norm[n - 1] == ' ') {
        norm[--n] = '\0';
    }
    if (file_name(norm, n)) {
        return true;
    }
    const char *s = norm;
    while (*s && trim_char(s)) {
        s++;
    }
    while (n > (size_t)(s - norm) && trim_char(&norm[n - 1])) {
        norm[--n] = '\0';
    }
    n -= (size_t)(s - norm);
    if (!n) {
        return true;
    }
    for (size_t i = 0; i < COUNT(EXACT); i++) {
        if (!strcmp(s, EXACT[i])) {
            return true;
        }
    }
    for (size_t i = 0; i < COUNT(STARTS); i++) {
        if (!strncmp(s, STARTS[i], strlen(STARTS[i]))) {
            return true;
        }
    }
    for (size_t i = 0; i < COUNT(WITHIN); i++) {
        if (strstr(s, WITHIN[i])) {
            return true;
        }
    }
    return has_uuid(s) || id_like(s, n);
}

void muse_chat_title_excerpt(const char *words, char *out, size_t cap)
{
    if (!cap) {
        return;
    }
    out[0] = '\0';
    if (!words || cap < 2) {
        return;
    }
    size_t n = 0;
    bool cut = false;
    for (const char *p = words; *p; p++) {
        char c = (unsigned char)*p < 0x20 || *p == 0x7f ? ' ' : *p;   /* a name holds no control characters */
        if (c == ' ' && (n == 0 || out[n - 1] == ' ')) {
            continue;
        }
        if (n == cap - 1) {
            const char *q = p;
            while (*q && ((unsigned char)*q <= 0x20 || *q == 0x7f)) {
                q++;
            }
            cut = *q != '\0';   /* more words than room, not just spaces after */
            break;
        }
        out[n++] = c;
    }
    while (n && out[n - 1] == ' ') {
        n--;
    }
    out[n] = '\0';
    if (cut) {
        /* Room for "...", at the last word that ends far enough in, or mid-word in a long one. */
        size_t room = cap - 1 >= 3 ? cap - 1 - 3 : 0, at = room;
        while (at > room / 2 && out[at] != ' ') {
            at--;
        }
        at = out[at] == ' ' ? at : room;
        while (at && ((unsigned char)out[at] & 0xc0) == 0x80) {
            at--;   /* not inside a UTF-8 letter */
        }
        while (at && (out[at - 1] == ' ' || out[at - 1] == ',' || out[at - 1] == '.')) {
            at--;
        }
        if (!at) {
            out[0] = '\0';
            return;
        }
        memcpy(out + at, "...", 4);
    }
    if (out[0] >= 'a' && out[0] <= 'z') {
        out[0] = (char)(out[0] - 'a' + 'A');
    }
}
