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
 * Upside down or not (muse_orient.h), from the IMU's smoothed gravity
 * (muse_imu_gravity), once the board has read the other way up for HOLD_S.
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

/* Turns once that axis reads more than this the other way, and more than the
 * screen's other axis; tilted on its side or lying flat, it stays as it is. */
#define TURN_G 0.35f
#define CHECK_S 0.25f
#define HOLD_S 1.0f             /* turned the other way this long before the screen follows */

static bool s_flipped;
static float s_next;
static int s_mode = -1;
static float s_turned_at = -1;   /* when the board started reading the other way up */

bool muse_orient_flipped(float now)
{
    if (now < s_next) {
        return s_flipped;
    }
    s_next = now + CHECK_S;

    muse_gadget_mode_t mode = muse_gadget_mode();
    float g[3];
    bool known = muse_imu_gravity(g);
    bool flipped = s_flipped;
    if (mode != MUSE_GADGET_NIGHT && mode != MUSE_GADGET_ON_THE_GO) {
        flipped = false;   /* Desk: always upright */
    } else if (known) {
        /* Up or down within the screen's plane, however far it leans back:
         * propped on a stand, gravity splits about evenly with the face axis. */
        float up = FLIP_SIGN * g[FLIP_AXIS];
        float side = g[1 - FLIP_AXIS];
        if (fabsf(up) > TURN_G && fabsf(up) > fabsf(side) && (up < 0) != s_flipped) {
            if (s_turned_at < 0) {
                s_turned_at = now;
            }
            if (now - s_turned_at >= HOLD_S) {
                flipped = up < 0;
            }
        } else {
            s_turned_at = -1;
        }
    }
    if (flipped != s_flipped || (int)mode != s_mode) {
        if (known) {
            ESP_LOGI(TAG, "gravity x=%.2f y=%.2f z=%.2f -> %s (%s)", g[0], g[1], g[2],
                     flipped ? "flipped" : "upright", muse_gadget_mode_name(mode));
        } else {
            ESP_LOGI(TAG, "gravity not known yet -> %s (%s)", flipped ? "flipped" : "upright",
                     muse_gadget_mode_name(mode));
        }
    }
    s_mode = (int)mode;
    s_flipped = flipped;
    return flipped;
}
