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
 * Where the gadget is, for a map widget's "you" dot and its places'
 * distances (muse_widget_map.c). No GPS: in order,
 *
 *   1. Wi-Fi: the access points a scan sees (muse_wifi_bssids) sent to
 *      BeaconDB (api.beacondb.net, free, no key, MLS-compatible), which
 *      falls back to the public IP's city when it doesn't know them;
 *   2. the phone's last known location, asked of Muse in the background
 *      (its location tool) when Wi-Fi only gave a city, used if it's under
 *      a day old;
 *   3. the last good fix, kept in NVS ("gadget").
 *
 * It looks once after boot and again when a map wants it (muse_where_want)
 * and the fix is over ten minutes old. The scan is the settings' own (it
 * doesn't drop the connection). The fetch runs on muse_present's task;
 * the rest, and the NVS writes, on the extras task.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSE_WHERE_NONE,
    MUSE_WHERE_WIFI,     /* nearby access points */
    MUSE_WHERE_IP,       /* the public IP's city */
    MUSE_WHERE_PHONE,    /* the phone's last known, through Muse */
    MUSE_WHERE_SAVED,    /* the last good one, from before a restart */
} muse_where_source_t;

typedef struct {
    double lat, lon;
    float accuracy_m;    /* as said; a city's is kilometres */
    uint8_t source;      /* muse_where_source_t; NONE: nothing known */
} muse_where_t;

#if CONFIG_MUSE_HATCH && CONFIG_MUSE_GADGET_EXTRAS
/* The fix there is: false if none. Any task. */
bool muse_where_get(muse_where_t *out);
/* A map's up: look again if the fix is old. Any task. */
void muse_where_want(void);
/* Distances in miles (the gadget's region is the US), else km. */
bool muse_where_miles(void);
/* Once a second, from the extras task. */
void muse_where_tick(void);
#else
static inline bool muse_where_get(muse_where_t *out)
{
    out->source = MUSE_WHERE_NONE;
    return false;
}
static inline void muse_where_want(void) {}
static inline bool muse_where_miles(void) { return true; }
#endif

#ifdef __cplusplus
}
#endif
