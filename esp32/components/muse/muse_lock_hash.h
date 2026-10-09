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
#include <stdint.h>

/*
 * What's kept of the passcode: never the digits, only PBKDF2-HMAC-SHA256 of
 * them (PSA crypto), with a random salt and enough rounds that each guess
 * takes the S3 a good part of a second. That's what makes the digits costly
 * to find from a copy of the flash: a 6-digit code has only a million.
 */

#define MUSE_LOCK_SALT_LEN 16
#define MUSE_LOCK_HASH_LEN 32
#define MUSE_LOCK_RECORD_VERSION 1
#define MUSE_LOCK_ROUNDS_MIN 10000
#define MUSE_LOCK_ROUNDS_MAX 2000000

typedef struct {
    uint32_t version;
    uint32_t rounds;
    uint8_t salt[MUSE_LOCK_SALT_LEN];
    uint8_t hash[MUSE_LOCK_HASH_LEN];
} muse_lock_record_t;

/* PBKDF2-HMAC-SHA256 of `pin`; false if the crypto failed. */
bool muse_lock_hash_derive(const char *pin, size_t pin_len, const uint8_t salt[MUSE_LOCK_SALT_LEN], uint32_t rounds,
                           uint8_t out[MUSE_LOCK_HASH_LEN]);
/* Equal, in a time that doesn't depend on where they differ. */
bool muse_lock_hash_same(const uint8_t *a, const uint8_t *b, size_t n);
/* A record of `pin` with `salt` (random, from the caller) and `rounds`. */
bool muse_lock_hash_make(const char *pin, size_t pin_len, const uint8_t salt[MUSE_LOCK_SALT_LEN], uint32_t rounds,
                         muse_lock_record_t *out);
/* `pin` is the one `rec` was made from; false for a record that's not sane. */
bool muse_lock_hash_check(const muse_lock_record_t *rec, const char *pin, size_t pin_len);
/* Rounds that take about `target_ms`, from `probe_rounds` that took `probe_us`. */
uint32_t muse_lock_hash_rounds_for(uint32_t probe_rounds, int64_t probe_us, uint32_t target_ms);
