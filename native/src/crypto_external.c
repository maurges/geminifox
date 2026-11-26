#include "psa/crypto.h"

#include "utils.h"

/// RNG for the psa part of mbedtls, which is used by tls 1.3. The library
/// calls this by name as we set MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG in the config
psa_status_t mbedtls_psa_external_get_random(
    mbedtls_psa_external_random_context_t * /* context */,
    uint8_t * output,
    size_t output_size,
    size_t * output_length
) {
    if (random_buffer(NULL, output, output_size) != 0) {
        return PSA_ERROR_INSUFFICIENT_ENTROPY;
    }
    *output_length = output_size;
    return PSA_SUCCESS;
}
