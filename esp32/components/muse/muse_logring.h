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
 * The log's last lines, kept in PSRAM as well as printed: what happened
 * before anyone was watching the console (">log" prints them). Lines a busy
 * moment would have to wait for are left out of it, never held up.
 */

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_MUSE_BOARD_WAVESHARE_S3_216
void muse_logring_start(void);
/* Prints what's kept, oldest first, between "@log begin" and "@log end". */
void muse_logring_print(void);
#else
static inline void muse_logring_start(void) {}
static inline void muse_logring_print(void) {}
#endif

#ifdef __cplusplus
}
#endif
