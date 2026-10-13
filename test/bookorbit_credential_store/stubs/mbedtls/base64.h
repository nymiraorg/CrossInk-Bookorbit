#pragma once

#include <cstddef>

#define MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL (-0x002A)

// Minimal stub: deobfuscation is not exercised by this test.
inline int mbedtls_base64_decode(unsigned char* dst, size_t dlen, size_t* olen, const unsigned char* src,
                                 size_t slen) {
  (void)dst;
  (void)dlen;
  (void)src;
  (void)slen;
  if (olen != nullptr) *olen = 0;
  return 0;
}
