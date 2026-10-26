#include "internal/deprecated.h"
#include "runtime/library/crypto/crypto/crypto.h"
#include "runtime/core/error/error.h"
#include <stddef.h>
#include <stdint.h>
#include <openssl/aes.h>
#include <openssl/crypto.h>
#include <string.h>
#include "crypto/modes.h"

extern unsigned int OPENSSL_ia32cap_P[];
#define AESNI_CAPABLE (OPENSSL_ia32cap_P[1] & (1 << (57 - 32)))

int aesni_set_encrypt_key(const unsigned char *key, int bits, AES_KEY *ks);
int aesni_set_decrypt_key(const unsigned char *key, int bits, AES_KEY *ks);
void aesni_encrypt(const unsigned char *in, unsigned char *out, const AES_KEY *ks);
void aesni_cbc_encrypt(const unsigned char *in, unsigned char *out, size_t length,
                       const AES_KEY *ks, unsigned char *ivec, int enc);
void aesni_ctr32_encrypt_blocks(const unsigned char *in, unsigned char *out, size_t blocks,
                                const void *ks, const unsigned char *ivec);

#if defined(TZ_AES_GCM) || defined(TZ_AES_CTR)
static void encrypt_key(const uint8_t *key, size_t length, AES_KEY *ks) {
  if (AESNI_CAPABLE) {
    aesni_set_encrypt_key(key, (int)length * 8, ks);
  } else {
    AES_set_encrypt_key(key, (int)length * 8, ks);
  }
}
#endif

#define NAMED(bits, mode) "aes-" #bits "-" mode

#ifdef TZ_AES_GCM

size_t aesni_gcm_encrypt(const unsigned char *in, unsigned char *out, size_t len, const void *key,
                         unsigned char ivec[16], u64 *Xi);
size_t aesni_gcm_decrypt(const unsigned char *in, unsigned char *out, size_t len, const void *key,
                         unsigned char ivec[16], u64 *Xi);
void gcm_ghash_avx(u64 Xi[2], const u128 Htable[16], const u8 *in, size_t len);

int ossl_vaes_vpclmulqdq_capable(void);
void ossl_aes_gcm_encrypt_avx512(const void *ks, void *gcm, unsigned int *pblocklen,
                                 const unsigned char *in, size_t len, unsigned char *out);
void ossl_aes_gcm_decrypt_avx512(const void *ks, void *gcm, unsigned int *pblocklen,
                                 const unsigned char *in, size_t len, unsigned char *out);
void ossl_aes_gcm_init_avx512(const void *ks, void *gcm);
void ossl_aes_gcm_setiv_avx512(const void *ks, void *gcm, const unsigned char *iv, size_t ivlen);
void ossl_aes_gcm_update_aad_avx512(void *gcm, const unsigned char *aad, size_t aadlen);
void ossl_aes_gcm_finalize_avx512(void *gcm, unsigned int pblocklen);
void ossl_gcm_gmult_avx512(u64 Xi[2], const void *gcm);

static block128_f block_function(void) {
  return AESNI_CAPABLE ? (block128_f)aesni_encrypt : (block128_f)AES_encrypt;
}

typedef struct {
  GCM128_CONTEXT gcm;
  AES_KEY ks;
  ctr128_f ctr;
  int vaes;
} Gcm;

static int gcm_init(TzCipher *c, const uint8_t *key, size_t key_length, const uint8_t *iv,
                    size_t iv_length) {
  Gcm *g = (Gcm *)c->ctx;
  if (ossl_vaes_vpclmulqdq_capable()) {
    aesni_set_encrypt_key(key, (int)key_length * 8, &g->ks);
    memset(&g->gcm, 0, sizeof g->gcm);
    g->gcm.key = &g->ks;
    g->vaes = 1;
    ossl_aes_gcm_init_avx512(&g->ks, &g->gcm);
    ossl_aes_gcm_setiv_avx512(&g->ks, &g->gcm, iv, iv_length);
    return 1;
  }
  encrypt_key(key, key_length, &g->ks);
  CRYPTO_gcm128_init(&g->gcm, &g->ks, block_function());
  g->ctr = AESNI_CAPABLE ? (ctr128_f)aesni_ctr32_encrypt_blocks : NULL;
  CRYPTO_gcm128_setiv(&g->gcm, iv, iv_length);

  return 1;
}

static int gcm_aad(TzCipher *c, const uint8_t *aad, size_t length, double plaintext_length) {
  (void)plaintext_length;
  Gcm *g = (Gcm *)c->ctx;
  if (!g->vaes) {
    return CRYPTO_gcm128_aad(&g->gcm, aad, length) == 0;
  }
  GCM128_CONTEXT *gcm = &g->gcm;
  gcm->len.u[0] += length;
  unsigned int ares = gcm->ares;
  if (ares > 0) {
    while (ares > 0 && length > 0) {
      gcm->Xi.c[15 - ares] ^= *aad++;
      --length;
      ares = (ares + 1) % 16;
    }
    if (ares != 0) {
      gcm->ares = ares;
      return 1;
    }
    ossl_gcm_gmult_avx512(gcm->Xi.u, gcm);
  }
  size_t blocks = length & ~(size_t)15;
  if (blocks) {
    ossl_aes_gcm_update_aad_avx512(gcm, aad, blocks);
    aad += blocks;
    length -= blocks;
  }
  for (size_t i = 0; i < length; ++i) {
    gcm->Xi.c[15 - i] ^= aad[i];
  }
  gcm->ares = (unsigned int)length;

  return 1;
}

static int64_t gcm_update(TzCipher *c, const uint8_t *in, size_t length, uint8_t *out) {
  Gcm *g = (Gcm *)c->ctx;
  GCM128_CONTEXT *gcm = &g->gcm;
  if (g->vaes) {
    gcm->len.u[1] += length;
    if (gcm->ares > 0) {
      ossl_gcm_gmult_avx512(gcm->Xi.u, gcm);
      gcm->ares = 0;
    }
    if (c->decrypt) {
      ossl_aes_gcm_decrypt_avx512(gcm->key, gcm, &gcm->mres, in, length, out);
    } else {
      ossl_aes_gcm_encrypt_avx512(gcm->key, gcm, &gcm->mres, in, length, out);
    }
    return (int64_t)length;
  }
  size_t bulk = 0;
  int stitched =
      g->ctr == (ctr128_f)aesni_ctr32_encrypt_blocks && gcm->funcs.ghash == gcm_ghash_avx;
  if (g->ctr && stitched && length >= (c->decrypt ? 16u : 32u)) {
    size_t res = (16 - gcm->mres) % 16;
    if ((c->decrypt ? CRYPTO_gcm128_decrypt : CRYPTO_gcm128_encrypt)(gcm, in, out, res)) {
      return -1;
    }
    bulk = (c->decrypt ? aesni_gcm_decrypt : aesni_gcm_encrypt)(in + res, out + res, length - res,
                                                                gcm->key, gcm->Yi.c, gcm->Xi.u);
    gcm->len.u[1] += bulk;
    bulk += res;
  }
  int failed;
  if (g->ctr) {
    failed = (c->decrypt ? CRYPTO_gcm128_decrypt_ctr32 : CRYPTO_gcm128_encrypt_ctr32)(
        gcm, in + bulk, out + bulk, length - bulk, g->ctr);
  } else {
    failed = (c->decrypt ? CRYPTO_gcm128_decrypt : CRYPTO_gcm128_encrypt)(gcm, in, out, length);
  }
  if (failed) {
    if (!c->quiet) {
      tz_throw(TZ_ERROR, "Trying to add data in unsupported state");
    }
    return -1;
  }

  return (int64_t)length;
}

static int64_t gcm_final(TzCipher *c, uint8_t *out) {
  (void)out;
  Gcm *g = (Gcm *)c->ctx;
  GCM128_CONTEXT *gcm = &g->gcm;
  int ok;
  if (g->vaes) {
    unsigned int *res = gcm->ares > 0 ? &gcm->ares : &gcm->mres;
    ossl_aes_gcm_finalize_avx512(gcm, *res);
    if (c->decrypt) {
      ok = CRYPTO_memcmp(gcm->Xi.c, c->tag, (size_t)c->tag_length) == 0;
    } else {
      memcpy(c->tag, gcm->Xi.c, 16);
      ok = 1;
    }
  } else if (c->decrypt) {
    ok = CRYPTO_gcm128_finish(gcm, c->tag, (size_t)c->tag_length) == 0;
  } else {
    CRYPTO_gcm128_tag(gcm, c->tag, 16);
    ok = 1;
  }
  if (!ok) {
    if (!c->quiet) {
      tz_throw(TZ_ERROR, "Unsupported state or unable to authenticate data");
    }
    return -1;
  }

  return 0;
}

static const TzCipherOps gcm_ops = {gcm_init, gcm_aad, gcm_update, gcm_final, NULL};

#define GCM(bits)                                                                                  \
  const TzCipherAlg tz_crypto_cipher_aes_##bits##_gcm = {                                          \
      &gcm_ops, NAMED(bits, "gcm"), sizeof(Gcm), bits / 8, 12, 1, TZ_CIPHER_AEAD, 1, NULL};
GCM(128)
GCM(192)
GCM(256)

#endif

#ifdef TZ_AES_CBC

typedef struct {
  AES_KEY ks;
  uint8_t iv[16];
} Cbc;

static int cbc_init(TzCipher *c, const uint8_t *key, size_t key_length, const uint8_t *iv,
                    size_t iv_length) {
  Cbc *k = (Cbc *)c->ctx;
  int bits = (int)key_length * 8;
  if (AESNI_CAPABLE) {
    (c->decrypt ? aesni_set_decrypt_key : aesni_set_encrypt_key)(key, bits, &k->ks);
  } else {
    (c->decrypt ? AES_set_decrypt_key : AES_set_encrypt_key)(key, bits, &k->ks);
  }
  memcpy(k->iv, iv, iv_length);

  return 1;
}

static void cbc_blocks(TzCipher *c, const uint8_t *in, size_t length, uint8_t *out) {
  Cbc *k = (Cbc *)c->ctx;
  if (AESNI_CAPABLE) {
    aesni_cbc_encrypt(in, out, length, &k->ks, k->iv, !c->decrypt);
  } else {
    AES_cbc_encrypt(in, out, length, &k->ks, k->iv, !c->decrypt);
  }
}

static int64_t cbc_update(TzCipher *c, const uint8_t *in, size_t length, uint8_t *out) {
  return tz_crypto_block_update(c, in, length, out, cbc_blocks);
}

static int64_t cbc_final(TzCipher *c, uint8_t *out) {
  return tz_crypto_block_final(c, out, cbc_blocks);
}

static const TzCipherOps cbc_ops = {cbc_init, NULL, cbc_update, cbc_final, NULL};

#define CBC(bits)                                                                                  \
  const TzCipherAlg tz_crypto_cipher_aes_##bits##_cbc = {                                          \
      &cbc_ops, NAMED(bits, "cbc"), sizeof(Cbc), bits / 8, 16, 16, TZ_CIPHER_BLOCK, 0, NULL};
CBC(128)
CBC(192)
CBC(256)

#endif

#ifdef TZ_AES_CTR

typedef struct {
  AES_KEY ks;
  uint8_t counter[16];
  uint8_t stream[16];
  unsigned int used;
} Ctr;

static int ctr_init(TzCipher *c, const uint8_t *key, size_t key_length, const uint8_t *iv,
                    size_t iv_length) {
  Ctr *k = (Ctr *)c->ctx;
  encrypt_key(key, key_length, &k->ks);
  memcpy(k->counter, iv, iv_length);

  return 1;
}

static int64_t ctr_update(TzCipher *c, const uint8_t *in, size_t length, uint8_t *out) {
  Ctr *k = (Ctr *)c->ctx;
  if (AESNI_CAPABLE) {
    CRYPTO_ctr128_encrypt_ctr32(in, out, length, &k->ks, k->counter, k->stream, &k->used,
                                (ctr128_f)aesni_ctr32_encrypt_blocks);
  } else {
    CRYPTO_ctr128_encrypt(in, out, length, &k->ks, k->counter, k->stream, &k->used,
                          (block128_f)AES_encrypt);
  }

  return (int64_t)length;
}

static int64_t ctr_final(TzCipher *c, uint8_t *out) {
  (void)c;
  (void)out;

  return 0;
}

static const TzCipherOps ctr_ops = {ctr_init, NULL, ctr_update, ctr_final, NULL};

#define CTR(bits)                                                                                  \
  const TzCipherAlg tz_crypto_cipher_aes_##bits##_ctr = {                                          \
      &ctr_ops, NAMED(bits, "ctr"), sizeof(Ctr), bits / 8, 16, 1, TZ_CIPHER_STREAM, 0, NULL};
CTR(128)
CTR(192)
CTR(256)

#endif
