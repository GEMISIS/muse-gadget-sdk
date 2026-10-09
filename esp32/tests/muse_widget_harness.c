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
#include "muse_widget.h"

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
    static const char *const KINDS[] = { "options", "list", "map", "shopping", "card" };
    static const char *const TYPES[] = { "generic", "option", "send", "link", "calendar",
                                         "email", "flight", "place", "product" };
    cJSON_AddStringToObject(out, "kind", KINDS[w.kind]);
    cJSON_AddStringToObject(out, "name", w.name);
    cJSON_AddStringToObject(out, "id", w.id);
    cJSON_AddStringToObject(out, "title", w.title);
    cJSON_AddStringToObject(out, "text", w.text);
    cJSON_AddBoolToObject(out, "filled", w.filled);
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
