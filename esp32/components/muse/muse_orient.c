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
 * Which quarter turn is up (muse_orient.h), from the IMU's smoothed gravity
 * (muse_imu_gravity), once the board has read a new one for HOLD_S.
 */
#include "muse_orient.h"

#include <math.h>

#include "esp_log.h"

#include "muse_gadget_mode.h"
#include "muse_imu.h"

static const char *TAG = "orient";

/*
 * The chip axis along the screen's vertical, and its sign when the board is
 * the right way up (keys on top, USB-C below): FLIP_SIGN * g[FLIP_AXIS] is
 * +1 g stood upright and -1 g stood on its keys. Taken from the vendor's
 * 04_Immersive_block example (waveshareteam/ESP32-S3-Touch-AMOLED-2.16,
 * examples/esp-idf), which runs the BSP's screen orientation, as Muse does,
 * and rolls its balls down the screen by +accelY (and right by -accelX): so
 * +Y reads +1 g with the top edge up. Confirmed on the board: propped up the
 * right way, it read y=+0.76 g (z=+0.71 g, leaning back).
 */
#define FLIP_AXIS 1             /* 0 x, 1 y */
#define FLIP_SIGN 1             /* +1 or -1 */
/* The sign of the other in-plane axis when the board lies on the side that
 * should be turn 1 (muse_board_t.set_turn): unconfirmed, flip it if the
 * picture comes out upside down with the board on its side. */
#define SIDE_SIGN 1

/* Turns once gravity in the screen's plane is more than this; lying flat, it
 * stays as it is. */
#define TURN_G 0.35f
#define CHECK_S 0.25f
#define DIAGONAL_G 0.15f        /* this close to 45 degrees, it keeps the turn it has */
#define HOLD_S 1.0f             /* turned this long before the screen follows */

static int s_turn;              /* the quarter turn on screen */
static float s_next;
static int s_mode = -1;
static int s_want = -1;          /* the turn the board has read for a while, not yet taken */
static float s_want_at;

static const char *const TURN_NAMES[] = { "upright", "on its side (1)", "upside down", "on its side (3)" };

/* The quarter turn gravity points to within the screen's plane, however far
 * the board leans back; -1 lying flat, or near a diagonal (it keeps its turn). */
static int turn_of(const float g[3])
{
    float up = FLIP_SIGN * g[FLIP_AXIS];
    float side = SIDE_SIGN * g[1 - FLIP_AXIS];
    float a = fabsf(up), b = fabsf(side);
    if ((a > b ? a : b) < TURN_G || fabsf(a - b) < DIAGONAL_G) {
        return -1;
    }
    return a > b ? (up > 0 ? 0 : 2) : (side > 0 ? 1 : 3);
}

int muse_orient_turn(float now)
{
    if (now < s_next) {
        return s_turn;
    }
    s_next = now + CHECK_S;

    muse_gadget_mode_t mode = muse_gadget_mode();
    float g[3];
    bool known = muse_imu_gravity(g);
    int turn = s_turn;
    int want = known ? turn_of(g) : -1;
    if (want < 0 || want == s_turn) {
        s_want = -1;
    } else if (want != s_want) {
        s_want = want;
        s_want_at = now;
    } else if (now - s_want_at >= HOLD_S) {
        turn = want;
        s_want = -1;
    }
    if (turn != s_turn || (int)mode != s_mode) {
        if (known) {
            ESP_LOGI(TAG, "gravity x=%.2f y=%.2f z=%.2f -> %s (%s)", g[0], g[1], g[2], TURN_NAMES[turn],
                     muse_gadget_mode_name(mode));
        } else {
            ESP_LOGI(TAG, "gravity not known yet -> %s (%s)", TURN_NAMES[turn], muse_gadget_mode_name(mode));
        }
    }
    s_mode = (int)mode;
    s_turn = turn;
    return turn;
}
