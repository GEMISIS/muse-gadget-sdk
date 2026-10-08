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
 * Replies spoken on the device (CONFIG_MUSE_TTS_PICO): SVOX Pico synthesizes
 * on a task of its own, into a buffer the chat session moves into the reply
 * audio as if it were a decoded MP3. 16 kHz mono, MUSE_AUDIO_RATE.
 *
 * One utterance at a time. muse_tts_say() and muse_tts_read() are called from
 * one task at a time: the chat session's during a turn, the voice task's for
 * a replay between turns.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_TTS_TEXT_MAX 2048   /* the most of a reply that's spoken, or kept to say again */

typedef enum {
    MUSE_TTS_SPEAKING,   /* more speech to come */
    MUSE_TTS_DONE,       /* all of it has been read */
    MUSE_TTS_FAILED,     /* the engine couldn't speak it */
} muse_tts_status_t;

/* Text as Pico should get it: no Markdown, links as "link", no emoji, line
 * breaks as pauses. Returns its length; out is always terminated. */
size_t muse_tts_clean(const char *in, char *out, size_t cap);

/* Maps the voice and starts the synthesis task. Call once, from a task whose
 * stack is in internal RAM. The engine itself loads on the first reply. */
void muse_tts_init(void);

/* Whether replies are spoken: the speaker setting is on and muse_gadget_tts_allowed() agrees. */
bool muse_tts_wanted(void);

/* Starts speaking text, ending anything still being said. False if it can't
 * (no voice partitions, nothing sayable, the engine stuck): show it instead. */
bool muse_tts_say(const char *text);

/* Up to frames of speech, without waiting. */
size_t muse_tts_read(int16_t *pcm, size_t frames);

/* Frames of speech ready to read. */
size_t muse_tts_buffered(void);

muse_tts_status_t muse_tts_status(void);

/* Stops synthesis; what's buffered is dropped by the next muse_tts_say(). */
void muse_tts_stop(void);

/* Keeps a reply's text for muse_tts_replay_last(): a turn's first message
 * replaces what was kept, later ones are added to it. */
void muse_tts_remember(const char *text, bool append);

/* Voice task, between turns: whether a replay was asked for, clearing the request. */
bool muse_tts_replay_take(void);

/* Copies the kept reply into out. False if there's none. */
bool muse_tts_last(char *out, size_t cap);

/* The caption page for a replay of text, played frames in. */
bool muse_tts_caption(const char *text, size_t played, char *out, size_t cap);

/*
 * Says the last reply again, between turns, if replies are spoken. Safe from
 * any task: the voice task picks it up within one idle pass (20 ms) and plays
 * it, and any button press stops it.
 */
void muse_tts_replay_last(void);

/* Whether a reply may be spoken now. The default always allows it; a gadget
 * mode can define its own (a strong definition replaces this weak one). */
bool muse_gadget_tts_allowed(void);

#ifdef __cplusplus
}
#endif
