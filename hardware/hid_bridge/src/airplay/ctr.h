#pragma once
#include <string.h>
#include "mbedtls/aes.h"

// Keep DMA buffers in internal task memory; PSRAM frames must not require a large temporary allocation.
// Offset, counter and partial keystream continue across both chunks and video packets.
static inline int ap_aes_ctr(mbedtls_aes_context *aes, size_t length, size_t *offset,
                             unsigned char counter[16], unsigned char stream[16], unsigned char *data) {
  unsigned char buffer[1024];
  while (length) {
    size_t n = length < sizeof buffer ? length : sizeof buffer;
    memcpy(buffer, data, n);
    int result = mbedtls_aes_crypt_ctr(aes, n, offset, counter, stream, buffer, buffer);
    if (result) return result;  // caller invalidates the entire stream, never skips a failed packet
    memcpy(data, buffer, n);
    data += n;
    length -= n;
  }
  return 0;
}
