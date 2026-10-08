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
 * Upside down or not (muse_orient.h), from the gravity the IMU saw when the
 * board last lay still for 1.5 s (muse_imu_gravity), so a hand turning it
 * over changes nothing until it's put down.
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
 * +Y reads +1 g with the top edge up. Unconfirmed on hardware. The log line
 * on each change gives the reading to check it by: stood upright, the axis
 * named here should read about +1 g (or -1 g if FLIP_SIGN should be -1). If
 * it's x that does, set FLIP_AXIS to 0.
 */
#define FLIP_AXIS 1             /* 0 x, 1 y */
#define FLIP_SIGN 1             /* +1 or -1 */
#define FACE_AXIS 2             /* z: out of the screen, so lying flat */

/* Turns once that axis reads more than this, either way, and more than the
 * face axis; in between (tilted, lying flat) it stays as it is. */
#define TURN_G 0.6f
#define CHECK_S 0.25f

static bool s_flipped;
static float s_next;
static int s_mode = -1;

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
        float up = FLIP_SIGN * g[FLIP_AXIS];
        if (fabsf(up) > TURN_G && fabsf(up) > fabsf(g[FACE_AXIS])) {
            flipped = up < 0;
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
