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
 * The screen turning with the board (muse_orient.h), as the face shows it
 * (muse_ui.c's update_flip): Muse knocked over by it, or, with something
 * else up, the screen dipping dark and back.
 *
 * FALL: the wobble (WHOA), and he goes, falling the way the board's turned
 * now, to land on his side on what's become the floor (LAND), anything he
 * was at going with him and gone in the dust. Lying there, dazed, the screen
 * dims, turns (TURN) and comes back, him lying just where he lay: it turns
 * by just as much as the board did, so he stays put. Then he gets up (UP to
 * STAND), dusts himself off, gives himself a shake (SHAKE), and sets it all
 * back (SET), pleased with himself till DONE. DIP: dark, turned, and back.
 *
 * Where he is goes in the turned screen's frame ("down" the way gravity is)
 * and comes out in the screen's as it is, so a new turn partway through
 * starts from wherever he's got to. Header-only, for the UI and its host test.
 */

#include <math.h>
#include <stdbool.h>

#include "muse_pixel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FALL, in seconds from its start. */
#define MUSE_TUMBLE_WHOA_S 0.16f     /* wobbling, then off he goes */
#define MUSE_TUMBLE_LAND_S 0.46f     /* on the floor */
#define MUSE_TUMBLE_DARK_S 0.50f     /* the screen dimming */
#define MUSE_TUMBLE_TURN_S 0.58f     /* dark: it turns */
#define MUSE_TUMBLE_LIT_S 0.62f      /* and comes back */
#define MUSE_TUMBLE_BRIGHT_S 0.76f
#define MUSE_TUMBLE_UP_S 0.86f       /* getting up */
#define MUSE_TUMBLE_STAND_S 1.18f    /* up, dusting himself off */
#define MUSE_TUMBLE_SHAKE_S 1.30f
#define MUSE_TUMBLE_SET_S 1.42f      /* what he was at, back */
#define MUSE_TUMBLE_DONE_S 1.66f
/* DIP. */
#define MUSE_TUMBLE_DIP_DOWN_S 0.12f
#define MUSE_TUMBLE_DIP_HOLD_S 0.05f   /* a frame or so, dark, for the redraw the new way round */
#define MUSE_TUMBLE_DIP_UP_S 0.16f

/* In Muse's grid: the middle (the screen turns about it), his feet standing,
 * and lying on his side on the floor, this far either side of the middle. */
#define MUSE_TUMBLE_MID 32.0f
#define MUSE_TUMBLE_FEET_X 32.0f
#define MUSE_TUMBLE_FEET_Y 57.0f
#define MUSE_TUMBLE_LIE_DX 23.0f
#define MUSE_TUMBLE_LIE_Y 42.0f
#define MUSE_TUMBLE_FLOOR_Y 58.0f

#define MUSE_TUMBLE_PI 3.14159265f

typedef enum { MUSE_TUMBLE_NONE, MUSE_TUMBLE_DIP, MUSE_TUMBLE_FALL } muse_tumble_kind_t;

typedef struct {
    muse_tumble_kind_t kind;
    float at, last;             /* when it began, and the last step */
    int from, to;               /* the quarter turn on screen as it began, and the one it's going to */
    float spin;                 /* radians the picture turns, clockwise, from `from` to `to` */
    float tilt0, x0, y0;        /* FALL: Muse as it began, in the turned frame */
    float whoa0;
    float lie;                  /* and his tilt lying down: a quarter turn either way */
    float dim0;                 /* how dark the screen was as it began */
} muse_tumble_t;

typedef struct {
    bool busy;                  /* still going */
    bool turned;                /* `to` on screen by now */
    bool props;                 /* what he was at shows (FALL: not from LAND till SET) */
    bool bump, boing;           /* this step: he hit the floor, or sprang up */
    float dim;                  /* 0..1, how far the screen's dimmed */
    muse_tumble_pose_t pose;    /* in the screen's frame as it is (`turned` or not) */
} muse_tumble_frame_t;

static inline float muse_tumble_seg(float x, float a, float b)
{
    float k = (x - a) / (b - a);
    return k < 0 ? 0 : k > 1 ? 1 : k;
}

static inline float muse_tumble_smooth(float k)
{
    return k * k * (3 - 2 * k);
}

/* An angle in (-pi, pi]. */
static inline float muse_tumble_wrap(float a)
{
    while (a > MUSE_TUMBLE_PI) {
        a -= 2 * MUSE_TUMBLE_PI;
    }
    while (a <= -MUSE_TUMBLE_PI) {
        a += 2 * MUSE_TUMBLE_PI;
    }
    return a;
}

/* (x, y) turned `a` radians clockwise (y down) about the grid's middle. */
static inline void muse_tumble_turn_pt(float a, float *x, float *y)
{
    float c = cosf(a), s = sinf(a), dx = *x - MUSE_TUMBLE_MID, dy = *y - MUSE_TUMBLE_MID;
    *x = MUSE_TUMBLE_MID + dx * c - dy * s;
    *y = MUSE_TUMBLE_MID + dx * s + dy * c;
}

/* The wobble before the screen turns (the board's turned, muse_orient.h
 * not sure of it yet): `whoa` 0..1, at `now`. */
static inline muse_tumble_pose_t muse_tumble_teeter(float whoa, float now)
{
    muse_tumble_pose_t p = {
        .on = whoa > 0,
        .floor = true,
        .tilt = whoa * 0.12f * sinf(now * 13.0f),
        .x = MUSE_TUMBLE_FEET_X,
        .y = MUSE_TUMBLE_FEET_Y,
        .whoa = whoa,
    };
    return p;
}

/*
 * Starts one at `now`, from the quarter turn on screen to `to`, the picture
 * turning `quarters` clockwise (-1..2) on the way; `was`, the last step's,
 * if one was going (or the teeter, in its pose), or NULL.
 */
static inline void muse_tumble_start(muse_tumble_t *t, muse_tumble_kind_t kind, float now, int from, int to,
                                     int quarters, const muse_tumble_frame_t *was)
{
    t->kind = kind;
    t->at = t->last = now;
    t->from = from;
    t->to = to;
    t->spin = quarters * (MUSE_TUMBLE_PI / 2);
    t->dim0 = was ? was->dim : 0;
    float tilt = 0, x = MUSE_TUMBLE_FEET_X, y = MUSE_TUMBLE_FEET_Y;
    t->whoa0 = 0;
    if (was && was->pose.on) {
        tilt = was->pose.tilt;
        x = was->pose.x;
        y = was->pose.y;
        t->whoa0 = was->pose.whoa;
    }
    muse_tumble_turn_pt(-t->spin, &x, &y);
    t->tilt0 = muse_tumble_wrap(tilt - t->spin);
    t->x0 = x;
    t->y0 = y;
    t->lie = t->tilt0 >= 0 ? MUSE_TUMBLE_PI / 2 : -MUSE_TUMBLE_PI / 2;   /* over the way he leans */
}

/* Where it's got to at `now`. */
static inline muse_tumble_frame_t muse_tumble_step(muse_tumble_t *t, float now)
{
    muse_tumble_frame_t f = { .props = true };
    float s = now - t->at, was = t->last - t->at;
    t->last = now;
    if (t->kind == MUSE_TUMBLE_DIP) {
        float down = MUSE_TUMBLE_DIP_DOWN_S * (1 - t->dim0), lit = down + MUSE_TUMBLE_DIP_HOLD_S;
        float k = s < down ? t->dim0 + s / MUSE_TUMBLE_DIP_DOWN_S : 1 - muse_tumble_seg(s, lit, lit + MUSE_TUMBLE_DIP_UP_S);
        f.dim = muse_tumble_smooth(k < 0 ? 0 : k > 1 ? 1 : k);
        f.turned = s >= down;
        f.busy = s < lit + MUSE_TUMBLE_DIP_UP_S;
        return f;
    }
    if (t->kind != MUSE_TUMBLE_FALL) {
        return f;
    }
    f.busy = s < MUSE_TUMBLE_DONE_S;
    f.turned = s >= MUSE_TUMBLE_TURN_S;
    f.props = s < MUSE_TUMBLE_LAND_S || s >= MUSE_TUMBLE_SET_S;
    f.bump = was < MUSE_TUMBLE_LAND_S && s >= MUSE_TUMBLE_LAND_S;
    f.boing = was < MUSE_TUMBLE_UP_S && s >= MUSE_TUMBLE_UP_S;
    if (t->from != t->to) {
        f.dim = s < MUSE_TUMBLE_DARK_S ? t->dim0 * (1 - muse_tumble_seg(s, 0, 0.1f))
                                       : muse_tumble_seg(s, MUSE_TUMBLE_DARK_S, MUSE_TUMBLE_TURN_S)
                                             * (1 - muse_tumble_seg(s, MUSE_TUMBLE_LIT_S, MUSE_TUMBLE_BRIGHT_S));
        f.dim = muse_tumble_smooth(f.dim);
    }

    /* In the turned frame. */
    muse_tumble_pose_t *p = &f.pose;
    p->on = f.busy;
    float lx = MUSE_TUMBLE_MID + (t->lie > 0 ? -MUSE_TUMBLE_LIE_DX : MUSE_TUMBLE_LIE_DX), ly = MUSE_TUMBLE_LIE_Y;
    float side = t->lie > 0 ? 1.0f : -1.0f;
    if (s < MUSE_TUMBLE_WHOA_S) {
        /* Whoa: a startled hop up off whatever he's on, and a wiggle. */
        float k = s / MUSE_TUMBLE_WHOA_S;
        float hop = 3.0f * sinf(MUSE_TUMBLE_PI * k);
        p->tilt = t->tilt0 + 0.14f * sinf(2 * MUSE_TUMBLE_PI * k);
        p->x = t->x0;
        p->y = t->y0 - hop;
        float in = muse_tumble_smooth(muse_tumble_seg(s, 0, 0.06f));
        p->whoa = t->whoa0 > in ? t->whoa0 : in;
    } else if (s < MUSE_TUMBLE_LAND_S) {
        /* Down he goes, faster and faster, over onto his side. */
        float k = muse_tumble_seg(s, MUSE_TUMBLE_WHOA_S, MUSE_TUMBLE_LAND_S), e = k * k;
        p->tilt = t->tilt0 + (t->lie - t->tilt0) * e;
        p->x = t->x0 + (lx - t->x0) * e;
        p->y = t->y0 + (ly - t->y0) * e;
        p->whoa = 1;
    } else if (s < MUSE_TUMBLE_UP_S) {
        /* Flat out, a bounce, and seeing stars. */
        float k = muse_tumble_seg(s, MUSE_TUMBLE_LAND_S, MUSE_TUMBLE_LAND_S + 0.22f);
        p->tilt = t->lie;
        p->x = lx;
        p->y = ly - 1.5f * sinf(MUSE_TUMBLE_PI * muse_tumble_seg(s, MUSE_TUMBLE_LAND_S + 0.05f, MUSE_TUMBLE_LAND_S + 0.22f));
        p->squash = -0.45f * (1 - k) * (1 - k);   /* flattened, along his length */
        p->whoa = 1 - muse_tumble_seg(s, MUSE_TUMBLE_LAND_S, MUSE_TUMBLE_LAND_S + 0.06f);
        p->daze = muse_tumble_seg(s, MUSE_TUMBLE_LAND_S + 0.08f, MUSE_TUMBLE_LAND_S + 0.2f);
        if (s < MUSE_TUMBLE_LAND_S + 0.36f) {
            p->dust = muse_tumble_seg(s, MUSE_TUMBLE_LAND_S, MUSE_TUMBLE_LAND_S + 0.36f);
            p->dust_x = MUSE_TUMBLE_MID;
            p->dust_y = MUSE_TUMBLE_FLOOR_Y;
        }
    } else if (s < MUSE_TUMBLE_STAND_S) {
        /* Up: a push off the floor, round onto his feet, rocking past and back. */
        float k = muse_tumble_seg(s, MUSE_TUMBLE_UP_S, MUSE_TUMBLE_STAND_S), e = muse_tumble_smooth(k);
        p->tilt = t->lie * (1 - e) - side * 0.12f * sinf(MUSE_TUMBLE_PI * muse_tumble_seg(k, 0.6f, 1));
        p->x = lx + (MUSE_TUMBLE_FEET_X - lx) * e;
        p->y = ly + (MUSE_TUMBLE_FEET_Y - ly) * e - 2.0f * sinf(MUSE_TUMBLE_PI * e);
        p->squash = 0.25f * sinf(MUSE_TUMBLE_PI * muse_tumble_seg(k, 0, 0.25f))
                    - 0.15f * sinf(MUSE_TUMBLE_PI * muse_tumble_seg(k, 0.3f, 0.75f));
        p->daze = 1 - muse_tumble_seg(s, MUSE_TUMBLE_UP_S - 0.04f, MUSE_TUMBLE_UP_S + 0.1f);
        p->up = muse_tumble_seg(s, MUSE_TUMBLE_UP_S - 0.04f, MUSE_TUMBLE_UP_S + 0.04f);
        p->dust = muse_tumble_seg(s, MUSE_TUMBLE_UP_S, MUSE_TUMBLE_UP_S + 0.3f);
        p->dust = p->dust < 1 ? p->dust : 0;
        p->dust_x = lx - side * 6.0f;   /* where his paws pushed */
        p->dust_y = MUSE_TUMBLE_FLOOR_Y;
    } else {
        /* On his feet: dusting off, a shake, and all set back. */
        p->tilt = 0;
        p->x = MUSE_TUMBLE_FEET_X;
        p->y = MUSE_TUMBLE_FEET_Y;
        p->up = 1 - muse_tumble_seg(s, MUSE_TUMBLE_STAND_S - 0.04f, MUSE_TUMBLE_STAND_S + 0.06f);
        p->pat = muse_tumble_seg(s, MUSE_TUMBLE_STAND_S, MUSE_TUMBLE_STAND_S + 0.04f)
                 * (1 - muse_tumble_seg(s, MUSE_TUMBLE_SHAKE_S - 0.02f, MUSE_TUMBLE_SHAKE_S + 0.02f));
        p->shake = sinf(MUSE_TUMBLE_PI * muse_tumble_seg(s, MUSE_TUMBLE_SHAKE_S, MUSE_TUMBLE_SET_S));
        p->there = s >= MUSE_TUMBLE_SET_S ? muse_tumble_seg(s, MUSE_TUMBLE_SET_S, MUSE_TUMBLE_DONE_S) : 0;
    }
    p->floor = f.turned || t->from == t->to;
    if (!f.turned) {
        /* Not turned yet: the screen as it was, the turned frame a quarter (or two) round from it. */
        p->tilt += t->spin;
        muse_tumble_turn_pt(t->spin, &p->x, &p->y);
        muse_tumble_turn_pt(t->spin, &p->dust_x, &p->dust_y);
    }
    p->tilt = muse_tumble_wrap(p->tilt);
    return f;
}

#ifdef __cplusplus
}
#endif
