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

/* The passcode's hash (muse_lock_hash.h): PSA's PBKDF2, on the host too (tests/lock_fakes). */
#include "muse_lock_hash.h"

#include <string.h>

#include "psa/crypto.h"

bool muse_lock_hash_derive(const char *pin, size_t pin_len, const uint8_t salt[MUSE_LOCK_SALT_LEN], uint32_t rounds,
                           uint8_t out[MUSE_LOCK_HASH_LEN])
{
    if (psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    bool ok = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256)) == PSA_SUCCESS
              && psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, rounds) == PSA_SUCCESS
              && psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, MUSE_LOCK_SALT_LEN)
                     == PSA_SUCCESS
              && psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD, (const uint8_t *)pin, pin_len)
                     == PSA_SUCCESS
              && psa_key_derivation_output_bytes(&op, out, MUSE_LOCK_HASH_LEN) == PSA_SUCCESS;
    psa_key_derivation_abort(&op);
    if (!ok) {
        memset(out, 0, MUSE_LOCK_HASH_LEN);
    }
    return ok;
}

bool muse_lock_hash_same(const uint8_t *a, const uint8_t *b, size_t n)
{
    volatile uint8_t d = 0;
    for (size_t i = 0; i < n; i++) {
        d |= a[i] ^ b[i];
    }
    return d == 0;
}

bool muse_lock_hash_make(const char *pin, size_t pin_len, const uint8_t salt[MUSE_LOCK_SALT_LEN], uint32_t rounds,
                         muse_lock_record_t *out)
{
    memset(out, 0, sizeof(*out));
    out->version = MUSE_LOCK_RECORD_VERSION;
    out->rounds = rounds;
    memcpy(out->salt, salt, MUSE_LOCK_SALT_LEN);
    return muse_lock_hash_derive(pin, pin_len, salt, rounds, out->hash);
}

bool muse_lock_hash_check(const muse_lock_record_t *rec, const char *pin, size_t pin_len)
{
    if (rec->version != MUSE_LOCK_RECORD_VERSION || rec->rounds < MUSE_LOCK_ROUNDS_MIN
        || rec->rounds > MUSE_LOCK_ROUNDS_MAX) {
        return false;
    }
    uint8_t got[MUSE_LOCK_HASH_LEN];
    bool ok = muse_lock_hash_derive(pin, pin_len, rec->salt, rec->rounds, got)
              && muse_lock_hash_same(got, rec->hash, MUSE_LOCK_HASH_LEN);
    memset(got, 0, sizeof(got));
    return ok;
}

uint32_t muse_lock_hash_rounds_for(uint32_t probe_rounds, int64_t probe_us, uint32_t target_ms)
{
    if (probe_us <= 0 || !probe_rounds) {
        return MUSE_LOCK_ROUNDS_MAX;
    }
    uint64_t r = (uint64_t)probe_rounds * target_ms * 1000 / (uint64_t)probe_us;
    r = r / 1000 * 1000;   /* a round number, for the log */
    return r < MUSE_LOCK_ROUNDS_MIN ? MUSE_LOCK_ROUNDS_MIN : r > MUSE_LOCK_ROUNDS_MAX ? MUSE_LOCK_ROUNDS_MAX : (uint32_t)r;
}
