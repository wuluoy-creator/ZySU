#pragma once
// Independent observer codec: only the existing pinned SHA-256 implementation.
#define MBEDTLS_SHA256_C
#if defined(_WIN32)
// mbedTLS's Windows configuration gate requires the platform feature even
// though SHA-256 does not call its optional printf/allocator entry points.
#define MBEDTLS_PLATFORM_C
#endif
