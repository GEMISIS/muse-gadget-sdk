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
 * For test_muse_widget.py. "parse JSON" prints muse_widget_parse's model as
 * JSON (or "null" for none); "sample NAME" parses muse_widget_sample(NAME);
 * "text CAP LINES SRC" prints muse_widget_text(SRC) cut to CAP bytes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "muse_browse.h"
#include "muse_map.h"
#include "muse_widget.h"

/* "browse FILE [TURN]": a JSON array of browser_task payloads, a second apart, a new turn
 * before the TURN'th if given: the history after them all, as JSON. */
static int browse(const char *path, int turn)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 2;
    }
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    cJSON *list = cJSON_Parse(buf);
    static muse_browse_t b;
    memset(&b, 0, sizeof(b));
    muse_browse_reset(&b);
    int i = 0;
    cJSON *p;
    cJSON_ArrayForEach(p, list) {
        if (i == turn) {
            muse_browse_reset(&b);
        }
        muse_browse_apply(&b, p, 1000LL * i++);
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "task", b.task);
    cJSON_AddBoolToObject(out, "done", b.done);
    cJSON_AddBoolToObject(out, "failed", b.failed);
    cJSON *steps = cJSON_AddArrayToObject(out, "steps");
    for (int k = 0; k < b.count; k++) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "what", b.steps[k].what);
        cJSON_AddStringToObject(s, "site", b.steps[k].site);
        cJSON_AddStringToObject(s, "title", b.steps[k].title);
        cJSON_AddStringToObject(s, "url", b.steps[k].url);
        cJSON_AddNumberToObject(s, "at", (double)b.steps[k].at_ms);
        cJSON_AddItemToArray(steps, s);
    }
    char *s = cJSON_PrintUnformatted(out);
    fputs(s, stdout);
    free(s);
    cJSON_Delete(out);
    cJSON_Delete(list);
    return 0;
}

/* "map LAT1 LON1 LAT2 LON2 [MILES]": the distance, which way, and the fit of both in 400 x 200. */
static int map(char **a, int n)
{
    double la1 = atof(a[0]), lo1 = atof(a[1]), la2 = atof(a[2]), lo2 = atof(a[3]);
    char d[24];
    muse_map_distance(d, sizeof(d), muse_map_metres(la1, lo1, la2, lo2), n > 4 && atoi(a[4]));
    double lat[2] = { la1, la2 }, lon[2] = { lo1, lo2 }, cx, cy;
    int z = muse_map_fit(lat, lon, 2, 400, 200, 30, 17, &cx, &cy);
    printf("{\"distance\":\"%s\",\"compass\":\"%s\",\"zoom\":%d,\"cx\":%.1f,\"cy\":%.1f}", d,
           muse_map_compass(la1, lo1, la2, lo2), z, cx, cy);
    return 0;
}

static int print_model(const char *json)
{
    cJSON *in = cJSON_Parse(json);
    if (!in) {
        return 2;
    }
    static muse_widget_t w;
    memset(&w, 0x5a, sizeof(w));   /* everything it leaves must be set by it */
    if (!muse_widget_parse(in, &w)) {
        cJSON_Delete(in);
        printf("null");
        return 0;
    }
    cJSON *out = cJSON_CreateObject();
    static const char *const KINDS[] = { "options", "list", "map", "shopping", "card", "form", "checks" };
    static const char *const TYPES[] = { "generic", "option", "send", "link", "calendar",
                                         "email", "flight", "place", "product", "field", "check" };
    cJSON_AddStringToObject(out, "kind", KINDS[w.kind]);
    cJSON_AddStringToObject(out, "name", w.name);
    cJSON_AddStringToObject(out, "id", w.id);
    cJSON_AddStringToObject(out, "title", w.title);
    cJSON_AddStringToObject(out, "text", w.text);
    cJSON_AddBoolToObject(out, "filled", w.filled);
    cJSON_AddStringToObject(out, "action", w.action);
    cJSON_AddStringToObject(out, "lead", w.lead);
    cJSON_AddNumberToObject(out, "zoom", w.zoom);
    cJSON *rows = cJSON_AddArrayToObject(out, "rows");
    for (int i = 0; i < w.count; i++) {
        const muse_widget_row_t *r = &w.rows[i];
        cJSON *row = cJSON_CreateObject();
        cJSON_AddStringToObject(row, "type", TYPES[r->type]);
        cJSON_AddStringToObject(row, "title", r->title);
        cJSON_AddStringToObject(row, "sub", r->sub);
        cJSON_AddStringToObject(row, "extra", r->extra);
        cJSON_AddStringToObject(row, "meta", r->meta);
        cJSON_AddStringToObject(row, "button", r->button);
        cJSON_AddStringToObject(row, "send", r->send);
        cJSON_AddStringToObject(row, "button2", r->button2);
        cJSON_AddStringToObject(row, "send2", r->send2);
        cJSON_AddStringToObject(row, "url", r->url);
        cJSON_AddStringToObject(row, "image", r->image);
        if (r->pos) {
            cJSON_AddNumberToObject(row, "lat", r->lat);
            cJSON_AddNumberToObject(row, "lon", r->lon);
        }
        cJSON_AddItemToArray(rows, row);
    }
    char *s = cJSON_PrintUnformatted(out);
    fputs(s, stdout);
    free(s);
    cJSON_Delete(out);
    cJSON_Delete(in);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "parse")) {
        return print_model(argv[2]);
    }
    if (argc >= 3 && !strcmp(argv[1], "browse")) {
        return browse(argv[2], argc > 3 ? atoi(argv[3]) : -1);
    }
    if (argc >= 6 && !strcmp(argv[1], "map")) {
        return map(argv + 2, argc - 2);
    }
    if (argc == 3 && !strcmp(argv[1], "lead")) {
        char out[MUSE_WIDGET_LEAD];
        muse_widget_lead(out, sizeof(out), argv[2]);
        fputs(out, stdout);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "sample")) {
        const char *json = muse_widget_sample(argv[2]);
        return json ? print_model(json) : 3;
    }
    if (argc == 5 && !strcmp(argv[1], "text")) {
        size_t cap = (size_t)atoi(argv[2]);
        char *out = malloc(cap);
        memset(out, 0x5a, cap);
        muse_widget_text(out, cap, argv[4], atoi(argv[3]) != 0);
        fputs(out, stdout);
        free(out);
        return 0;
    }
    return 2;
}
