#pragma once
#include <cstddef>
#define MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL -0x002A
#define MBEDTLS_ERR_BASE64_INVALID_CHARACTER -0x002C
int mbedtls_base64_decode(unsigned char* dst, size_t dlen, size_t* olen, const unsigned char* src,
                          size_t slen);
