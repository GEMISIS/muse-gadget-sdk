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
 * muse_jpeg.c on the host: muse_jpeg_harness IN OUT FIT_W FIT_H BUDGET MAX_EIGHTHS
 * prints "info W H COMPS SOF BASELINE" (or "info none"), then "ok W H SRC_W
 * SRC_H EIGHTHS PROGRESSIVE NEED MS" (OUT gets the RGB565 pixels, native byte
 * order) or "error WHY".
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "muse_jpeg.h"

int main(int argc, char **argv)
{
    if (argc != 7) {
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(n > 0 ? (size_t)n : 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        return 2;
    }
    fclose(f);
    muse_jpeg_info_t info;
    if (muse_jpeg_info(buf, (size_t)n, &info)) {
        printf("info %d %d %d %02x %d\n", info.w, info.h, info.comps, info.sof, muse_jpeg_is_baseline(&info));
    } else {
        printf("info none\n");
    }
    muse_jpeg_t img;
    char err[64];
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    bool ok = muse_jpeg_decode(buf, (size_t)n, atoi(argv[3]), atoi(argv[4]), (size_t)atol(argv[5]), atoi(argv[6]),
                               &img, err, sizeof(err));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    free(buf);
    if (!ok) {
        printf("error %s\n", err);
        return 0;
    }
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    FILE *o = fopen(argv[2], "wb");
    if (!o) {
        return 2;
    }
    fwrite(img.px, 2, (size_t)img.w * img.h, o);
    fclose(o);
    printf("ok %d %d %d %d %d %d %zu %.2f\n", img.w, img.h, img.src_w, img.src_h, img.eighths, img.progressive,
           img.need, ms);
    free(img.px);
    return 0;
}
