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
 * Streams argv[1..] into a reply's text one piece at a time, as the chat
 * session does (append, then muse_chat_strip_images), for test_muse_chat_md.py.
 * After each piece it prints the text as shown (muse_chat_shown_len) and the
 * text kept, then the first image's path and label:
 *   <shown>\x1f<kept>\x1e ... <path>\x1f<label>
 * With "find" first, prints muse_chat_first_image(argv[2]) and
 * muse_chat_image_file(argv[2]) instead.
 */
#include <stdio.h>
#include <string.h>

#include "muse_chat_md.h"

int main(int argc, char **argv)
{
    muse_chat_image_t img = { "", "" };
    if (argc == 3 && !strcmp(argv[1], "find")) {
        const char *file = muse_chat_image_file(argv[2]);
        bool found = muse_chat_first_image(argv[2], &img);
        printf("%s\x1f%s\x1f%s", found ? img.path : "-", found ? img.label : "-", file ? file : "-");
        return 0;
    }
    static char text[1024];
    for (int i = 1; i < argc; i++) {
        size_t had = strlen(text), add = strlen(argv[i]);
        if (had + add >= sizeof(text)) {
            return 2;
        }
        memcpy(text + had, argv[i], add + 1);
        size_t gone = muse_chat_strip_images(text, &img);
        if (strlen(text) + gone != had + add) {
            return 3;
        }
        size_t shown = muse_chat_shown_len(text);
        printf("%.*s\x1f%s\x1e", (int)shown, text, text);
    }
    printf("%s\x1f%s", img.path, img.label);
    return 0;
}
