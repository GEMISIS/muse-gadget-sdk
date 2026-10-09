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
#include <stdio.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The microSD card (CONFIG_MUSE_GADGET_SD), mounted at /sdcard by the extras
 * task (muse_extras.c) if a card is in the slot. Without one, or in builds
 * without it, everything here is a no-op and muse_sd_ready() is false.
 *
 * Files Muse keeps go under /sdcard/muse/:
 *   images/<YYYYMMDD-HHMMSS>.jpg|.raw  display.draw_url downloads, as sent
 *   captions.log                       what was heard and Muse's replies
 */

#define MUSE_SD_ROOT "/sdcard"
#define MUSE_SD_DIR MUSE_SD_ROOT "/muse"

typedef struct muse_sd_tee muse_sd_tee_t;

#if CONFIG_MUSE_GADGET_SD

/* Mounts the card; from the extras task (it can take a second without one). */
void muse_sd_mount(void);
bool muse_sd_ready(void);

/* Opens MUSE_SD_DIR "/" rel with fopen() mode `mode`, creating the
 * directories on the way. NULL without a card. */
FILE *muse_sd_fopen(const char *rel, const char *mode);
/* Creates MUSE_SD_DIR "/" rel and its parents; true if it exists. */
bool muse_sd_mkdirs(const char *rel);

/* The "sd_save_images" setting (NVS namespace "gadget"). */
bool muse_sd_save_images(void);
void muse_sd_set_save_images(bool on);

/*
 * Saving a download as it streams: begin() opens a temporary file (NULL when
 * saving is off or there's no card), write() takes every body byte in order,
 * end() renames it to images/<time>.<ext> on success or deletes it, and frees
 * the tee. Writes are batched through a small PSRAM buffer; a write error
 * just stops the saving.
 */
muse_sd_tee_t *muse_sd_tee_begin(void);
void muse_sd_tee_write(muse_sd_tee_t *t, const void *data, size_t len);
/* ext: "jpg" or "raw"; a raw image's name also gets its size, w x h. */
void muse_sd_tee_end(muse_sd_tee_t *t, bool ok, const char *ext, int w, int h);

#else

static inline void muse_sd_mount(void) {}
static inline bool muse_sd_ready(void) { return false; }
static inline FILE *muse_sd_fopen(const char *rel, const char *mode) { return NULL; }
static inline bool muse_sd_mkdirs(const char *rel) { return false; }
static inline bool muse_sd_save_images(void) { return false; }
static inline void muse_sd_set_save_images(bool on) {}
static inline muse_sd_tee_t *muse_sd_tee_begin(void) { return NULL; }
static inline void muse_sd_tee_write(muse_sd_tee_t *t, const void *data, size_t len) {}
static inline void muse_sd_tee_end(muse_sd_tee_t *t, bool ok, const char *ext, int w, int h) {}

#endif

/*
 * The caption log (CONFIG_MUSE_GADGET_CAPTION_LOG), fed from the voice task's
 * reply loop (muse_voice.c). These never touch the card themselves: they
 * collect the turn in PSRAM and queue its lines for the extras task to append.
 *
 * event(): a muse_hatch_ev_t and its text. What was heard is logged once the
 * turn is over; an error is logged as one.
 * page(): each caption page shown while the reply plays. Pages overlap by a
 * line (muse_hatch_caption_at); the reply's text is put back together from
 * them, so what's logged is what was shown.
 * flush(): the turn is over; queue its lines.
 */
#if CONFIG_MUSE_GADGET_CAPTION_LOG
void muse_sd_caption_event(int ev, const char *text);
void muse_sd_caption_page(const char *page);
void muse_sd_caption_flush(void);
/* From the extras task: appends the queued lines to captions.log. */
void muse_sd_caption_write(void);
#else
static inline void muse_sd_caption_event(int ev, const char *text) {}
static inline void muse_sd_caption_page(const char *page) {}
static inline void muse_sd_caption_flush(void) {}
static inline void muse_sd_caption_write(void) {}
#endif

#ifdef __cplusplus
}
#endif
