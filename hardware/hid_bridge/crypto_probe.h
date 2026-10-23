#pragma once
#include <mbedtls/aes.h>
#include "src/airplay/ctr.h"

// Serial-only diagnostic. Synthetic public zero key; never reads session keys or video.
static void crypto_probe(bool inject_pressure) {
  const size_t length = 32777;
  uint8_t key[16] = {}, nonce[16] = {}, block[16];
  uint8_t *expected = (uint8_t *)heap_caps_malloc(length, MALLOC_CAP_SPIRAM);
  uint8_t *data = (uint8_t *)heap_caps_malloc(length, MALLOC_CAP_SPIRAM);
  if (!expected || !data) {
    free(expected); free(data); Serial.println("crypto-test: allocation failed"); return;
  }
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  int result = mbedtls_aes_setkey_enc(&aes, key, 128);
  for (size_t offset = 0; !result && offset < length; offset += 16) {
    result = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, nonce, block);
    memcpy(expected + offset, block, min((size_t)16, length - offset));
    for (int i = 15; i >= 0 && ++nonce[i] == 0; --i) {}
  }
  if (result) Serial.printf("crypto-test: ECB reference failed %d\n", result);
  else {
    void *reserve[24] = {};
    const size_t strides[] = {length, 4096, 2683, 2048, 1600, 573, 17};
    for (bool pressure : {false, true}) {
      if (pressure && !inject_pressure) break;
      if (pressure) {
        for (auto &p : reserve) {
          if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 7000) break;
          p = heap_caps_malloc(1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
          if (!p) break;
        }
      }
      for (bool bounded : {false, true}) for (size_t stride : strides) {
        memset(nonce, 0, sizeof nonce); memset(data, 0, length);
        size_t offset = 0;
        uint8_t stream[16] = {};
        uint32_t start = millis();
        for (size_t pos = 0; pos < length; pos += stride) {
          size_t n = min(stride, length - pos);
          result = bounded ? ap_aes_ctr(&aes, n, &offset, nonce, stream, data + pos)
              : mbedtls_aes_crypt_ctr(&aes, n, &offset, nonce, stream, data + pos, data + pos);
          if (result) break;
        }
        size_t mismatch = 0;
        while (mismatch < length && data[mismatch] == expected[mismatch]) ++mismatch;
        Serial.printf("crypto-test: pressure=%d bounded=%d stride=%u result=%d match=%u/%u ms=%lu heap=%u largest=%u\n",
                      pressure, bounded, (unsigned)stride, result, (unsigned)mismatch, (unsigned)length,
                      millis() - start, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
      }
    }
    for (auto p : reserve) free(p);
  }
  mbedtls_aes_free(&aes);
  free(expected); free(data);
}
