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

/* PSA's PBKDF2-HMAC-SHA256 on OpenSSL, for the host; the inputs taken in
 * PSA's order (cost, salt, password), as mbedTLS insists on. */
#include "psa/crypto.h"

#include <string.h>

#include <openssl/evp.h>

psa_status_t psa_crypto_init(void)
{
    return PSA_SUCCESS;
}

psa_status_t psa_key_derivation_setup(psa_key_derivation_operation_t *op, psa_algorithm_t alg)
{
    if (op->stage) {
        return PSA_ERROR_BAD_STATE;
    }
    if (alg != PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256)) {
        return PSA_ERROR_NOT_SUPPORTED;
    }
    op->alg = alg;
    op->stage = 1;
    return PSA_SUCCESS;
}

psa_status_t psa_key_derivation_input_integer(psa_key_derivation_operation_t *op, psa_key_derivation_step_t step,
                                              uint64_t value)
{
    if (op->stage != 1 || step != PSA_KEY_DERIVATION_INPUT_COST) {
        return PSA_ERROR_BAD_STATE;
    }
    if (!value || value > 0x7fffffff) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    op->cost = value;
    op->stage = 2;
    return PSA_SUCCESS;
}

psa_status_t psa_key_derivation_input_bytes(psa_key_derivation_operation_t *op, psa_key_derivation_step_t step,
                                            const uint8_t *data, size_t len)
{
    if (step == PSA_KEY_DERIVATION_INPUT_SALT && op->stage == 2 && len <= sizeof(op->salt)) {
        memcpy(op->salt, data, len);
        op->salt_len = len;
        op->stage = 3;
        return PSA_SUCCESS;
    }
    if (step == PSA_KEY_DERIVATION_INPUT_PASSWORD && op->stage == 3 && len <= sizeof(op->password)) {
        memcpy(op->password, data, len);
        op->password_len = len;
        op->stage = 4;
        return PSA_SUCCESS;
    }
    return PSA_ERROR_BAD_STATE;
}

psa_status_t psa_key_derivation_output_bytes(psa_key_derivation_operation_t *op, uint8_t *out, size_t len)
{
    if (op->stage != 4) {
        return PSA_ERROR_BAD_STATE;
    }
    int ok = PKCS5_PBKDF2_HMAC((const char *)op->password, (int)op->password_len, op->salt, (int)op->salt_len,
                               (int)op->cost, EVP_sha256(), (int)len, out);
    return ok == 1 ? PSA_SUCCESS : PSA_ERROR_BAD_STATE;
}

psa_status_t psa_key_derivation_abort(psa_key_derivation_operation_t *op)
{
    memset(op, 0, sizeof(*op));
    return PSA_SUCCESS;
}
