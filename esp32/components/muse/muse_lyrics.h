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
 * A reply's captions as lyrics (muse_lyrics_ui.h): the whole reply wrapped
 * as the pages are (muse_hatch_caption_at: words whole, CJK's rules), and
 * where the speech is in it, as a line and the words of it said so far.
 *
 * Speech doesn't go through text at an even pace: it holds a beat at a comma
 * and longer at a sentence's end. A byte of text takes a byte's time (so a
 * CJK character, three bytes, about what it takes to say), and punctuation
 * that ends a clause adds its pause after it. A message's speech, so far
 * through, has got as far through its weight; the caption follows that, so
 * it never runs ahead of the words. Pure C, for the host tests.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_LYRICS_PAUSE_COMMA 4   /* after , ; : and their CJK forms, in bytes' time */
#define MUSE_LYRICS_PAUSE_STOP 8    /* after . ! ? (a run of them: the last), a line break */

typedef struct {
    uint16_t start;   /* byte offset in the text */
    uint16_t len;
} muse_lyrics_line_t;

/* `text` wrapped to `cols` as the captions are: up to `max` lines; returns how many. */
int muse_lyrics_wrap(const char *text, int cols, muse_lyrics_line_t *lines, int max);

/* `at` below is how far the speech has got: the bytes of the text said. */

/* The line being said: the last one begun (the first before any is). */
int muse_lyrics_line_at(const muse_lyrics_line_t *lines, int n, size_t at);

/*
 * How much of `line` is lit: up to the end of the word being said (a word
 * lights as it's begun), 0 before the line, all of it after. *word: where
 * the last lit word starts, from the line's start. A CJK character is a
 * word of its own; closing punctuation goes with the word before it.
 */
size_t muse_lyrics_lit(const char *text, const muse_lyrics_line_t *line, size_t at, size_t *word);

/* The first `len` bytes of text's time: its bytes and its pauses. */
size_t muse_lyrics_weight(const char *text, size_t len);

/* How far the speech has got `w` into the first `len` bytes' time
 * (muse_lyrics_weight): held after punctuation while its pause lasts. */
size_t muse_lyrics_at(const char *text, size_t len, size_t w);

#ifdef __cplusplus
}
#endif
