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

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A new chat's title, as the Muse gives it. Before its real one it may hold
 * a stand-in for a while ("Processing audio", "Audio file", "New chat", the
 * voice note's file name): never a chat's name here. Pure; any task
 * (test_muse_chat_title.py).
 */

/* True if `title` is no real title yet: empty, a stand-in the Muse shows while
 * it works one out, a file's name or an id. */
bool muse_chat_title_placeholder(const char *title);

/* A name from the first words said in a chat, for one the Muse never titled:
 * up to `cap` - 1 bytes, cut at a word with "..." if it's longer, the first
 * letter a capital. "" if there are no words. */
void muse_chat_title_excerpt(const char *words, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
