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
 * Included ahead of libwebp's src/utils/utils.c (CMakeLists.txt), where all
 * of its allocations are made: they go to PSRAM, falling back to internal
 * RAM only if PSRAM is full, so the decoder never takes the internal RAM
 * Wi-Fi and TLS need. stdlib.h first, so its own declarations are untouched.
 */
#pragma once

#include <stdlib.h>

#include "esp_heap_caps.h"

#define WEBP_PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define malloc(n) heap_caps_malloc_prefer((n), 2, WEBP_PSRAM_CAPS, MALLOC_CAP_8BIT)
#define calloc(n, s) heap_caps_calloc_prefer((n), (s), 2, WEBP_PSRAM_CAPS, MALLOC_CAP_8BIT)
#define free(p) heap_caps_free(p)
