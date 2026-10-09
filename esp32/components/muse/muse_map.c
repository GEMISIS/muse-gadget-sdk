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

/* A map widget's arithmetic (muse_map.h). */
#include "muse_map.h"

#include <math.h>
#include <stdio.h>

#define EARTH_M 6371008.8
#define LAT_MAX 85.05112878

void muse_map_project(double lat, double lon, int z, double *x, double *y)
{
    lat = lat > LAT_MAX ? LAT_MAX : lat < -LAT_MAX ? -LAT_MAX : lat;
    double world = MUSE_MAP_TILE * pow(2.0, z);
    double s = sin(lat * M_PI / 180.0);
    *x = (lon + 180.0) / 360.0 * world;
    *y = (0.5 - log((1 + s) / (1 - s)) / (4 * M_PI)) * world;
}

int muse_map_fit(const double *lat, const double *lon, int n, int w, int h, int pad, int zmax, double *cx,
                 double *cy)
{
    zmax = zmax > MUSE_MAP_ZOOM_MAX ? MUSE_MAP_ZOOM_MAX : zmax < MUSE_MAP_ZOOM_MIN ? MUSE_MAP_ZOOM_MIN : zmax;
    int z = zmax;
    for (; z >= MUSE_MAP_ZOOM_MIN; z--) {
        double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
        for (int i = 0; i < n; i++) {
            double x, y;
            muse_map_project(lat[i], lon[i], z, &x, &y);
            x0 = x < x0 ? x : x0;
            x1 = x > x1 ? x : x1;
            y0 = y < y0 ? y : y0;
            y1 = y > y1 ? y : y1;
        }
        *cx = (x0 + x1) / 2;
        *cy = (y0 + y1) / 2;
        if (x1 - x0 <= w - 2 * pad && y1 - y0 <= h - 2 * pad) {
            return z;
        }
    }
    return MUSE_MAP_ZOOM_MIN;
}

double muse_map_metres(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * M_PI / 180, p2 = lat2 * M_PI / 180;
    double dp = p2 - p1, dl = (lon2 - lon1) * M_PI / 180;
    double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    return 2 * EARTH_M * atan2(sqrt(a), sqrt(1 - a));
}

const char *muse_map_compass(double lat1, double lon1, double lat2, double lon2)
{
    static const char *const DIRS[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    double p1 = lat1 * M_PI / 180, p2 = lat2 * M_PI / 180, dl = (lon2 - lon1) * M_PI / 180;
    double brg = atan2(sin(dl) * cos(p2), cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl)) * 180 / M_PI;
    brg = fmod(brg + 360.0, 360.0);
    return DIRS[(int)((brg + 22.5) / 45.0) % 8];
}

void muse_map_distance(char *dst, size_t cap, double metres, bool miles)
{
    if (miles) {
        double mi = metres / 1609.344;
        if (mi < 0.1) {
            int ft = (int)(metres * 3.28084 / 50 + 0.5) * 50;
            snprintf(dst, cap, "%d ft", ft < 50 ? 50 : ft);
        } else if (mi < 10) {
            snprintf(dst, cap, "%.1f mi", mi);
        } else {
            snprintf(dst, cap, "%d mi", (int)(mi + 0.5));
        }
    } else if (metres < 950) {
        int m = (int)(metres / 10 + 0.5) * 10;
        snprintf(dst, cap, "%d m", m < 10 ? 10 : m);
    } else if (metres < 10000) {
        snprintf(dst, cap, "%.1f km", metres / 1000);
    } else {
        snprintf(dst, cap, "%d km", (int)(metres / 1000 + 0.5));
    }
}
