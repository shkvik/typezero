// Web Crypto (docs/crypto.md, step 4): AES-KW of wrapKey and unwrapKey (RFC 3394), on a thread
// of the pool, over the AES of the direct layer. An object of its own: a program that does not
// wrap keys brings none of it, nor the AES it calls.

// The low-level AES functions are deprecated for programs; OpenSSL keeps them for itself.
#include "internal/deprecated.h"
#include "runtime/library/crypto/crypto/crypto.h"
#include <openssl/aes.h>
#include <stdlib.h>
#include <string.h>

typedef struct KwJob {
  TzWork work; // first: the pool knows the job by it
  TzPromise *promise;
  uint8_t *key, *data;
  size_t key_length, length;
  int32_t unwrap;
  TzArrayBuffer *out;
  int failed;
} KwJob;

static const uint8_t kDefaultIv[8] = {0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6};

static void kw_work(TzWork *w) {
  KwJob *j = (KwJob *)w;
  if (j->failed) {
    return;
  }
  size_t n = j->length / 8;
  int bits = (int)j->key_length * 8;
  AES_KEY ks;
  // A whole number of 64-bit blocks: at least two to wrap, and three (the IV and two) to unwrap.
  if ((j->key_length != 16 && j->key_length != 24 && j->key_length != 32) || j->length % 8 ||
      n < (j->unwrap ? 3u : 2u)) {
    j->failed = 1;
    return;
  }
  uint8_t a[8], b[16];
  uint8_t *r = j->out->data + (j->unwrap ? 0 : 8);
  if (!j->unwrap) {
    AES_set_encrypt_key(j->key, bits, &ks);
    memcpy(a, kDefaultIv, 8);
    memcpy(r, j->data, j->length);
    for (uint64_t t = 1, s = 0; s < 6; ++s) {
      for (size_t i = 0; i < n; ++i, ++t) {
        memcpy(b, a, 8);
        memcpy(b + 8, r + 8 * i, 8);
        AES_encrypt(b, b, &ks);
        memcpy(a, b, 8);
        for (int k = 0; k < 8; ++k) {
          a[7 - k] ^= (uint8_t)(t >> (8 * k));
        }
        memcpy(r + 8 * i, b + 8, 8);
      }
    }
    memcpy(j->out->data, a, 8);
  } else {
    n -= 1;
    AES_set_decrypt_key(j->key, bits, &ks);
    memcpy(a, j->data, 8);
    memcpy(r, j->data + 8, 8 * n);
    for (uint64_t t = 6 * (uint64_t)n, s = 0; s < 6; ++s) {
      for (size_t i = n; i-- > 0; --t) {
        for (int k = 0; k < 8; ++k) {
          a[7 - k] ^= (uint8_t)(t >> (8 * k));
        }
        memcpy(b, a, 8);
        memcpy(b + 8, r + 8 * i, 8);
        AES_decrypt(b, b, &ks);
        memcpy(a, b, 8);
        memcpy(r + 8 * i, b + 8, 8);
      }
    }
    // The integrity check, in constant time.
    volatile uint8_t diff = 0;
    for (int k = 0; k < 8; ++k) {
      diff |= a[k] ^ kDefaultIv[k];
    }
    j->failed = diff != 0;
  }
  memset(&ks, 0, sizeof ks);
  memset(b, 0, sizeof b);
}

static void kw_done(TzWork *w) {
  KwJob *j = (KwJob *)w;
  TzArrayBuffer *out = j->failed ? NULL : j->out;
  if (out) {
    out->length = (int32_t)(j->unwrap ? j->length - 8 : j->length + 8);
    j->out = NULL;
  }
  memcpy((char *)j->promise + TZ_PROMISE_VALUE, &out, sizeof out);
  tz_promise_fulfill(j->promise);
  memset(j->key, 0, j->key_length);
  free(j->key);
  memset(j->data, 0, j->length);
  free(j->data);
  if (j->out) {
    tz_instance_release((TzInstance *)j->out);
  }
  tz_obj_release((TzObject *)j->promise);
  tz_free(j);
}

static uint8_t *copy(const uint8_t *data, size_t length) {
  uint8_t *p = (uint8_t *)malloc(length ? length : 1);
  if (!p) {
    tz_panic("out of memory", -1);
  }
  if (length) {
    memcpy(p, data, length);
  }

  return p;
}

// wrapKey and unwrapKey with AES-KW: the key is a secret KeyObject, the data the key to wrap or
// the wrapped one; undefined for a failure.
void tz_crypto_subtle_aes_kw(TzPromise *p, const TzType *kt, const void *kv, int32_t unwrap,
                             const TzType *dt, const void *dv) {
  TzCryptoData key, data;
  if (!tz_crypto_data(&key, kt, kv, TZ_ENC_UTF8, "key") ||
      !tz_crypto_data(&data, dt, dv, TZ_ENC_UTF8, "data")) {
    void *error = tz_error_take();
    tz_promise_reject(p, error);
    return;
  }
  KwJob *j = (KwJob *)tz_alloc(sizeof(KwJob));
  memset(j, 0, sizeof *j);
  j->promise = p;
  tz_ref_retain(p);
  j->key = copy(key.data, key.length);
  j->key_length = key.length;
  j->data = copy(data.data, data.length);
  j->length = data.length;
  j->unwrap = unwrap;
  tz_crypto_data_done(&data);
  tz_crypto_data_done(&key);
  j->out = tz_abuf_new((int64_t)j->length + 8);
  if (!j->out) {
    // Longer than an ArrayBuffer can be: the job fails, as node's does.
    tz_instance_release((TzInstance *)tz_error_take());
    j->failed = 1;
    j->out = tz_abuf_new(0);
  }
  j->work.work = kw_work;
  j->work.done = kw_done;
  j->work.turns = 1;
  tz_work_queue(&j->work);
}
