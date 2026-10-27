#pragma once

#define TZ_NO_THREAD_STATE
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/bytes/bytes/bytes.h"
#include "runtime/library/json/json/json.h"
#include <stddef.h>
#include <stdint.h>

typedef struct TzMdOps {
  void (*init)(void *ctx);
  void (*update)(void *ctx, const uint8_t *data, size_t length);
  void (*final)(void *ctx, uint8_t *out, size_t length);
  void (*copy)(void *to, const void *from);
  void (*cleanup)(void *ctx);
} TzMdOps;

typedef struct TzMd {
  const TzMdOps *ops;
  const char *name;
  uint32_t ctx_size;
  uint32_t size;
  uint32_t block;
  uint32_t xof;
  const void *seed;
  uint32_t seed_size;
} TzMd;

typedef struct TzHash {
  int64_t rc;
  const TzLayout *layout;
  const TzMd *md;
  int32_t length;
  int32_t done;
  _Alignas(16) uint8_t ctx[];
} TzHash;

TzHash *tz_crypto_hash_alloc(const TzMd *md, int hmac);
void tz_crypto_pbkdf2_raw(const TzMd *md, const uint8_t *password, size_t password_length,
                          const uint8_t *salt, size_t salt_length, double iterations, uint8_t *out,
                          size_t length);
int tz_crypto_check_integer(double value, const char *name, double min, double max);
void tz_crypto_md_start(const TzMd *md, void *ctx);
void tz_crypto_md_end(const TzMd *md, void *ctx);
void tz_crypto_digest_raw(const TzMd *md, const uint8_t *data, size_t length, uint8_t *out);
void tz_crypto_hmac_raw(const TzMd *md, const uint8_t *key, size_t key_length, const uint8_t *data,
                        size_t length, uint8_t *out);
void tz_crypto_hkdf_raw(const TzMd *md, const uint8_t *key, size_t key_length, const uint8_t *salt,
                        size_t salt_length, const uint8_t *info, size_t info_length, uint8_t *out,
                        size_t length);

typedef struct TzCipher TzCipher;

typedef struct TzCipherOps {
  int (*init)(TzCipher *c, const uint8_t *key, size_t key_length, const uint8_t *iv,
              size_t iv_length);
  int (*aad)(TzCipher *c, const uint8_t *data, size_t length, double plaintext_length);
  int64_t (*update)(TzCipher *c, const uint8_t *in, size_t length, uint8_t *out);
  int64_t (*final)(TzCipher *c, uint8_t *out);
  void (*cleanup)(TzCipher *c);
} TzCipherOps;

enum {
  TZ_CIPHER_STREAM = 0,
  TZ_CIPHER_BLOCK = 1,
  TZ_CIPHER_AEAD = 2,
};

typedef struct TzCipherAlg {
  const TzCipherOps *ops;
  const char *name;
  uint32_t ctx_size;
  uint32_t key_length;
  uint32_t iv_length;
  uint32_t block;
  uint32_t kind;
  uint32_t iv_any;
  const void *seed;
} TzCipherAlg;

struct TzCipher {
  int64_t rc;
  const TzLayout *layout;
  const TzCipherAlg *alg;
  int32_t decrypt;
  int32_t state;
  int32_t padding;
  int32_t tag_length;
  int32_t tag_set;
  int32_t pending;
  uint8_t tag[16];
  uint8_t block[16];
  uint8_t last[16];
  int32_t has_last;
  int32_t aad_set;
  int32_t quiet;
  int32_t has_problem;
  char problem_code[48];
  char problem[208];
  _Alignas(16) uint8_t ctx[];
};

enum { TZ_KEY_SECRET = 0, TZ_KEY_PUBLIC = 1, TZ_KEY_PRIVATE = 2 };

typedef struct TzKey TzKey;

typedef struct TzKeyOps {
  void (*release)(void *pkey);
  int32_t (*equals)(const TzKey *a, const TzKey *b);
  void (*asymmetric_type)(TzStr *out, const TzKey *key);
  void (*export_pem)(TzStr *out, const TzKey *key, const char *type, const char *cipher,
                     const uint8_t *passphrase, size_t passphrase_length);
  TzBytes *(*export_der)(const TzKey *key, const char *type, const char *cipher,
                         const uint8_t *passphrase, size_t passphrase_length);
  TzJson *(*export_jwk)(const TzKey *key);
} TzKeyOps;

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

typedef struct TzCryptoData {
  const uint8_t *data;
  size_t length;
  TzBytes *made;
} TzCryptoData;

int tz_crypto_data(TzCryptoData *out, const TzType *type, const void *value, int32_t encoding,
                   const char *what);
void tz_crypto_data_done(TzCryptoData *d);

void tz_crypto_errors(TzMaker make);
void tz_crypto_fail(const char *code, const char *format, ...);
void tz_openssl_fail(const char *code, const char *otherwise);

TzBytes *tz_crypto_buffer(const uint8_t *data, size_t length);
void tz_crypto_text(TzStr *out, const uint8_t *data, size_t length, int32_t encoding);
size_t tz_crypto_cstr(void *obj, uint64_t meta, char *out, size_t capacity);

#define TZ_CRYPTO_LAYOUT(drop, name) {(drop), NULL, 0, TZ_LAYOUT_TIMER, (name), sizeof(name) - 1, 0}

typedef void (*TzBlocks)(TzCipher *c, const uint8_t *in, size_t length, uint8_t *out);
int64_t tz_crypto_block_update(TzCipher *c, const uint8_t *in, size_t length, uint8_t *out,
                               TzBlocks blocks);
int64_t tz_crypto_block_final(TzCipher *c, uint8_t *out, TzBlocks blocks);

TzBytes *tz_buffer_alloc(int64_t size, int32_t zero);
TzArrayBuffer *tz_abuf_new(int64_t length);
TzJson *tz_json_object_new(void);
void tz_json_object_put(TzJson *o, void *key_obj, uint64_t key_meta, TzJson *value);
TzJson *tz_json_string(void *obj, uint64_t meta);
TzJson *tz_json_get(const TzJson *j, void *key_obj, uint64_t key_meta);

void tz_crypto_random(uint8_t *out, size_t length);

enum {
  TZ_ENC_UTF8 = 0,
  TZ_ENC_UTF16LE,
  TZ_ENC_LATIN1,
  TZ_ENC_ASCII,
  TZ_ENC_BASE64,
  TZ_ENC_BASE64URL,
  TZ_ENC_HEX
};
