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

/* muse_img_url.c on the host: each argument's smaller copy on a line, "-" for none. */
#include <stdio.h>
#include <stdlib.h>

#include "muse_img_url.c"

int main(int argc, char **argv)
{
    int px = getenv("PX") ? atoi(getenv("PX")) : MUSE_IMG_URL_PX;
    size_t cap = getenv("CAP") ? (size_t)atoi(getenv("CAP")) : 1024;
    for (int i = 1; i < argc; i++) {
        char out[1024] = "untouched";
        bool ok = muse_img_url_smaller(argv[i], px, out, cap);
        printf("%s\n", ok ? out : (out[0] == 'u' ? "-" : "TOUCHED"));
    }
    return 0;
}
