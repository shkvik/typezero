#include "runtime/library/crypto/crypto/crypto.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/error/error.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/bytes/bytes/bytes.h"
#include <limits.h>
#include <string.h>

extern const TzMd tz_crypto_md_sha256;

static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void salsa20_8(uint32_t b[16]) {
  uint32_t x[16];
  memcpy(x, b, sizeof x);
  for (int i = 0; i < 8; i += 2) {
    x[4] ^= rotl(x[0] + x[12], 7), x[8] ^= rotl(x[4] + x[0], 9);
    x[12] ^= rotl(x[8] + x[4], 13), x[0] ^= rotl(x[12] + x[8], 18);
    x[9] ^= rotl(x[5] + x[1], 7), x[13] ^= rotl(x[9] + x[5], 9);
    x[1] ^= rotl(x[13] + x[9], 13), x[5] ^= rotl(x[1] + x[13], 18);
    x[14] ^= rotl(x[10] + x[6], 7), x[2] ^= rotl(x[14] + x[10], 9);
    x[6] ^= rotl(x[2] + x[14], 13), x[10] ^= rotl(x[6] + x[2], 18);
    x[3] ^= rotl(x[15] + x[11], 7), x[7] ^= rotl(x[3] + x[15], 9);
    x[11] ^= rotl(x[7] + x[3], 13), x[15] ^= rotl(x[11] + x[7], 18);
    x[1] ^= rotl(x[0] + x[3], 7), x[2] ^= rotl(x[1] + x[0], 9);
    x[3] ^= rotl(x[2] + x[1], 13), x[0] ^= rotl(x[3] + x[2], 18);
    x[6] ^= rotl(x[5] + x[4], 7), x[7] ^= rotl(x[6] + x[5], 9);
    x[4] ^= rotl(x[7] + x[6], 13), x[5] ^= rotl(x[4] + x[7], 18);
    x[11] ^= rotl(x[10] + x[9], 7), x[8] ^= rotl(x[11] + x[10], 9);
    x[9] ^= rotl(x[8] + x[11], 13), x[10] ^= rotl(x[9] + x[8], 18);
    x[12] ^= rotl(x[15] + x[14], 7), x[13] ^= rotl(x[12] + x[15], 9);
    x[14] ^= rotl(x[13] + x[12], 13), x[15] ^= rotl(x[14] + x[13], 18);
  }
  for (int i = 0; i < 16; ++i) {
    b[i] += x[i];
  }
}

static void block_mix(const uint32_t *in, uint32_t *out, uint64_t r) {
  uint32_t x[16];
  memcpy(x, in + (2 * r - 1) * 16, sizeof x);
  for (uint64_t i = 0; i < 2 * r; ++i) {
    for (int k = 0; k < 16; ++k) {
      x[k] ^= in[i * 16 + k];
    }
    salsa20_8(x);
    memcpy(out + ((i & 1) * r + i / 2) * 16, x, sizeof x);
  }
}

static void ro_mix(uint8_t *b, uint64_t r, uint64_t n, uint32_t *v, uint32_t *x, uint32_t *t) {
  size_t words = (size_t)(32 * r);
  for (size_t i = 0; i < words; ++i) {
    x[i] = (uint32_t)b[4 * i] | (uint32_t)b[4 * i + 1] << 8 | (uint32_t)b[4 * i + 2] << 16 |
           (uint32_t)b[4 * i + 3] << 24;
  }
  for (uint64_t i = 0; i < n; ++i) {
    memcpy(v + i * words, x, words * 4);
    block_mix(x, t, r);
    memcpy(x, t, words * 4);
  }
  for (uint64_t i = 0; i < n; ++i) {
    const uint32_t *last = x + (2 * r - 1) * 16;
    uint64_t j = ((uint64_t)last[0] | (uint64_t)last[1] << 32) & (n - 1);
    for (size_t k = 0; k < words; ++k) {
      x[k] ^= v[j * words + k];
    }
    block_mix(x, t, r);
    memcpy(x, t, words * 4);
  }
  for (size_t i = 0; i < words; ++i) {
    b[4 * i] = (uint8_t)x[i];
    b[4 * i + 1] = (uint8_t)(x[i] >> 8);
    b[4 * i + 2] = (uint8_t)(x[i] >> 16);
    b[4 * i + 3] = (uint8_t)(x[i] >> 24);
  }
}

static int check(uint64_t n, uint64_t r, uint64_t p, uint64_t maxmem) {
  const uint64_t pr_max = (1u << 30) - 1;
  if (r == 0 || p == 0 || n < 2 || (n & (n - 1))) {
    return 0;
  }
  if (p > pr_max / r) {
    return -1;
  }
  if (16 * r <= 63 && n >= ((uint64_t)1 << (16 * r))) {
    return -1;
  }
  uint64_t blen = p * 128 * r;
  if (blen > INT_MAX) {
    return -1;
  }
  if (n + 2 > (UINT64_MAX / (32 * sizeof(uint32_t))) / r) {
    return -1;
  }
  uint64_t vlen = 32 * r * (n + 2) * sizeof(uint32_t);
  if (blen > UINT64_MAX - vlen || blen + vlen > maxmem) {
    return -1;
  }

  return 1;
}

static void explain(int ok, uint64_t n, uint64_t r, uint64_t p, uint64_t limit) {
  if (ok == 0 && r == 0) {
    tz_crypto_note_parameter("r", 0);
    tz_crypto_fail("ERR_CRYPTO_INVALID_SCRYPT_PARAMS", "scrypt: r must be 1 or more, not 0");
    return;
  }
  if (ok == 0 && p == 0) {
    tz_crypto_note_parameter("p", 0);
    tz_crypto_fail("ERR_CRYPTO_INVALID_SCRYPT_PARAMS", "scrypt: p must be 1 or more, not 0");
    return;
  }
  if (ok == 0) {
    tz_crypto_note_parameter("N", (double)n);
    tz_crypto_fail("ERR_CRYPTO_INVALID_SCRYPT_PARAMS",
                   "scrypt: N must be a power of two greater than 1, not %llu",
                   (unsigned long long)n);
    return;
  }
  double need = 128.0 * (double)r * ((double)n + 2) + 128.0 * (double)r * (double)p;
  if (need > (double)limit) {
    tz_crypto_note_parameter("maxmem", (double)limit);
    tz_crypto_fail("ERR_OSSL_EVP_MEMORY_LIMIT_EXCEEDED",
                   "scrypt with N = %llu, r = %llu and p = %llu needs %.0f bytes of memory, more "
                   "than maxmem (%llu)",
                   (unsigned long long)n, (unsigned long long)r, (unsigned long long)p, need,
                   (unsigned long long)limit);
    return;
  }
  tz_crypto_note_parameter("N", (double)n);
  tz_crypto_fail("ERR_CRYPTO_INVALID_SCRYPT_PARAMS",
                 "scrypt: N = %llu, r = %llu and p = %llu are too large together",
                 (unsigned long long)n, (unsigned long long)r, (unsigned long long)p);
}

TzBytes *tz_crypto_scrypt(const TzType *pt, const void *pv, const TzType *st, const void *sv,
                          double keylen, double cost, double block_size, double parallelization,
                          double maxmem) {
  if (!tz_crypto_check_integer(keylen, "keylen", 0, 2147483647) ||
      !tz_crypto_check_integer(cost, "N", 0, 4294967295.0) ||
      !tz_crypto_check_integer(block_size, "r", 0, 4294967295.0) ||
      !tz_crypto_check_integer(parallelization, "p", 0, 4294967295.0) ||
      !tz_crypto_check_integer(maxmem, "maxmem", 0, 9007199254740991.0)) {
    return NULL;
  }
  uint64_t n = cost ? (uint64_t)cost : 16384, r = block_size ? (uint64_t)block_size : 8;
  uint64_t p = parallelization ? (uint64_t)parallelization : 1;
  uint64_t limit = maxmem ? (uint64_t)maxmem : 32u << 20;
  int ok = check(n, r, p, limit);
  if (ok <= 0) {
    explain(ok, n, r, p, limit);
    return NULL;
  }
  TzCryptoData password, salt;
  if (!tz_crypto_data(&password, pt, pv, TZ_ENC_UTF8, "password")) {
    return NULL;
  }
  if (!tz_crypto_data(&salt, st, sv, TZ_ENC_UTF8, "salt")) {
    tz_crypto_data_done(&password);
    return NULL;
  }
  TzBytes *result = tz_buffer_alloc((int64_t)keylen, 0);
  if (result && keylen > 0) {
    size_t blen = (size_t)(p * 128 * r);
    uint8_t *b = (uint8_t *)tz_alloc(blen);
    uint32_t *v = (uint32_t *)tz_alloc((size_t)(32 * r * (n + 2) * sizeof(uint32_t)));
    uint32_t *x = v + 32 * r * n, *t = x + 32 * r;
    tz_crypto_pbkdf2_raw(&tz_crypto_md_sha256, password.data, password.length, salt.data,
                         salt.length, 1, b, blen);
    for (uint64_t i = 0; i < p; ++i) {
      ro_mix(b + i * 128 * r, r, n, v, x, t);
    }
    tz_crypto_pbkdf2_raw(&tz_crypto_md_sha256, password.data, password.length, b, blen, 1,
                         result->data, (size_t)keylen);
    memset(b, 0, blen);
    tz_free(b);
    tz_free(v);
  }
  tz_crypto_data_done(&salt);
  tz_crypto_data_done(&password);

  return result;
}
