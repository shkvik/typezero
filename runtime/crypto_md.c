// The hashes of the direct layer (docs/crypto.md, section 3): each a TzMd over the low-level
// functions of OpenSSL, which the program names when it names the hash in its code. The file is
// compiled once for each TZ_MD_* (runtime/CMakeLists.txt), so that each family is an object of
// its own and a program brings only the hashes it uses.

// The low-level functions are deprecated for programs; OpenSSL keeps them for itself, as this.
#include "internal/deprecated.h"

#include "crypto.h"

#include <openssl/md5.h>
#include <openssl/ripemd.h>
#include <openssl/sha.h>
#include <string.h>

#include "crypto/sha.h"
#include "internal/sha3.h"
#include "internal/sm3.h"

// A TzMd named `id` over functions of the form f(ctx), f(ctx, data, n) and f(out, ctx).
#define MD(id, label, ctx_type, init_fn, update_fn, final_fn, size_, block_)                       \
  static void md_##id##_init(void *c) { init_fn((ctx_type *)c); }                                  \
  static void md_##id##_update(void *c, const uint8_t *p, size_t n) {                              \
    update_fn((ctx_type *)c, p, n);                                                                \
  }                                                                                                \
  static void md_##id##_final(void *c, uint8_t *out, size_t n) {                                   \
    (void)n;                                                                                       \
    final_fn(out, (ctx_type *)c);                                                                  \
  }                                                                                                \
  static const TzMdOps md_##id##_ops = {md_##id##_init, md_##id##_update, md_##id##_final, NULL,   \
                                        NULL};                                                     \
  const TzMd tz_crypto_md_##id = {&md_##id##_ops, label, sizeof(ctx_type), size_, block_, 0,       \
                                  NULL,           0};

#ifdef TZ_MD_MD5
MD(md5, "md5", MD5_CTX, MD5_Init, MD5_Update, MD5_Final, 16, 64)
#endif

#ifdef TZ_MD_SHA1
MD(sha1, "sha1", SHA_CTX, SHA1_Init, SHA1_Update, SHA1_Final, 20, 64)
#endif

#ifdef TZ_MD_SHA256
MD(sha224, "sha224", SHA256_CTX, SHA224_Init, SHA224_Update, SHA224_Final, 28, 64)
MD(sha256, "sha256", SHA256_CTX, SHA256_Init, SHA256_Update, SHA256_Final, 32, 64)
#endif

#ifdef TZ_MD_SHA512
MD(sha384, "sha384", SHA512_CTX, SHA384_Init, SHA384_Update, SHA384_Final, 48, 128)
MD(sha512, "sha512", SHA512_CTX, SHA512_Init, SHA512_Update, SHA512_Final, 64, 128)
MD(sha512_224, "sha512-224", SHA512_CTX, sha512_224_init, SHA512_Update, SHA512_Final, 28, 128)
MD(sha512_256, "sha512-256", SHA512_CTX, sha512_256_init, SHA512_Update, SHA512_Final, 32, 128)
#endif

#ifdef TZ_MD_RIPEMD160
MD(ripemd160, "ripemd160", RIPEMD160_CTX, RIPEMD160_Init, RIPEMD160_Update, RIPEMD160_Final, 20, 64)
#endif

#ifdef TZ_MD_SM3
MD(sm3, "sm3", SM3_CTX, ossl_sm3_init, ossl_sm3_update, ossl_sm3_final, 32, 64)
#endif

#ifdef TZ_MD_MD5_SHA1
// md5-sha1 of TLS 1.0: MD5 and SHA-1 of the same data, side by side.
typedef struct {
  MD5_CTX md5;
  SHA_CTX sha1;
} Md5Sha1;
static void md5_sha1_init(void *c) {
  MD5_Init(&((Md5Sha1 *)c)->md5);
  SHA1_Init(&((Md5Sha1 *)c)->sha1);
}
static void md5_sha1_update(void *c, const uint8_t *p, size_t n) {
  MD5_Update(&((Md5Sha1 *)c)->md5, p, n);
  SHA1_Update(&((Md5Sha1 *)c)->sha1, p, n);
}
static void md5_sha1_final(void *c, uint8_t *out, size_t n) {
  (void)n;
  MD5_Final(out, &((Md5Sha1 *)c)->md5);
  SHA1_Final(out + 16, &((Md5Sha1 *)c)->sha1);
}
static const TzMdOps md5_sha1_ops = {md5_sha1_init, md5_sha1_update, md5_sha1_final, NULL, NULL};
const TzMd tz_crypto_md_md5_sha1 = {&md5_sha1_ops, "md5-sha1", sizeof(Md5Sha1), 36, 64, 0, NULL, 0};
#endif

#ifdef TZ_MD_SHA3
// SHA-3 and SHAKE: Keccak with the padding of each (0x06 and 0x1f) and the rate of its strength;
// `final` gives any length, which only SHAKE takes from the program.
#define KECCAK(id, label, pad, bits, size_, xof_)                                                  \
  static void id##_init(void *c) { ossl_sha3_init((KECCAK1600_CTX *)c, pad, bits); }               \
  static const TzMdOps id##_ops = {id##_init, keccak_update, keccak_final, NULL, NULL};            \
  const TzMd tz_crypto_md_##id = {                                                                 \
      &id##_ops, label, sizeof(KECCAK1600_CTX), size_, SHA3_BLOCKSIZE(bits), xof_, NULL, 0};

static void keccak_update(void *c, const uint8_t *p, size_t n) {
  ossl_sha3_update((KECCAK1600_CTX *)c, p, n);
}
static void keccak_final(void *c, uint8_t *out, size_t n) {
  ossl_sha3_final((KECCAK1600_CTX *)c, out, n);
}

KECCAK(sha3_224, "sha3-224", 0x06, 224, 28, 0)
KECCAK(sha3_256, "sha3-256", 0x06, 256, 32, 0)
KECCAK(sha3_384, "sha3-384", 0x06, 384, 48, 0)
KECCAK(sha3_512, "sha3-512", 0x06, 512, 64, 0)
KECCAK(shake128, "shake128", 0x1f, 128, 16, 1)
KECCAK(shake256, "shake256", 0x1f, 256, 32, 1)
#endif

#if defined(TZ_MD_BLAKE2B) || defined(TZ_MD_BLAKE2S)
// The BLAKE2 contexts of OpenSSL (providers/implementations/include/prov/blake2.h), whose header
// brings much of its internals with it.
typedef struct {
  uint8_t digest_length, key_length, fanout, depth, leaf_length[4];
#ifdef TZ_MD_BLAKE2B
  uint8_t node_offset[8], node_depth, inner_length, reserved[14], salt[16], personal[16];
#else
  uint8_t node_offset[6], node_depth, inner_length, salt[8], personal[8];
#endif
} Blake2Param;
#endif

#ifdef TZ_MD_BLAKE2B
typedef struct {
  uint64_t h[8], t[2], f[2];
  uint8_t buf[128];
  size_t buflen, outlen;
} Blake2bCtx;
void ossl_blake2b_param_init(Blake2Param *p);
int ossl_blake2b_init(Blake2bCtx *c, const Blake2Param *p);
int ossl_blake2b_update(Blake2bCtx *c, const void *data, size_t n);
int ossl_blake2b_final(unsigned char *md, Blake2bCtx *c);
static void blake2b_init(void *c) {
  Blake2Param p;
  ossl_blake2b_param_init(&p);
  ossl_blake2b_init((Blake2bCtx *)c, &p);
}
static void blake2b_update(void *c, const uint8_t *p, size_t n) {
  ossl_blake2b_update((Blake2bCtx *)c, p, n);
}
static void blake2b_final(void *c, uint8_t *out, size_t n) {
  (void)n;
  ossl_blake2b_final(out, (Blake2bCtx *)c);
}
static const TzMdOps blake2b_ops = {blake2b_init, blake2b_update, blake2b_final, NULL, NULL};
const TzMd tz_crypto_md_blake2b512 = {
    &blake2b_ops, "blake2b512", sizeof(Blake2bCtx), 64, 128, 0, NULL, 0};
#endif

#ifdef TZ_MD_BLAKE2S
typedef struct {
  uint32_t h[8], t[2], f[2];
  uint8_t buf[64];
  size_t buflen, outlen;
} Blake2sCtx;
void ossl_blake2s_param_init(Blake2Param *p);
int ossl_blake2s_init(Blake2sCtx *c, const Blake2Param *p);
int ossl_blake2s_update(Blake2sCtx *c, const void *data, size_t n);
int ossl_blake2s_final(unsigned char *md, Blake2sCtx *c);
static void blake2s_init(void *c) {
  Blake2Param p;
  ossl_blake2s_param_init(&p);
  ossl_blake2s_init((Blake2sCtx *)c, &p);
}
static void blake2s_update(void *c, const uint8_t *p, size_t n) {
  ossl_blake2s_update((Blake2sCtx *)c, p, n);
}
static void blake2s_final(void *c, uint8_t *out, size_t n) {
  (void)n;
  ossl_blake2s_final(out, (Blake2sCtx *)c);
}
static const TzMdOps blake2s_ops = {blake2s_init, blake2s_update, blake2s_final, NULL, NULL};
const TzMd tz_crypto_md_blake2s256 = {
    &blake2s_ops, "blake2s256", sizeof(Blake2sCtx), 32, 64, 0, NULL, 0};
#endif
