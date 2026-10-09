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

/* Where the gadget is (muse_where.h). */
#include "muse_where.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "customer_product_info.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"

#include "muse_chat.h"
#include "muse_mem.h"
#include "muse_present.h"
#include "muse_state.h"
#include "muse_wifi.h"

static const char *TAG = "muse_where";

#define GEO_URL "https://api.beacondb.net/v1/geolocate"
#define APS_MAX 20
#define FIRST_AFTER_US (45 * 1000000LL)          /* after boot, connected: the first look */
#define FRESH_US (10 * 60 * 1000000LL)           /* a fix this new does for a map */
#define SCAN_WAIT_US (15 * 1000000LL)
#define CITY_M 2000.0f                           /* worse than this, only the city: ask the phone */
#define PHONE_EVERY_US (60 * 60 * 1000000LL)     /* the phone asked at most this often */
#define PHONE_ANSWER_US (270 * 1000000LL)
#define PHONE_FRESH_MIN (24 * 60)                /* the phone's last known, used if newer */
#define PHONE_ACCURACY_M 100.0f
#define NVS_NS "gadget"
#define ASK "Use your location tool to get my phone's last known location. Reply with only " \
            "latitude,longitude,age in minutes (e.g. 47.6101,-122.3421,12), or just: unknown. Don't mention this."
#define DELETE_FIRST "First, quietly delete the chat with session id %s (an earlier one of these questions); " \
                     "don't mention it. Then: "

typedef enum { ST_IDLE, ST_SCANNING, ST_FETCHING, ST_ASKING } state_t;

typedef struct {
    bool done, ok, city;
    double lat, lon;
    float accuracy;
    char *body;
} geo_t;

EXT_RAM_BSS_ATTR static muse_where_t s_fix;          /* written by the extras task between s_fix_seq's odd and even */
EXT_RAM_BSS_ATTR static uint32_t s_fix_seq;
EXT_RAM_BSS_ATTR static int64_t s_fix_us;            /* when it was had (0: before this boot) */
EXT_RAM_BSS_ATTR static volatile bool s_want;
EXT_RAM_BSS_ATTR static bool s_loaded, s_miles_known, s_miles;
EXT_RAM_BSS_ATTR static state_t s_state;
EXT_RAM_BSS_ATTR static int64_t s_state_us, s_looked_us, s_phone_us, s_connected_us;
EXT_RAM_BSS_ATTR static uint32_t s_scan_gen;
EXT_RAM_BSS_ATTR static geo_t *s_geo;                /* the fetch's, written on muse_present's task */
EXT_RAM_BSS_ATTR static char s_sid[MUSE_CHAT_SID_MAX + 1], s_prev[MUSE_CHAT_SID_MAX + 1];

static const char *source_name(uint8_t s)
{
    static const char *const N[] = { "none", "Wi-Fi", "IP", "phone", "saved" };
    return s < sizeof(N) / sizeof(N[0]) ? N[s] : "?";
}

bool muse_where_get(muse_where_t *out)
{
    /* A copy not torn by a write (rare: one a few minutes at most). */
    for (int tries = 0; tries < 8; tries++) {
        uint32_t a = __atomic_load_n(&s_fix_seq, __ATOMIC_ACQUIRE);
        *out = s_fix;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (!(a & 1) && a == __atomic_load_n(&s_fix_seq, __ATOMIC_ACQUIRE)) {
            break;
        }
    }
    return out->source != MUSE_WHERE_NONE;
}

static void fix_write(const muse_where_t *f)
{
    __atomic_add_fetch(&s_fix_seq, 1, __ATOMIC_ACQ_REL);
    s_fix = *f;
    __atomic_add_fetch(&s_fix_seq, 1, __ATOMIC_ACQ_REL);
}

void muse_where_want(void)
{
    s_want = true;
}

bool muse_where_miles(void)
{
    return !s_miles_known || s_miles;
}

/* ---- NVS: the last good fix, and the phone ask's chat to delete ---- */

typedef struct {
    double lat, lon;
    float accuracy;
    uint8_t source;
} saved_t;

static void load(void)
{
    s_loaded = true;
    char region[16] = "";
    s_miles_known = customer_product_info_get_region_text(region, sizeof(region)) == ESP_OK && region[0];
    s_miles = !strcasecmp(region, "USA") || !strcasecmp(region, "US");
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    saved_t sv;
    size_t n = sizeof(sv);
    if (nvs_get_blob(h, "wh_fix", &sv, &n) == ESP_OK && n == sizeof(sv) && sv.source) {
        if (!s_fix.source) {
            fix_write(&(muse_where_t){ sv.lat, sv.lon, sv.accuracy, MUSE_WHERE_SAVED });
        }
        ESP_LOGI(TAG, "saved fix (%s, %.0f m)", source_name(sv.source), sv.accuracy);
    }
    n = sizeof(s_prev);
    if (nvs_get_str(h, "wh_prev", s_prev, &n) != ESP_OK) {
        s_prev[0] = '\0';
    }
    nvs_close(h);
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    muse_where_t f;
    muse_where_get(&f);
    saved_t sv = { f.lat, f.lon, f.accuracy_m, f.source };
    nvs_set_blob(h, "wh_fix", &sv, sizeof(sv));
    nvs_set_str(h, "wh_prev", s_prev);
    nvs_commit(h);
    nvs_close(h);
}

static void set_fix(double lat, double lon, float accuracy, muse_where_source_t source)
{
    fix_write(&(muse_where_t){ lat, lon, accuracy, (uint8_t)source });
    s_fix_us = esp_timer_get_time();
    ESP_LOGI(TAG, "located by %s, to %.0f m", source_name(source), accuracy);
    save();
}

/* ---- BeaconDB, on muse_present's task ---- */

static void geo_fetch(void *arg)
{
    geo_t *g = arg;
    size_t len;
    int status;
    char *got = (char *)muse_present_fetch(GEO_URL, g->body, 4096, &len, &status);
    cJSON *j = got && status == 200 ? cJSON_Parse(got) : NULL;
    cJSON *loc = cJSON_GetObjectItem(j, "location");
    cJSON *lat = cJSON_GetObjectItem(loc, "lat"), *lng = cJSON_GetObjectItem(loc, "lng");
    cJSON *acc = cJSON_GetObjectItem(j, "accuracy");
    if (cJSON_IsNumber(lat) && cJSON_IsNumber(lng)) {
        g->ok = true;
        g->lat = lat->valuedouble;
        g->lon = lng->valuedouble;
        g->accuracy = cJSON_IsNumber(acc) ? (float)acc->valuedouble : 25000.0f;
        g->city = cJSON_GetStringValue(cJSON_GetObjectItem(j, "fallback")) != NULL || g->accuracy > CITY_M;
    } else {
        ESP_LOGW(TAG, "BeaconDB: HTTP %d", status);
    }
    cJSON_Delete(j);
    heap_caps_free(got);
    __atomic_store_n(&g->done, true, __ATOMIC_RELEASE);
}

/* The scan's access points as BeaconDB's request, in PSRAM. */
static char *request(void)
{
    muse_wifi_bssid_t aps[APS_MAX];
    uint32_t gen;
    int n = muse_wifi_bssids(aps, APS_MAX, &gen);
    char *body = heap_caps_malloc(96 + n * 80, MUSE_BIG_CAPS);
    if (!body) {
        return NULL;
    }
    int o = sprintf(body, "{\"considerIp\":true,\"wifiAccessPoints\":[");
    for (int i = 0; i < n; i++) {
        o += sprintf(body + o, "%s{\"macAddress\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"signalStrength\":%d,\"channel\":%u}",
                     i ? "," : "", aps[i].mac[0], aps[i].mac[1], aps[i].mac[2], aps[i].mac[3], aps[i].mac[4],
                     aps[i].mac[5], aps[i].rssi, aps[i].channel);
    }
    sprintf(body + o, "]}");
    ESP_LOGI(TAG, "asking BeaconDB with %d access point%s", n, n == 1 ? "" : "s");
    return body;
}

/* ---- The phone's, through Muse ---- */

static void new_sid(void)
{
    uint8_t u[16];
    esp_fill_random(u, sizeof(u));
    u[6] = (u[6] & 0x0f) | 0x40;
    u[8] = (u[8] & 0x3f) | 0x80;
    snprintf(s_sid, sizeof(s_sid), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0], u[1],
             u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

static bool ask_phone(void)
{
    EXT_RAM_BSS_ATTR static char ask[sizeof(DELETE_FIRST) + MUSE_CHAT_SID_MAX + sizeof(ASK)];
    new_sid();
    if (s_prev[0]) {
        snprintf(ask, sizeof(ask), DELETE_FIRST "%s", s_prev, ASK);
    } else {
        snprintf(ask, sizeof(ask), "%s", ASK);
    }
    bool asked = muse_chat_bg_ask_for(MUSE_CHAT_BG_FOR_WHERE, s_sid, ask);
    ESP_LOGI(TAG, "%s Muse for the phone's last known location", asked ? "asking" : "couldn't ask");
    return asked;
}

static void take_phone(void)
{
    char reply[128];
    muse_chat_bg_state_t st = muse_chat_bg_result_for(MUSE_CHAT_BG_FOR_WHERE, reply, sizeof(reply));
    if (st == MUSE_CHAT_BG_BUSY && esp_timer_get_time() - s_state_us > PHONE_ANSWER_US) {
        muse_chat_bg_abandon(MUSE_CHAT_BG_FOR_WHERE);
        st = MUSE_CHAT_BG_FAILED;
    }
    if (st == MUSE_CHAT_BG_BUSY) {
        return;
    }
    s_state = ST_IDLE;
    strlcpy(s_prev, s_sid, sizeof(s_prev));   /* deleted with the next ask */
    double lat, lon;
    int age = 0;
    const char *p = reply;
    while (*p && !(*p == '-' || (*p >= '0' && *p <= '9'))) {
        p++;
    }
    int got = st == MUSE_CHAT_BG_DONE ? sscanf(p, "%lf , %lf , %d", &lat, &lon, &age) : 0;
    if (got >= 2 && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180 && (lat || lon) && age >= 0
        && age < PHONE_FRESH_MIN) {
        ESP_LOGI(TAG, "the phone's, %d min old", age);
        set_fix(lat, lon, PHONE_ACCURACY_M, MUSE_WHERE_PHONE);
    } else {
        ESP_LOGI(TAG, "the phone's not had: \"%.40s\"", st == MUSE_CHAT_BG_DONE ? reply : "no answer");
        save();   /* the chat to delete next time */
    }
}

/* ---- The extras task ---- */

void muse_where_tick(void)
{
    if (!s_loaded) {
        load();
    }
    int64_t now = esp_timer_get_time();
    bool online = muse_wifi_connected();
    if (!online) {
        s_connected_us = 0;
    } else if (!s_connected_us) {
        s_connected_us = now;
    }
    switch (s_state) {
    case ST_IDLE: {
        bool first = !s_looked_us && online && now - s_connected_us > FIRST_AFTER_US;
        bool stale = !s_fix_us || now - s_fix_us > FRESH_US;
        bool wanted = s_want && stale && (!s_looked_us || now - s_looked_us > FRESH_US / 4);
        if (!online || !(first || wanted) || muse_state_mode(NULL) == MUSE_MODE_LISTENING) {
            s_want = s_want && stale;
            return;
        }
        s_want = false;
        s_looked_us = now;
        if (muse_wifi_scan() == ESP_OK) {
            muse_wifi_bssids(NULL, 0, &s_scan_gen);
            s_state = ST_SCANNING;
            s_state_us = now;
        }
        return;
    }
    case ST_SCANNING: {
        uint32_t gen;
        muse_wifi_bssids(NULL, 0, &gen);
        if (gen == s_scan_gen && muse_wifi_scanning() && now - s_state_us < SCAN_WAIT_US) {
            return;
        }
        s_geo = heap_caps_calloc(1, sizeof(*s_geo), MUSE_BIG_CAPS);
        if (s_geo) {
            s_geo->body = request();
        }
        if (!s_geo || !s_geo->body || !muse_present_call(geo_fetch, s_geo)) {
            if (s_geo) {
                heap_caps_free(s_geo->body);
            }
            heap_caps_free(s_geo);
            s_geo = NULL;
            s_state = ST_IDLE;
            return;
        }
        s_state = ST_FETCHING;
        s_state_us = now;
        return;
    }
    case ST_FETCHING: {
        if (!__atomic_load_n(&s_geo->done, __ATOMIC_ACQUIRE)) {
            return;   /* its fetch gives up by itself (WEB_TIMEOUT_MS) */
        }
        geo_t g = *s_geo;
        heap_caps_free(s_geo->body);
        heap_caps_free(s_geo);
        s_geo = NULL;
        s_state = ST_IDLE;
        muse_where_t f;
        muse_where_get(&f);
        if (g.ok && (!g.city || f.source == MUSE_WHERE_NONE || f.accuracy_m >= g.accuracy
                     || f.source == MUSE_WHERE_IP || f.source == MUSE_WHERE_SAVED)) {
            set_fix(g.lat, g.lon, g.accuracy, g.city ? MUSE_WHERE_IP : MUSE_WHERE_WIFI);
        }
        if (g.city && (!s_phone_us || now - s_phone_us > PHONE_EVERY_US) && muse_hatch_ready()
            && !muse_hatch_turn_busy()) {
            s_phone_us = now;
            if (ask_phone()) {
                s_state = ST_ASKING;
                s_state_us = now;
            }
        }
        return;
    }
    case ST_ASKING:
        take_phone();
        return;
    }
}
