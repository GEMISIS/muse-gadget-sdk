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

/* Muse's browser in a turn (muse_browse.h): its steps, and the turn's copy. */
#include "muse_browse.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *str(const cJSON *o, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, key));
    return s && s[0] ? s : NULL;
}

static void put(char *dst, size_t cap, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;   /* a whole character */
        }
    }
    memcpy(dst, src ? src : "", n);
    dst[n] = '\0';
}

void muse_browse_reset(muse_browse_t *b)
{
    b->task[0] = '\0';
    b->done = b->failed = false;
    b->started_ms = b->done_ms = 0;
    b->count = 0;
    b->turn++;
    b->seq++;
}

static bool finished(const char *status, const char *state)
{
    static const char *const ENDS[] = { "complete", "fail", "error", "cancel", "stop", "abort", NULL };
    for (int i = 0; ENDS[i]; i++) {
        if ((status && strcasestr(status, ENDS[i])) || (state && strcasestr(state, ENDS[i]))) {
            return true;
        }
    }
    return false;
}

/* "www.rei.com" -> "rei.com". */
static void site_of(char *dst, size_t cap, const char *domain, const char *url)
{
    const char *d = domain;
    char host[MUSE_BROWSE_SITE] = "";
    if (!d && url) {
        const char *p = strstr(url, "://");
        p = p ? p + 3 : url;
        size_t n = strcspn(p, "/:?#");
        n = n < sizeof(host) - 1 ? n : sizeof(host) - 1;
        memcpy(host, p, n);
        host[n] = '\0';
        d = host;
    }
    if (d && !strncasecmp(d, "www.", 4)) {
        d += 4;
    }
    put(dst, cap, d);
    for (char *c = dst; *c; c++) {
        *c = (char)tolower((unsigned char)*c);
    }
}

/* "Starting browser..." -> "Starting browser". */
static void what_of(char *dst, size_t cap, const char *what)
{
    put(dst, cap, what);
    size_t n = strlen(dst);
    for (;;) {
        if (n && (dst[n - 1] == '.' || dst[n - 1] == ' ')) {
            n--;
        } else if (n >= 3 && !memcmp(dst + n - 3, "\xE2\x80\xA6", 3)) {
            n -= 3;   /* an ellipsis */
        } else {
            break;
        }
    }
    dst[n] = '\0';
}

/* Whether the task's been seen before, and is this turn's: registers it if not. */
static bool ours(muse_browse_t *b, const char *id, bool ended)
{
    for (int i = 0; i < MUSE_BROWSE_TASKS; i++) {
        if (b->known[i].id[0] && !strcmp(b->known[i].id, id)) {
            return b->known[i].turn == b->turn && !b->known[i].stale;
        }
    }
    int k = b->known_next;
    b->known_next = (k + 1) % MUSE_BROWSE_TASKS;
    put(b->known[k].id, sizeof(b->known[k].id), id);
    b->known[k].turn = b->turn;
    b->known[k].stale = ended;   /* finished the first time it's seen: an old one replayed */
    return !ended;
}

bool muse_browse_apply(muse_browse_t *b, const cJSON *payload, int64_t now_ms)
{
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(payload, "data");
    data = cJSON_IsObject(data) ? data : payload;
    const char *id = str(data, "browser_task_id");
    id = id ? id : str(payload, "id");
    if (!id) {
        return false;
    }
    const char *status = str(data, "status"), *state = str(data, "display_state");
    bool ended = finished(status, state);
    if (!ours(b, id, ended)) {
        return false;
    }
    bool changed = false;
    if (strcmp(b->task, id) != 0) {
        /* The turn's first; or a newer one starting, or one after the last finished: the history goes on. */
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(data, "step_count");
        int steps = cJSON_IsNumber(n) ? (int)n->valuedouble : 0;
        bool starting = (state && strcasestr(state, "start")) || (status && strcasestr(status, "queue")) || steps <= 0;
        if (b->task[0] && !b->done && !starting) {
            return false;   /* another of the turn's, under way beside it: the one shown goes on */
        }
        put(b->task, sizeof(b->task), id);
        b->done = b->failed = false;
        b->started_ms = b->started_ms ? b->started_ms : now_ms;
        changed = true;
    }
    const cJSON *site = cJSON_GetObjectItemCaseSensitive(data, "current_site");
    const cJSON *tab = cJSON_GetObjectItemCaseSensitive(data, "latest_tab");
    const char *url = str(site, "url");
    url = url ? url : str(tab, "url");
    const char *title = str(site, "page_title");
    title = title ? title : str(tab, "title");
    muse_browse_step_t s = { .at_ms = now_ms };
    what_of(s.what, sizeof(s.what), str(data, "activity_title"));
    site_of(s.site, sizeof(s.site), str(site, "domain"), url);
    put(s.title, sizeof(s.title), title);
    put(s.url, sizeof(s.url), url);
    muse_browse_step_t *last = b->count ? &b->steps[b->count - 1] : NULL;
    /* A step a page: on the same one, only what it's at (and its title) change. Before any page
     * ("Starting browser", "Checking site"), the first page takes that step's place. */
    if (last && (!strcmp(last->url, s.url) || !last->url[0])) {
        bool to_page = !last->url[0] && s.url[0];
        if (to_page) {
            memcpy(last->site, s.site, sizeof(s.site));
            memcpy(last->url, s.url, sizeof(s.url));
            if (!s.what[0]) {
                strcpy(s.what, "Opened page");
            }
            changed = true;
        }
        if (s.what[0] && strcmp(last->what, s.what) != 0) {
            memcpy(last->what, s.what, sizeof(s.what));
            changed = true;
        }
        if (s.title[0] && strcmp(last->title, s.title) != 0) {
            memcpy(last->title, s.title, sizeof(s.title));
            changed = true;
        }
    } else {
        if (!s.what[0]) {
            strcpy(s.what, s.url[0] ? "Opened page" : "Working");
        }
        if (b->count == MUSE_BROWSE_STEPS) {
            memmove(&b->steps[0], &b->steps[1], sizeof(b->steps[0]) * (MUSE_BROWSE_STEPS - 1));
            b->count--;
        }
        b->steps[b->count++] = s;
        changed = true;
    }
    if (ended && !b->done) {
        b->done = true;
        b->failed = !(status && strcasestr(status, "complete")) && !(state && strcasestr(state, "complete"));
        b->done_ms = now_ms;
        changed = true;
    }
    if (changed) {
        b->seq++;
    }
    return changed;
}

uint32_t muse_browse_color(const char *site)
{
    /* A hue from the name; saturated, light enough to read on the dark card. */
    uint32_t h = 2166136261u;
    for (const char *p = site ? site : ""; *p && *p != '.'; p++) {
        h = (h ^ (unsigned char)*p) * 16777619u;
    }
    float hue = (float)(h % 360u) / 60.0f, s = 0.55f, v = 0.85f;
    int i = (int)hue;
    float f = hue - (float)i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    float r, g, bl;
    switch (i % 6) {
    case 0: r = v, g = t, bl = p; break;
    case 1: r = q, g = v, bl = p; break;
    case 2: r = p, g = v, bl = t; break;
    case 3: r = p, g = q, bl = v; break;
    case 4: r = t, g = p, bl = v; break;
    default: r = v, g = p, bl = q; break;
    }
    return (uint32_t)(r * 255) << 16 | (uint32_t)(g * 255) << 8 | (uint32_t)(bl * 255);
}

/* ---- The turn's ---- */

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"

static const char *TAG = "muse_browse";

EXT_RAM_BSS_ATTR static muse_browse_t *s_b;
EXT_RAM_BSS_ATTR static SemaphoreHandle_t s_lock;
EXT_RAM_BSS_ATTR static volatile uint32_t s_seq;
EXT_RAM_BSS_ATTR static volatile bool s_busy;   /* the turn's task under way */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void muse_browse_init(void)
{
    if (s_lock) {
        return;
    }
    s_b = heap_caps_calloc(1, sizeof(*s_b), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lock = s_b ? xSemaphoreCreateMutexWithCaps(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
}

static bool lock(void)
{
    muse_browse_init();
    return s_lock && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}

static void unlock(void)
{
    s_seq = s_b->seq;
    s_busy = muse_browse_running(s_b);
    xSemaphoreGive(s_lock);
}

void muse_browse_turn(void)
{
    if (lock()) {
        muse_browse_reset(s_b);
        unlock();
    }
}

void muse_browse_update(const cJSON *payload)
{
    if (!lock()) {
        return;
    }
    int before = s_b->count;
    bool was_done = s_b->done;
    if (muse_browse_apply(s_b, payload, now_ms()) && (s_b->count != before || s_b->done != was_done)) {
        const muse_browse_step_t *s = &s_b->steps[s_b->count - 1];
        ESP_LOGI(TAG, "%s: %s %s \"%.60s\"%s", s_b->task + (strncmp(s_b->task, "browser-task:", 13) ? 0 : 13), s->what,
                 s->site, s->title, s_b->done ? (s_b->failed ? " (stopped)" : " (done)") : "");
    }
    unlock();
}

bool muse_browse_busy(void)
{
    return s_busy;
}

uint32_t muse_browse_seq(void)
{
    return s_seq;
}

bool muse_browse_get(muse_browse_t *out)
{
    if (!lock()) {
        return false;
    }
    *out = *s_b;
    xSemaphoreGive(s_lock);
    return true;
}

/* ---- Bench ---- */

static const struct {
    float at;
    const char *what, *status, *domain, *title, *url;
} BENCH[] = {
    { 0.0f, "Starting browser...", "Queued", NULL, NULL, NULL },
    { 1.0f, "Checking site", "Running", NULL, NULL, NULL },
    { 2.0f, NULL, "Running", "www.spl.org", "The Seattle Public Library", "https://www.spl.org/" },
    { 3.2f, "Reading page", "Running", "www.spl.org", "The Seattle Public Library", "https://www.spl.org/" },
    { 4.4f, "Selecting option", "Running", "www.spl.org", "Hours & Locations | The Seattle Public Library",
      "https://www.spl.org/hours-and-locations" },
    { 5.6f, NULL, "Running", "www.spl.org", "Central Library | The Seattle Public Library",
      "https://www.spl.org/hours-and-locations/central-library" },
    { 6.6f, "Scrolling page", "Running", "www.spl.org", "Central Library | The Seattle Public Library",
      "https://www.spl.org/hours-and-locations/central-library" },
    { 8.0f, "Task completed", "Completed", "www.spl.org", "Central Library | The Seattle Public Library",
      "https://www.spl.org/hours-and-locations/central-library" },
};

EXT_RAM_BSS_ATTR static int64_t s_bench_at;   /* 0: none running */
EXT_RAM_BSS_ATTR static int s_bench_next;
EXT_RAM_BSS_ATTR static int s_bench_runs;   /* each run's task is new: an old one's is passed over */

void muse_browse_bench(void)
{
    muse_browse_turn();
    s_bench_runs++;
    s_bench_next = 0;
    s_bench_at = now_ms();
}

void muse_browse_bench_tick(void)
{
    if (!s_bench_at) {
        return;
    }
    float t = (float)(now_ms() - s_bench_at) / 1000.0f;
    while (s_bench_next < (int)(sizeof(BENCH) / sizeof(BENCH[0])) && BENCH[s_bench_next].at <= t) {
        const typeof(BENCH[0]) *s = &BENCH[s_bench_next++];
        cJSON *p = cJSON_CreateObject(), *d = cJSON_AddObjectToObject(p, "data");
        char id[32];
        snprintf(id, sizeof(id), "browser-task:bench-%d", s_bench_runs);
        cJSON_AddStringToObject(d, "browser_task_id", id);
        cJSON_AddStringToObject(d, "status", s->status);
        if (s->what) {
            cJSON_AddStringToObject(d, "activity_title", s->what);
        }
        if (s->domain) {
            cJSON *site = cJSON_AddObjectToObject(d, "current_site");
            cJSON_AddStringToObject(site, "domain", s->domain);
            cJSON_AddStringToObject(site, "page_title", s->title);
            cJSON_AddStringToObject(site, "url", s->url);
        }
        muse_browse_update(p);
        cJSON_Delete(p);
    }
    if (s_bench_next >= (int)(sizeof(BENCH) / sizeof(BENCH[0]))) {
        s_bench_at = 0;
    }
}
#endif
