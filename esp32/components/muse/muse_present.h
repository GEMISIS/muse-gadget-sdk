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

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Images Muse shows the user: pushed whole over Link's private session with
 * display.show_image (main/gadget_commands.c), decoded on a task of their
 * own, saved to the microSD card like display.draw_url's (muse_sd.h), and
 * handed to the face, where Muse takes the photo out of his pocket and holds
 * it up (muse_ui_present). Baseline JPEG only, up to MUSE_PRESENT_MAX bytes;
 * anything else is logged and dropped. One image at a time, a newer one
 * waiting behind it.
 *
 * An image in a chat reply (a delta.presentation event) only names a file in
 * Muse's workspace, which the VM won't serve to the gadget, so the chat
 * session asks for it to be pushed (muse_present_ask).
 */

#define MUSE_PRESENT_MAX (512 * 1024)

/* The image's bytes, all here: takes `data` (heap_caps_malloc'd) whatever
 * happens. False if it was dropped unshown (busy, or out of memory). Any task. */
bool muse_present_bytes(uint8_t *data, size_t len, const char *label);

/*
 * Asks Muse, in the background (muse_chat_bg_ask_for, in a chat of its own),
 * to push the image at `path` (a workspace file, or an http(s) URL Muse
 * downloads) with display.show_image: straight away, beside the turn that
 * named it, once no other background request is under way, giving up after
 * two minutes. Every mode's contract asks Muse to push it by itself in that
 * turn too (muse_gadget_mode.c): one pushed first calls off a request not
 * gone yet, and of two pushes while the request is under way only the first
 * shows. A newer image replaces one still waiting; one asked for in the last
 * ten minutes isn't asked for again. Nothing shows until the push is all
 * here. Any task.
 */
void muse_present_ask(const char *path, const char *label);

/* Counts the images handed to the face (muse_ui_present), which then has Muse
 * unbox it and take it out of his pocket (muse_present_up_seq once it's up).
 * Any task. */
uint32_t muse_present_seq(void);

/*
 * Waiting for an image (a reply's speech held for it): on starts the clock
 * muse_present_progress estimates by, off stops it. Any task.
 */
void muse_present_wait(bool on);

/* A display.show_image chunk taken: `received` bytes so far of `size` (0 if
 * not said). Any task. */
void muse_present_chunk(size_t received, size_t size);

/* How far the image waited for has got, 0-100, or -1 when none is waited
 * for. Any task. */
int muse_present_progress(void);

/*
 * Where an image has really got, for Muse to act it out (muse_ui.c):
 * WAITING, one waited for (muse_present_wait) with none of it here yet (Muse
 * finding it, making it, or writing out the push); FETCHING, its bytes
 * coming (a web fetch connected, or a push's chunks), *progress 0..1, or -1
 * not knowing its size; DECODING, all here and being decoded and sized
 * (*progress 1). A sharper copy coming after the one shown is none of them.
 * Any task.
 */
typedef enum {
    MUSE_PRESENT_NONE,
    MUSE_PRESENT_WAITING,
    MUSE_PRESENT_FETCHING,
    MUSE_PRESENT_DECODING,
} muse_present_phase_t;

muse_present_phase_t muse_present_phase(float *progress);

/* A preview's been shown and Muse is still to push the sharper copy he was
 * asked for (muse_present_ask): the face keeps it up for that. Any task. */
bool muse_present_sharper_pending(void);

/*
 * The face has the photo up in his hands (or never will, dropped unshown):
 * muse_present_up_seq moves, and a reply's speech held for the image goes
 * on. Later than muse_present_seq, by however long he takes to unbox it.
 * Any task.
 */
void muse_present_up(void);
uint32_t muse_present_up_seq(void);

/*
 * The estimate behind it: Muse writes the image out as base64 before any of
 * it arrives, about a minute, so until bytes come it eases towards 90% over
 * that, then follows the bytes; 99% until it's shown.
 */
#define MUSE_PRESENT_EXPECTED_US (60 * 1000000LL)
static inline int muse_present_estimate(int64_t waited_us, size_t received, size_t size)
{
    /* 90 * (1 - e^(-t/T)), T half the expected wait: 78% by then, 88% at twice it. */
    double x = waited_us > 0 ? (double)waited_us * 2 / MUSE_PRESENT_EXPECTED_US : 0;
    int p = (int)(90.0 * (1.0 - exp(-x)));
    if (received && size) {
        int got = (int)((uint64_t)received * 100 / size);
        p = got > p ? got : p;
    } else if (received) {
        p = p > 90 ? p : 90;
    }
    return p < 0 ? 0 : p > 99 ? 99 : p;
}

#ifdef __cplusplus
}
#endif
