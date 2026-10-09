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
 * Whether a push labelled `label` is one the gadget wants now: the image it
 * asked Muse for, or the reply named, or any while a turn waits on one, or
 * the last shown's late sharper copy. Muse retries old pushes, and they'd
 * show in place of the one asked for. If not, *why says so, for Muse.
 */
bool muse_present_push_ok(const char *label, char *why, size_t cap);
/* The ended image requests' chats to delete ("id, id"), handed over once: 0 if none. Any task. */
size_t muse_present_stale_take(char *out, size_t cap);

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

/* Muse is to push an image he was asked for (muse_present_ask: a workspace
 * file, or a web image that couldn't be fetched here), and none of it's come
 * yet: he's writing it out, a minute or more (the face's cloud, waiting). A
 * web image still to be fetched here, or being, isn't. Any task. */
bool muse_present_pushing(void);
/* A web image named (an https URL in a reply) to be fetched here, or being:
 * Muse found it (the face's "found it!", up into the cloud). Not once it's
 * gone over to Muse pushing it. Any task. */
bool muse_present_found(void);

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
 * Other work for this task, which fetches over HTTPS with its stack in PSRAM
 * (no flash, no NVS): a map's tiles, a product's picture, where the gadget
 * is (muse_widget_map.c, muse_where.c). fn(arg) runs on it, after the image
 * in hand if there is one. False if it couldn't be queued (busy: try again).
 * Any task.
 */
bool muse_present_call(void (*fn)(void *arg), void *arg);

/*
 * On that task only (from a muse_present_call): GETs `url`, or POSTs `body`
 * (JSON) to it, with no token or cookie, into PSRAM: at most `max` bytes, or
 * NULL (and why in the log) if it couldn't. *status gets the HTTP status.
 * heap_caps_free what it returns.
 */
uint8_t *muse_present_fetch(const char *url, const char *body, size_t max, size_t *len, int *status);

/*
 * ">fetch=URL": the web image fetched and shown as a reply's would be (its
 * CDN's smaller copy first), with no turn, each phase timed and printed as
 * "@fetch {...}" (DNS, TCP, TLS, waiting, the body, decoding and showing).
 * A leading "+" shows it as the sharper copy of the one held (its sharpening).
 * False if it isn't an http(s) URL or the task is busy. Any task.
 */
bool muse_present_bench_fetch(const char *url);

/*
 * A JPEG (progressive too, given the PSRAM: muse_jpeg.h), or with
 * CONFIG_MUSE_PRESENT_FORMATS a PNG or WebP, decoded
 * to RGB565 in PSRAM fitting fit_w x fit_h (keeping its shape, never
 * enlarged): *px (heap_caps_free it), *w, *h. False if it couldn't be.
 */
bool muse_present_decode(const uint8_t *data, size_t len, int fit_w, int fit_h, uint16_t **px, int *w, int *h);

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
