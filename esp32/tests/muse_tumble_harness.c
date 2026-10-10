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

/* The screen turning's timeline (muse_tumble.h), as the face steps it. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "muse_tumble.h"

#define FRAME 0.04f     /* the face's frame */
#define NEAR 0.35f      /* grid cells, or radians: no visible jump */

static int s_failed;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
            s_failed++;                                   \
        }                                                 \
    } while (0)

static float angle_gap(float a, float b)
{
    return fabsf(muse_tumble_wrap(a - b));
}

/* A pose seen in a screen turned `spin` further clockwise: the same on the panel. */
static muse_tumble_pose_t seen_turned(muse_tumble_pose_t p, float spin)
{
    p.tilt = muse_tumble_wrap(p.tilt - spin);
    muse_tumble_turn_pt(-spin, &p.x, &p.y);
    return p;
}

static bool same_place(const muse_tumble_pose_t *a, const muse_tumble_pose_t *b)
{
    return angle_gap(a->tilt, b->tilt) < NEAR && fabsf(a->x - b->x) < NEAR && fabsf(a->y - b->y) < NEAR;
}

static muse_tumble_frame_t step_at(const muse_tumble_t *t, float now)
{
    muse_tumble_t c = *t;
    return muse_tumble_step(&c, now);
}

/* A whole fall from standing, a quarter turn (or two) round. */
static void check_fall(int from, int quarters)
{
    int to = (from + quarters + 4) & 3;
    muse_tumble_t t;
    muse_tumble_start(&t, MUSE_TUMBLE_FALL, 10.0f, from, to, quarters, NULL);
    float spin = quarters * MUSE_TUMBLE_PI / 2;

    muse_tumble_frame_t f0 = step_at(&t, 10.0f);
    CHECK(f0.busy && !f0.turned && f0.props && f0.dim == 0, "starts lit, unturned (q %d)", quarters);
    CHECK(fabsf(f0.pose.tilt) < 1e-4f && fabsf(f0.pose.x - MUSE_TUMBLE_FEET_X) < 1e-3f
              && fabsf(f0.pose.y - MUSE_TUMBLE_FEET_Y) < 1e-3f,
          "starts standing where he stood (q %d): %.3f %.2f %.2f", quarters, f0.pose.tilt, f0.pose.x, f0.pose.y);

    /* Across the turn: the screen goes round by `spin`, and he stays put on the panel. */
    muse_tumble_frame_t before = step_at(&t, 10.0f + MUSE_TUMBLE_TURN_S - 1e-3f);
    muse_tumble_frame_t after = step_at(&t, 10.0f + MUSE_TUMBLE_TURN_S + 1e-3f);
    CHECK(!before.turned && after.turned, "turns at TURN (q %d)", quarters);
    CHECK(before.dim > 0.95f && after.dim > 0.95f, "dark as it turns (q %d): %.2f %.2f", quarters, before.dim, after.dim);
    muse_tumble_pose_t was = seen_turned(before.pose, spin);
    CHECK(same_place(&was, &after.pose), "lies still across the turn (q %d): %.2f %.2f %.2f -> %.2f %.2f %.2f", quarters,
          was.tilt, was.x, was.y, after.pose.tilt, after.pose.x, after.pose.y);
    /* Lying on his side on the floor, the turned screen's. */
    CHECK(fabsf(fabsf(after.pose.tilt) - MUSE_TUMBLE_PI / 2) < 1e-3f, "on his side (q %d): %.3f", quarters, after.pose.tilt);
    CHECK(fabsf(after.pose.y - MUSE_TUMBLE_LIE_Y) < 1.6f, "on the floor (q %d): %.2f", quarters, after.pose.y);
    CHECK(after.pose.floor, "his shadow on the floor once turned (q %d)", quarters);

    /* Frame by frame, as the face steps it. */
    int bumps = 0, boings = 0, jumps = 0;
    bool props_gone = false;
    muse_tumble_frame_t last = f0, f = f0;
    float now = 10.0f;
    for (int i = 1; i < 100; i++) {
        now = 10.0f + i * FRAME;
        f = muse_tumble_step(&t, now);
        bumps += f.bump;
        boings += f.boing;
        float s = now - 10.0f;
        CHECK(f.props == (s < MUSE_TUMBLE_LAND_S || s >= MUSE_TUMBLE_SET_S), "props at %.2f s (q %d)", s, quarters);
        props_gone |= !f.props;
        CHECK(f.dim >= 0 && f.dim <= 1, "dim in range");
        if (f.busy) {
            CHECK(f.pose.on, "on while busy");
            CHECK(f.pose.x > -1 && f.pose.x < 65 && f.pose.y > -1 && f.pose.y < 65, "in the grid at %.2f s: %.1f %.1f", s,
                  f.pose.x, f.pose.y);
            muse_tumble_pose_t prev = last.turned == f.turned ? last.pose : seen_turned(last.pose, spin);
            /* No frame's jump bigger than his fall's quickest (from the top, upside down). */
            float d = hypotf(f.pose.x - prev.x, f.pose.y - prev.y);
            jumps += d > 11.0f || angle_gap(f.pose.tilt, prev.tilt) > 0.8f;
        }
        if (s < MUSE_TUMBLE_DARK_S - 0.11f || s > MUSE_TUMBLE_BRIGHT_S) {
            CHECK(f.dim == 0, "lit at %.2f s (q %d): %.2f", s, quarters, f.dim);
        }
        last = f;
        if (!f.busy) {
            break;
        }
    }
    CHECK(!f.busy && f.turned && f.dim == 0 && f.props, "done, turned, lit (q %d)", quarters);
    CHECK(now - 10.0f < MUSE_TUMBLE_DONE_S + 2 * FRAME, "done in time (q %d): %.2f", quarters, now - 10.0f);
    CHECK(bumps == 1 && boings == 1, "one bump, one boing (q %d): %d %d", quarters, bumps, boings);
    CHECK(props_gone, "his things gone while he's down (q %d)", quarters);
    CHECK(jumps == 0, "no jumps (q %d): %d", quarters, jumps);

    /* Up again where he stood, the right way up. */
    muse_tumble_frame_t up = step_at(&t, 10.0f + MUSE_TUMBLE_SET_S);
    CHECK(fabsf(up.pose.tilt) < 1e-4f && fabsf(up.pose.x - MUSE_TUMBLE_FEET_X) < 1e-3f
              && fabsf(up.pose.y - MUSE_TUMBLE_FEET_Y) < 1e-3f,
          "back on his feet (q %d)", quarters);
}

/* A new turn partway through: on from wherever he'd got to. */
static void check_again(float at, int quarters2)
{
    muse_tumble_t t;
    muse_tumble_start(&t, MUSE_TUMBLE_FALL, 0, 0, 1, 1, NULL);
    muse_tumble_frame_t f = { 0 };
    for (float now = 0; now <= at + 1e-4f; now += FRAME) {
        f = muse_tumble_step(&t, now);
    }
    int on_screen = f.turned ? 1 : 0;
    int to = (on_screen + quarters2 + 4) & 3;
    muse_tumble_t t2;
    muse_tumble_start(&t2, MUSE_TUMBLE_FALL, at + FRAME, on_screen, to, quarters2, &f);
    muse_tumble_frame_t g = step_at(&t2, at + FRAME);
    CHECK(same_place(&f.pose, &g.pose), "again at %.2f s: from where he was: %.2f %.2f %.2f -> %.2f %.2f %.2f", at,
          f.pose.tilt, f.pose.x, f.pose.y, g.pose.tilt, g.pose.x, g.pose.y);
    float now = at + FRAME;
    for (int i = 0; i < 100 && g.busy; i++) {
        now += FRAME;
        g = muse_tumble_step(&t2, now);
    }
    CHECK(!g.busy && g.props, "again at %.2f s: done", at);
    muse_tumble_frame_t end = step_at(&t2, at + FRAME + MUSE_TUMBLE_SET_S);
    CHECK(fabsf(end.pose.tilt) < 1e-4f && fabsf(end.pose.y - MUSE_TUMBLE_FEET_Y) < 1e-3f, "again at %.2f s: up", at);
}

static void check_dip(void)
{
    muse_tumble_t t;
    muse_tumble_start(&t, MUSE_TUMBLE_DIP, 0, 0, 2, 2, NULL);
    bool turned = false;
    muse_tumble_frame_t f = { 0 };
    float now = 0;
    for (int i = 0; i < 40; i++, now += FRAME / 2) {
        f = muse_tumble_step(&t, now);
        CHECK(!f.pose.on, "no Muse in a dip");
        if (f.turned && !turned) {
            CHECK(f.dim > 0.99f, "dark as the dip turns: %.2f", f.dim);
        }
        turned |= f.turned;
        if (!f.busy) {
            break;
        }
    }
    CHECK(turned && !f.busy && f.dim == 0, "the dip's done, turned, lit");
    CHECK(now < MUSE_TUMBLE_DIP_DOWN_S + MUSE_TUMBLE_DIP_HOLD_S + MUSE_TUMBLE_DIP_UP_S + FRAME, "a short dip: %.2f", now);

    /* Started half dark: dark the sooner. */
    muse_tumble_frame_t half = { .dim = 0.5f };
    muse_tumble_start(&t, MUSE_TUMBLE_DIP, 0, 0, 1, 1, &half);
    f = step_at(&t, 0);
    CHECK(fabsf(f.dim - 0.5f) < 0.01f, "goes on from half dark: %.2f", f.dim);
    f = step_at(&t, MUSE_TUMBLE_DIP_DOWN_S * 0.5f + 1e-3f);
    CHECK(f.turned && f.dim > 0.99f, "turned the sooner");
}

/* Turned back before the screen went: a stumble, no turn, no dimming. */
static void check_back(void)
{
    muse_tumble_t t;
    muse_tumble_start(&t, MUSE_TUMBLE_FALL, 0, 0, 3, -1, NULL);
    muse_tumble_frame_t f = muse_tumble_step(&t, 0.3f);
    muse_tumble_start(&t, MUSE_TUMBLE_FALL, 0.3f, 0, 0, 0, &f);
    muse_tumble_frame_t g = step_at(&t, 0.3f);
    CHECK(same_place(&f.pose, &g.pose), "back: from where he was");
    for (float now = 0.3f; now < 3.0f; now += FRAME) {
        g = muse_tumble_step(&t, now);
        CHECK(g.dim == 0, "back: no dimming");
        CHECK(g.pose.floor || !g.busy, "back: the floor's the floor");
    }
    CHECK(!g.busy, "back: done");
}

/* Turned about every which way at random: always done once it's let be. */
static void check_restless(void)
{
    srand(7);
    for (int run = 0; run < 200; run++) {
        muse_tumble_t t = { 0 };
        muse_tumble_frame_t f = { .props = true };
        int on_screen = 0, going = 0;
        float now = 0;
        for (int i = 0; i < 400; i++, now += FRAME) {
            if (i < 200 && rand() % 13 == 0) {
                int to = rand() & 3;
                if (to != going) {
                    int q = ((to - on_screen) & 3) == 3 ? -1 : (to - on_screen) & 3;
                    muse_tumble_start(&t, rand() & 1 ? MUSE_TUMBLE_FALL : MUSE_TUMBLE_DIP, now, on_screen, to, q,
                                      t.kind != MUSE_TUMBLE_NONE ? &f : NULL);
                    going = to;
                }
            }
            if (t.kind == MUSE_TUMBLE_NONE) {
                continue;
            }
            f = muse_tumble_step(&t, now);
            CHECK(!isnan(f.pose.tilt) && !isnan(f.pose.x) && !isnan(f.pose.y) && !isnan(f.dim), "restless: a number");
            if (f.turned) {
                on_screen = t.to;
            }
            if (!f.busy) {
                t.kind = MUSE_TUMBLE_NONE;
                f = (muse_tumble_frame_t){ .props = true };
            }
        }
        CHECK(t.kind == MUSE_TUMBLE_NONE && on_screen == going, "restless run %d: done, on %d (going %d)", run, on_screen,
              going);
        if (s_failed) {
            return;
        }
    }
}

static void check_teeter(void)
{
    muse_tumble_pose_t p = muse_tumble_teeter(0, 1.0f);
    CHECK(!p.on, "no wobble, none on");
    p = muse_tumble_teeter(1, 1.0f);
    CHECK(p.on && p.whoa == 1 && fabsf(p.tilt) <= 0.13f && p.floor, "a wobble, standing");
}

int main(void)
{
    for (int from = 0; from < 4; from++) {
        check_fall(from, 1);
        check_fall(from, -1);
        check_fall(from, 2);
    }
    for (float at = 0.1f; at < MUSE_TUMBLE_DONE_S; at += 0.12f) {
        check_again(at, 1);
        check_again(at, 2);
        check_again(at, -1);
    }
    check_dip();
    check_back();
    check_restless();
    check_teeter();
    if (s_failed) {
        printf("%d failed\n", s_failed);
        return 1;
    }
    printf("PASS muse_tumble\n");
    return 0;
}
