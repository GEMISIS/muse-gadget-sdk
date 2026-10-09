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

/* Just the PSA key derivation muse_lock_hash.c uses, for the host: PBKDF2 on
 * OpenSSL (psa_openssl.c). Names and order of inputs as PSA's own. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int32_t psa_status_t;
typedef uint32_t psa_algorithm_t;
typedef uint16_t psa_key_derivation_step_t;

#define PSA_SUCCESS ((psa_status_t)0)
#define PSA_ERROR_BAD_STATE ((psa_status_t)-137)
#define PSA_ERROR_NOT_SUPPORTED ((psa_status_t)-134)
#define PSA_ERROR_INVALID_ARGUMENT ((psa_status_t)-135)

#define PSA_ALG_SHA_256 ((psa_algorithm_t)0x02000009)
#define PSA_ALG_PBKDF2_HMAC(h) ((psa_algorithm_t)(0x08800100 | ((h) & 0xff)))

#define PSA_KEY_DERIVATION_INPUT_PASSWORD ((psa_key_derivation_step_t)0x0102)
#define PSA_KEY_DERIVATION_INPUT_SALT ((psa_key_derivation_step_t)0x0202)
#define PSA_KEY_DERIVATION_INPUT_COST ((psa_key_derivation_step_t)0x0205)

typedef struct {
    psa_algorithm_t alg;
    int stage;               /* 0 none, 1 set up, 2 cost, 3 salt, 4 password */
    uint64_t cost;
    uint8_t salt[64];
    size_t salt_len;
    uint8_t password[64];
    size_t password_len;
} psa_key_derivation_operation_t;

#define PSA_KEY_DERIVATION_OPERATION_INIT { 0 }

psa_status_t psa_crypto_init(void);
psa_status_t psa_key_derivation_setup(psa_key_derivation_operation_t *op, psa_algorithm_t alg);
psa_status_t psa_key_derivation_input_integer(psa_key_derivation_operation_t *op, psa_key_derivation_step_t step,
                                              uint64_t value);
psa_status_t psa_key_derivation_input_bytes(psa_key_derivation_operation_t *op, psa_key_derivation_step_t step,
                                            const uint8_t *data, size_t len);
psa_status_t psa_key_derivation_output_bytes(psa_key_derivation_operation_t *op, uint8_t *out, size_t len);
psa_status_t psa_key_derivation_abort(psa_key_derivation_operation_t *op);
