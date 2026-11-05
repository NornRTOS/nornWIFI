/* Not mbedTLS: the supplicant's OS port zeroes memory through this. */
#ifndef NW_STUB_MBEDTLS_PLATFORM_UTIL_INCLUDED
#define NW_STUB_MBEDTLS_PLATFORM_UTIL_INCLUDED
#include <stddef.h>
void mbedtls_platform_zeroize(void *buf, size_t len);
#endif
