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
 * libwebp's build configuration (HAVE_CONFIG_H, set by CMakeLists.txt and the
 * host test), written by hand in place of its configure step. Plain C only:
 * no WEBP_HAVE_* for SSE, NEON or MIPS (only the C sources are vendored), and
 * no WEBP_USE_THREAD. The byte-swap builtins GCC and Clang both have.
 */
#ifndef WEBP_WEBP_CONFIG_H_
#define WEBP_WEBP_CONFIG_H_

#define HAVE_BUILTIN_BSWAP16 1
#define HAVE_BUILTIN_BSWAP32 1
#define HAVE_BUILTIN_BSWAP64 1

#endif
