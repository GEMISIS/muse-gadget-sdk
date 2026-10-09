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
 * A map widget's arithmetic (muse_widget_map.c): Web Mercator as the tile
 * servers draw it (256 px tiles, zoom z: the world 256 * 2^z px across),
 * the zoom and middle that fit some places in a view, and how far and which
 * way a place is from the user. Plain C, for host tests.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_MAP_TILE 256
#define MUSE_MAP_ZOOM_MIN 3
#define MUSE_MAP_ZOOM_MAX 17

/* lat, lon at zoom z: world pixels from the top left. */
void muse_map_project(double lat, double lon, int z, double *x, double *y);

/*
 * The highest zoom (at most zmax, at least MUSE_MAP_ZOOM_MIN) at which all n
 * points fit a w x h view with `pad` px clear at its edges; their middle in
 * that zoom's world pixels into *cx, *cy.
 */
int muse_map_fit(const double *lat, const double *lon, int n, int w, int h, int pad, int zmax, double *cx,
                 double *cy);

/* Metres between two points (great circle). */
double muse_map_metres(double lat1, double lon1, double lat2, double lon2);
/* Which way the second is from the first: "N", "NE", ... "NW". */
const char *muse_map_compass(double lat1, double lon1, double lat2, double lon2);
/* "0.4 mi", "800 ft", "1.2 km", "350 m": as far as people say it. */
void muse_map_distance(char *dst, size_t cap, double metres, bool miles);

#ifdef __cplusplus
}
#endif
