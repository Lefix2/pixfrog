// Real SHA-256 (FIPS 180-4) behind the mbedtls API subset config_store uses.
#pragma once
#include <cstddef>
#include <cstdint>
typedef struct {
    uint32_t state[8];
    uint64_t bits;
    uint8_t buf[64];
    size_t used;
} mbedtls_sha256_context;
void mbedtls_sha256_init(mbedtls_sha256_context* ctx);
void mbedtls_sha256_free(mbedtls_sha256_context* ctx);
int mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int is224);
int mbedtls_sha256_update(mbedtls_sha256_context* ctx, const unsigned char* in, size_t len);
int mbedtls_sha256_finish(mbedtls_sha256_context* ctx, unsigned char out[32]);
