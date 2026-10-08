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
#include <stdint.h>

#include "muse_state.h"

/*
 * Procedural pixel-art renderer for the Muse character.
 *
 * Muse is drawn on a coarse MUSE_PX_W x MUSE_PX_H grid with a small palette,
 * ordered dithering and hard outlines, then blown up to the screen with
 * nearest-neighbour blocks so the pixels stay chunky.
 */

#define MUSE_PX_W 64
#define MUSE_PX_H 64

typedef struct {
    muse_mode_t mode;
    float t;         /* seconds since boot */
    float mode_t;    /* seconds in current mode */
    float level;     /* 0..1 live audio level */
    float happy;     /* 0..1 pet reaction */
    /* The Night face (CONFIG_MUSE_GADGET_NIGHT_FACE): tucked up in bed, and
     * asleep there (eyes shut, slow breaths, z's) unless sitting up to listen
     * or answer. A renderer that doesn't draw a bed may leave both alone. */
    bool bed;
    bool sleepy;
    /* 0..1: shaken about by an earthquake (a shake of the board), easing
     * out after. Swirly eyes, a wobbly mouth, stars round the head. A
     * renderer may leave it alone. */
    float dizzy;
    /* Showing an image from a reply (muse_ui.c): 0..1 reaching down into a
     * pocket on the right-hand side of the body (the viewer's right) with
     * that arm, glancing down at it. Then `holding` it up: the UI draws the
     * arms (from the shoulders, grid (32 -/+ 14, 34)) and the photo, so the
     * renderer leaves its own arms out and looks pleased. The pocket shows
     * while either is on. A renderer may leave both alone. */
    float reach;
    bool holding;
    /* 0..1: just plugged in to charge (muse_ui_plugged), 1 at the plug and
     * easing to 0 over about two seconds. A happy hop with arms up, a whirl
     * of sparkles, and a lightning bolt popping up beside the head, glowing,
     * that flickers out. Idle only. A renderer may leave it alone. */
    float plugged;
} muse_pose_t;

/* Accent colour of a mode (for the surrounding UI), as 0xRRGGBB. */
uint32_t muse_pixel_accent(muse_mode_t mode);

/* Render one frame into Muse's own MUSE_PX_W x MUSE_PX_H grid. */
void muse_pixel_render(const muse_pose_t *pose);

/* Size (square, in screen pixels) muse_pixel_scale() blows the grid up to. */
void muse_pixel_set_size(int px);

/*
 * Write screen pixels [x0, x1] x [y0, y1] of the blown-up frame as RGB565,
 * stride_px apart. Cheap enough to call per display strip, so the full-size
 * image never has to exist in RAM.
 */
void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1);
