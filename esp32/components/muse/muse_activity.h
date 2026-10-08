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
 * What Muse says he's at work on in a turn, for the face to act out
 * (muse_pixel.h's acts). Every turn's agent.status goes "is working"
 * (activity_code "working"), then something in his own words ("Searching
 * news", "Checking calendar", "Generating image": the model picks them, so
 * they vary), then "is responding" (code "responding") while the answer
 * streams, and "online" once it's done. muse_activity_of() sorts the words
 * by keyword, at the start of a word and in any case; anything it doesn't
 * know is NONE, and Muse stays on the phone.
 *
 * Header-only, for the chat session (C++), the UI and console (C), and
 * their host test.
 */

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSE_ACTIVITY_NONE,             /* nothing said, or nothing known: on the phone */
    MUSE_ACTIVITY_SEARCH,
    MUSE_ACTIVITY_NEWS,
    MUSE_ACTIVITY_CALENDAR,
    MUSE_ACTIVITY_REMINDER,
    MUSE_ACTIVITY_REMINDER_CANCEL,
    MUSE_ACTIVITY_MAIL,
    MUSE_ACTIVITY_CALC,
    MUSE_ACTIVITY_TOOLS,
    MUSE_ACTIVITY_WEATHER,
    MUSE_ACTIVITY_MAP,
    MUSE_ACTIVITY_MUSIC,
    MUSE_ACTIVITY_WRITE,
    MUSE_ACTIVITY_MEMORY,
    MUSE_ACTIVITY_RESPOND,          /* the answer on its way */
    MUSE_ACTIVITY_IMAGE,            /* making an image */
    MUSE_ACTIVITY_IMAGE_MADE,       /* made one this turn, on to something else since (the session says) */
    MUSE_ACTIVITY_COUNT,
} muse_activity_t;

/* Its name, for the console (">activity=", ">face=act:"). */
static inline const char *muse_activity_name(muse_activity_t a)
{
    static const char *const NAMES[MUSE_ACTIVITY_COUNT] = {
        "none", "search", "news", "calendar", "reminder", "reminder_cancel", "mail", "calc", "tools", "weather",
        "map", "music", "write", "memory", "respond", "image", "image_made",
    };
    return a >= 0 && a < MUSE_ACTIVITY_COUNT ? NAMES[a] : "?";
}

/* Whether `text` has `word` (lower case) at the start of one of its words. */
static inline bool muse_activity_has(const char *text, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = text; *p; p++) {
        if (p != text && isalpha((unsigned char)p[-1])) {
            continue;
        }
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == word[i]) {
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return false;
}

/* Whether `text` has any of `words` (NULL-ended). */
static inline bool muse_activity_any(const char *text, const char *const *words)
{
    for (; *words; words++) {
        if (muse_activity_has(text, *words)) {
            return true;
        }
    }
    return false;
}

/*
 * What an agent.status says he's at: its activity_code and activity_text
 * (either may be NULL). In order, so "Searching news" is the news, "Drafting
 * an email" mail, "Searching images" a search and "Generating image" an image.
 */
static inline muse_activity_t muse_activity_of(const char *code, const char *text)
{
    if (code && !strcmp(code, "responding")) {
        return MUSE_ACTIVITY_RESPOND;
    }
    if (!text || !text[0]) {
        return MUSE_ACTIVITY_NONE;
    }
    static const char *const REMIND[] = { "remind", "alarm", "timer", NULL };
    static const char *const UNDO[] = { "cancel", "delet", "remov", "clear", "dismiss", "stop", NULL };
    static const struct {
        muse_activity_t what;
        const char *const words[8];
    } KEYS[] = {
        { MUSE_ACTIVITY_RESPOND, { "respond", NULL } },
        { MUSE_ACTIVITY_NEWS, { "news", "headline", NULL } },
        { MUSE_ACTIVITY_CALENDAR, { "calendar", "event", "schedul", "meeting", "agenda", "appointment", NULL } },
        { MUSE_ACTIVITY_MAIL, { "inbox", "email", "e-mail", "mail", "message", "letter", NULL } },
        { MUSE_ACTIVITY_WEATHER, { "weather", "forecast", "temperature", NULL } },
        { MUSE_ACTIVITY_MAP, { "map", "direction", "route", "navigat", "traffic", NULL } },
        { MUSE_ACTIVITY_MUSIC, { "music", "song", "playlist", "spotify", "play", NULL } },
        { MUSE_ACTIVITY_SEARCH, { "search", "research", "look", "find", "brows", NULL } },
        { MUSE_ACTIVITY_IMAGE, { "image", "picture", "photo", "draw", "paint", "illustrat", "sketch", NULL } },
        { MUSE_ACTIVITY_CALC, { "calculat", "math", "comput", "arithmet", NULL } },
        { MUSE_ACTIVITY_TOOLS, { "tool", NULL } },
        { MUSE_ACTIVITY_WRITE, { "writ", "draft", "document", "cod", "edit", NULL } },
        { MUSE_ACTIVITY_MEMORY, { "memor", "remember", "note", "saving", NULL } },
    };
    if (muse_activity_any(text, REMIND)) {
        return muse_activity_any(text, UNDO) ? MUSE_ACTIVITY_REMINDER_CANCEL : MUSE_ACTIVITY_REMINDER;
    }
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
        if (muse_activity_any(text, KEYS[i].words)) {
            return KEYS[i].what;
        }
    }
    return MUSE_ACTIVITY_NONE;
}

#ifdef __cplusplus
}
#endif
