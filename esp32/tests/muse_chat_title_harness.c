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
 * muse_chat_title.c for test_muse_chat_title.py. Each line of stdin is a
 * title (or first words); each line out is the answer for it:
 *
 *   harness placeholder     1 if it's a stand-in, else 0
 *   harness excerpt CAP     the stand-in name from those words, CAP bytes
 *
 * "\n" in a line is a newline, so a title can hold one.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_chat_title.h"

static void unescape(char *s)
{
    char *o = s;
    for (const char *p = s; *p; p++) {
        if (p[0] == '\\' && p[1] == 'n') {
            *o++ = '\n';
            p++;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        return 2;
    }
    bool excerpt = !strcmp(argv[1], "excerpt");
    size_t cap = excerpt && argc > 2 ? (size_t)atoi(argv[2]) : 0;
    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = '\0';
        unescape(line);
        if (excerpt) {
            char out[128];
            memset(out, 'X', sizeof(out));
            muse_chat_title_excerpt(line, out, cap);
            if (cap && strlen(out) >= cap) {
                return 3;   /* past its room */
            }
            printf("%s\n", cap ? out : "");
        } else {
            printf("%d\n", muse_chat_title_placeholder(line) ? 1 : 0);
        }
    }
    return 0;
}
