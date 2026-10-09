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
 * One SVOX Pico engine, its voice read straight from the tts_ta and tts_sg
 * flash partitions. Not thread-safe: one task drives it. It speaks 16 kHz,
 * 16-bit mono.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PICOTTS_SAMPLE_RATE 16000
#define PICOTTS_WORK_BYTES 1100000   /* the engine's working memory, taken from PSRAM */

typedef enum {
    PICOTTS_IDLE = 0,    /* nothing more until more text */
    PICOTTS_BUSY = 1,    /* call again for more speech */
    PICOTTS_ERROR = -1,  /* reset or close the engine */
} picotts_step_t;

/* Maps the voice partitions. Call once, from a task whose stack is in internal RAM. */
bool picotts_engine_map(void);

/* Allocates the working memory and loads the voice. False (and closed) on failure. */
bool picotts_engine_open(void);
void picotts_engine_close(void);
bool picotts_engine_is_open(void);

/* Queues UTF-8 text; a '\0' among it makes the engine speak what it has. Returns the bytes taken, or -1. */
int picotts_engine_put(const char *utf8, size_t len);

/* Up to cap frames of speech into pcm; *frames says how many. */
picotts_step_t picotts_engine_get(int16_t *pcm, size_t cap, size_t *frames);

/* Drops queued text and unspoken speech. */
void picotts_engine_reset(void);

#ifdef __cplusplus
}
#endif
