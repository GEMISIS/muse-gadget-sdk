// Copyright (c) Meta Platforms, Inc. and affiliates.
// SPDX-License-Identifier: Apache-2.0

/* Host harness for muse_activity.h: each argument "CODE|TEXT" (an empty
 * CODE is none), its activity's name on a line of its own. */
#include <stdio.h>
#include <string.h>

#include "muse_activity.h"

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        char code[64] = "";
        const char *bar = strchr(argv[i], '|');
        const char *text = bar ? bar + 1 : argv[i];
        if (bar && (size_t)(bar - argv[i]) < sizeof(code)) {
            memcpy(code, argv[i], (size_t)(bar - argv[i]));
            code[bar - argv[i]] = '\0';
        }
        printf("%s\n", muse_activity_name(muse_activity_of(code[0] ? code : NULL, text)));
    }
    return 0;
}
