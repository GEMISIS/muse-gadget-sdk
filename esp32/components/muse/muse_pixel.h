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

typedef enum {
    MUSE_ACT_NONE,
    MUSE_ACT_PHONE_TALK,
    MUSE_ACT_PHONE_LISTEN,
    MUSE_ACT_PACKAGES,
    MUSE_ACT_UNBOX,
    MUSE_ACT_ASSEMBLE,
    MUSE_ACT_SEARCH,
    MUSE_ACT_PAINT,
    MUSE_ACT_CLOUD,
    MUSE_ACT_NEWS,
    MUSE_ACT_CALENDAR,
    MUSE_ACT_REMINDER,
    MUSE_ACT_MAIL,
    MUSE_ACT_CALC,
    MUSE_ACT_TOOLS,
    MUSE_ACT_WEATHER,
    MUSE_ACT_MAP,
    MUSE_ACT_MUSIC,
    MUSE_ACT_WRITE,
    MUSE_ACT_MEMORY,
    MUSE_ACT_RESPOND,
    MUSE_ACT_BROWSE,
    MUSE_ACT_COUNT,
} muse_act_t;

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
    /* The battery (muse_state_power), when `battery` says there's a reading:
     * battery_pct 0..100, and `charging` on USB power (charging, or full).
     * Run low (20% or less) and not charging, a battery badge beside Muse,
     * yellow, then red and blinking under 10%. `belly` (charging, or just
     * patted): the level on his belly, with a bolt going red to green as it
     * fills. A renderer may leave them alone. */
    bool battery;
    bool charging;
    bool belly;
    int battery_pct;
    /* 0..1: tired, as the battery runs down (muse_ui.c; 0 when charging, or
     * in bed). Idle only: heavy lids, a slower bob, a slouch, yawns, and at
     * the most, nodding off. A renderer may leave it alone. */
    float tired;
    /* No Wi-Fi: a little signal-with-a-slash badge on the floor at Muse's
     * right (the battery's is at his left). A renderer may leave it alone. */
    bool offline;
    /* What he's busy with in a turn, instead of words on the screen (muse_ui.c
     * picks it; act_t is seconds in it). PHONE_TALK: the note's going up, so
     * he's talking into a phone. PHONE_LISTEN: waiting on the answer, phone
     * to his ear (for an image too, till its bytes start coming). PACKAGES:
     * an image is downloading, and he's hauling boxes, act_progress (0..1,
     * or -1 when unknown) as how many have arrived. Then, all here, UNBOX:
     * he opens the boxes and the pieces of the picture fly out, act_progress
     * 0..1 through it. ASSEMBLE: the pieces fit together into a little framed
     * picture in his hands, act_progress 0..1 through it, held at 1 till the
     * photo's ready, then 1..2 tucking it into his pocket, which the photo
     * comes out of (reach). Each plays in full, however quick the image.
     * What Muse says he's at work on (muse_activity.h), each looping as long
     * as it lasts, its prop popping in and going in a puff: SEARCH, peering
     * about through a magnifying glass (act_progress -1); then, an image
     * found, 0..1 through a start at the glass, the picture in it, the photo
     * out of it held up, and tossed up into the cloud as PAINT's canvas is
     * (then CLOUD); NEWS, reading a newspaper; CALENDAR,
     * flipping a little wall calendar; REMINDER, writing sticky notes and
     * slapping them up (act_progress 1: crossing one out, crumpling it and
     * tossing it); MAIL, pulling letters out of a mail bag and reading them
     * in little glasses; CALC, tapping a calculator, sums flying off it;
     * TOOLS, rummaging in a toolbox for a wrench; WEATHER, under an umbrella,
     * a rain cloud and the sun by turns; MAP, puzzling over a map, turning
     * it round; MUSIC, headphones on, bopping, notes floating up; WRITE,
     * typing on a little laptop; MEMORY, a thought filed away in a cabinet;
     * RESPOND, done, turning to us, about to talk. PAINT, Muse is making
     * the image: in a beret at an easel, palette in one paw, dabbing the
     * picture onto the canvas with a brush (act_progress -1, as long as it
     * takes); then, made, 0..1 through a flourish and tossing the canvas up
     * into the cloud the boxes will come out of. CLOUD: that cloud up, its
     * arrow pulsing, and him waiting on it, tapping a foot, till the bytes
     * start coming (PACKAGES); talking, if he says the reply meanwhile.
     * BROWSE, Muse's browser at work on the web: at a little desk, a hand on the mouse, a laptop's browser window loading
     * pages in the site's colour (browse_site), scrolling them and clicking
     * through, his eyes on the pointer (act_progress -1, as long as it
     * runs); then, done, 0..1 through a cheer and shutting the lid. At
     * something at his side (the laptop, the easel, the calendar...), he
     * turns to it, three-quarters on, rather than posing with it at us; round
     * to us for a look now and then, when patted, and when it's done. A
     * renderer may leave them alone. */
    muse_act_t act;
    float act_t;
    float act_progress;
    /* Idle, time of day (muse_ui.c, from the clock): a cup of tea in the
     * morning, pajamas and a nightcap in the evening and on the Night face.
     * A renderer may leave them alone. */
    bool tea;
    bool pajamas;
    /* 0..1: the board's being shaken hard right now (muse_imu): he plants his
     * feet, arms out, and stands his ground. `dizzy` is the wobble after. A
     * renderer may leave it alone. */
    float brace;
    /* MUSE_ACT_BROWSE: the site's colour (0xRRGGBB) for its page's header
     * and the address bar's icon; 0 for none yet (starting up). */
    uint32_t browse_site;
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
