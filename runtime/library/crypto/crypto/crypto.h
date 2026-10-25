// node:crypto (docs/crypto.md): what the parts of the glue between programs and OpenSSL share.
//
// The glue is a library of its own, tzcrypto.lib, that tz links only into programs that use
// node:crypto, with one of two builds of OpenSSL's libcrypto (docs/crypto.md, section 3):
//
//   direct  libcrypto_direct.lib: the functions of the algorithms themselves, which a program
//           reaches when it names them in its code (`createHash("sha256")`). Each algorithm is
//           an object of its own here (crypto_md.c, crypto_aes.c...), so a program brings only
//           the ones it uses: tens of KB each.
//   full    libcrypto.lib, all of it: keys, signatures, certificates, and the algorithms whose
//           names are known only when the program runs, which OpenSSL looks up in its tables.
//           The functions of this layer are named tz_openssl_* (crypto_evp.c, crypto_keys.c,
//           crypto_x509.c); a program that calls one gets libcrypto.lib instead of the direct
//           build, which it contains.
//
// crypto.c holds what both layers share and needs no OpenSSL: the objects (a Hash, an Hmac, a
// Cipheriv, a KeyObject...), HMAC, PBKDF2 and HKDF over any hash, the random numbers (from the
// system, as node's come from OpenSSL's generator seeded by it), timingSafeEqual.
//
// Errors are those of node, with its messages (tz_throw): the function throws and returns, and
// the program checks after the call.

#pragma once

// Built once for both runtimes (see TZ_PARALLEL in tzrt.h).
#define TZ_NO_THREAD_STATE
#include "tzrt.h"

#include <stddef.h>
#include <stdint.h>

//===----------------------------------------------------------------------===//
// Hashes
//===----------------------------------------------------------------------===//

// The functions of a hash over its context: the low-level functions of OpenSSL for the direct
// layer, its EVP API for the full one. `final` writes `length` bytes: the size of the digest, or
// any length for an extendable-output function (SHAKE).
typedef struct TzMdOps {
  void (*init)(void *ctx);
  void (*update)(void *ctx, const uint8_t *data, size_t length);
  void (*final)(void *ctx, uint8_t *out, size_t length);
  // Copies a context into fresh memory of the same size; NULL for memcpy.
  void (*copy)(void *to, const void *from);
  // Frees what a context holds; NULL for nothing (the memory is cleansed).
  void (*cleanup)(void *ctx);
} TzMdOps;

// A hash algorithm: a constant tz_crypto_md_* of crypto_md.c (one object each), or one the full
// layer made for a name known only when the program runs (crypto_evp.c), whose `seed` holds the
// EVP_MD and is copied into each new context.
typedef struct TzMd {
  const TzMdOps *ops;
  const char *name; // as node names it in errors
  uint32_t ctx_size;
  uint32_t size;    // of the digest; for an XOF, its default length
  uint32_t block;   // for HMAC
  uint32_t xof;     // whether the length of the output is the caller's
  const void *seed; // copied to the start of each context before init, or NULL
  uint32_t seed_size;
} TzMd;

// A Hash or an Hmac: the context of the algorithm follows the header; an Hmac keeps the inner
// context, keyed, then the outer one.
typedef struct TzHash {
  int64_t rc;
  const TzLayout *layout;
  const TzMd *md;
  int32_t length; // of the digest
  int32_t done;   // digest() was called
  _Alignas(16) uint8_t ctx[];
} TzHash;

TzHash *tz_crypto_hash_alloc(const TzMd *md, int hmac);
// PBKDF2 of `md` into `out`, for scrypt and the threads of the pool.
void tz_crypto_pbkdf2_raw(const TzMd *md, const uint8_t *password, size_t password_length,
                          const uint8_t *salt, size_t salt_length, double iterations, uint8_t *out,
                          size_t length);
// The integer checks of node, with its messages, for the KDFs.
int tz_crypto_check_integer(double value, const char *name, double min, double max);
// Starts a context of `md` in `ctx`.
void tz_crypto_md_start(const TzMd *md, void *ctx);
void tz_crypto_md_end(const TzMd *md, void *ctx);
// The digest, HMAC (md->size bytes into `out`) and HKDF of data at once, for the threads of the
// pool too: they use the memory of the system, not the allocator of the runtime.
void tz_crypto_digest_raw(const TzMd *md, const uint8_t *data, size_t length, uint8_t *out);
void tz_crypto_hmac_raw(const TzMd *md, const uint8_t *key, size_t key_length, const uint8_t *data,
                        size_t length, uint8_t *out);
void tz_crypto_hkdf_raw(const TzMd *md, const uint8_t *key, size_t key_length, const uint8_t *salt,
                        size_t salt_length, const uint8_t *info, size_t info_length, uint8_t *out,
                        size_t length);

//===----------------------------------------------------------------------===//
// Ciphers
//===----------------------------------------------------------------------===//

typedef struct TzCipher TzCipher;

// The functions of a cipher over its context, which follows the TzCipher. `update` writes up to
// length + block bytes and returns how many; `final` up to a block, and -1 when the data does not
// authenticate or the padding is wrong (after it threw).
typedef struct TzCipherOps {
  // The key and the IV were checked against the lengths of the algorithm.
  int (*init)(TzCipher *c, const uint8_t *key, size_t key_length, const uint8_t *iv,
              size_t iv_length);
  // setAAD(data, { plaintextLength }): the length is -1 when not given (CCM needs it).
  int (*aad)(TzCipher *c, const uint8_t *data, size_t length, double plaintext_length);
  int64_t (*update)(TzCipher *c, const uint8_t *in, size_t length, uint8_t *out);
  int64_t (*final)(TzCipher *c, uint8_t *out);
  void (*cleanup)(TzCipher *c);
} TzCipherOps;

enum {
  TZ_CIPHER_STREAM = 0, // CTR, ChaCha20: no padding, the output follows the input
  TZ_CIPHER_BLOCK = 1,  // CBC, ECB: padding, output in whole blocks
  TZ_CIPHER_AEAD = 2,   // GCM, CCM, OCB, ChaCha20-Poly1305: an authentication tag
};

typedef struct TzCipherAlg {
  const TzCipherOps *ops;
  const char *name;
  uint32_t ctx_size;
  uint32_t key_length;
  uint32_t iv_length; // the default; GCM and OCB take others (see iv_check)
  uint32_t block;     // 1 for a stream
  uint32_t kind;      // TZ_CIPHER_*
  uint32_t iv_any;    // GCM: any IV of at least one byte; OCB: 1 to 15
  const void *seed;   // for the full layer: its EVP_CIPHER, which init uses
} TzCipherAlg;

struct TzCipher {
  int64_t rc;
  const TzLayout *layout;
  const TzCipherAlg *alg;
  int32_t decrypt;
  int32_t state;      // 0 before any data, 1 after update, 2 after final
  int32_t padding;    // setAutoPadding
  int32_t tag_length; // of the tag, for an AEAD
  int32_t tag_set;    // setAuthTag was called (a Decipheriv)
  int32_t pending;    // bytes of a partial block held back (CBC)
  uint8_t tag[16];
  uint8_t block[16]; // the partial block
  uint8_t last[16];  // a decipher holds back the last whole block for the padding
  int32_t has_last;
  int32_t aad_set;
  // A cipher of a job of the pool (Web Crypto, crypto_subtle.c), which must not throw there: a
  // failure is only the -1 of `final`.
  int32_t quiet;
  _Alignas(16) uint8_t ctx[];
};

//===----------------------------------------------------------------------===//
// Keys
//===----------------------------------------------------------------------===//

enum { TZ_KEY_SECRET = 0, TZ_KEY_PUBLIC = 1, TZ_KEY_PRIVATE = 2 };

typedef struct TzKey TzKey;

// What an asymmetric key does, which only the full layer can: it sets these when it makes one,
// so that a program with only secret keys needs no OpenSSL.
typedef struct TzKeyOps {
  void (*release)(void *pkey);
  int32_t (*equals)(const TzKey *a, const TzKey *b);
  void (*asymmetric_type)(TzStr *out, const TzKey *key);
  // export({ format: 'pem' | 'der', type, cipher, passphrase }); type and cipher as strings,
  // empty for none.
  void (*export_pem)(TzStr *out, const TzKey *key, const char *type, const char *cipher,
                     const uint8_t *passphrase, size_t passphrase_length);
  TzBytes *(*export_der)(const TzKey *key, const char *type, const char *cipher,
                         const uint8_t *passphrase, size_t passphrase_length);
  TzJson *(*export_jwk)(const TzKey *key);
} TzKeyOps;

// A KeyObject: secret bytes, or an EVP_PKEY of the full layer with its ops.
struct TzKey {
  int64_t rc;
  const TzLayout *layout;
  int32_t type;
  int32_t length;
  uint8_t *secret;
  void *pkey;
  const TzKeyOps *ops;
};

extern const TzLayout tz_crypto_secret_key_layout;
extern const TzLayout tz_crypto_public_key_layout;
extern const TzLayout tz_crypto_private_key_layout;

//===----------------------------------------------------------------------===//
// Data
//===----------------------------------------------------------------------===//

// The bytes of a value a function takes as data: a string in an encoding (numbered as in
// buffer.c), a typed array, a Buffer, a DataView, an ArrayBuffer, or a secret KeyObject.
typedef struct TzCryptoData {
  const uint8_t *data;
  size_t length;
  TzBytes *made; // the bytes of a string, which tz_crypto_data_done releases
} TzCryptoData;

// `type` and `value` as values of any type are passed (a union is resolved); `what` names the
// argument in the TypeError for anything else. A NULL type is undefined: no bytes.
int tz_crypto_data(TzCryptoData *out, const TzType *type, const void *value, int32_t encoding,
                   const char *what);
void tz_crypto_data_done(TzCryptoData *d);

// Strings and Buffers of results.
TzBytes *tz_crypto_buffer(const uint8_t *data, size_t length);
void tz_crypto_text(TzStr *out, const uint8_t *data, size_t length, int32_t encoding);
// A NUL-terminated copy of a string of the program in UTF-8, cut at `capacity`; returns its
// length.
size_t tz_crypto_cstr(void *obj, uint64_t meta, char *out, size_t capacity);

// The objects of node:crypto print as `Hash {}` (TZ_LAYOUT_TIMER prints a layout's name).
#define TZ_CRYPTO_LAYOUT(drop, name) {(drop), NULL, 0, TZ_LAYOUT_TIMER, (name), sizeof(name) - 1, 0}

// The update and final of a cipher of whole blocks with padding (CBC), as OpenSSL's providers do
// them: `blocks` ciphers whole blocks.
typedef void (*TzBlocks)(TzCipher *c, const uint8_t *in, size_t length, uint8_t *out);
int64_t tz_crypto_block_update(TzCipher *c, const uint8_t *in, size_t length, uint8_t *out,
                               TzBlocks blocks);
int64_t tz_crypto_block_final(TzCipher *c, uint8_t *out, TzBlocks blocks);

// What the glue uses of buffer.c, bytes.c and json.c.
TzBytes *tz_buffer_alloc(int64_t size, int32_t zero);
TzArrayBuffer *tz_abuf_new(int64_t length);
TzJson *tz_json_object_new(void);
void tz_json_object_put(TzJson *o, void *key_obj, uint64_t key_meta, TzJson *value);
TzJson *tz_json_string(void *obj, uint64_t meta);
TzJson *tz_json_get(const TzJson *j, void *key_obj, uint64_t key_meta);

// Random bytes of the system.
void tz_crypto_random(uint8_t *out, size_t length);

// Encodings, as numbered in buffer.c.
enum {
  TZ_ENC_UTF8 = 0,
  TZ_ENC_UTF16LE,
  TZ_ENC_LATIN1,
  TZ_ENC_ASCII,
  TZ_ENC_BASE64,
  TZ_ENC_BASE64URL,
  TZ_ENC_HEX
};
