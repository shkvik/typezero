// Web Crypto (docs/crypto.md, step 4): the work of crypto.subtle that takes time, which runs on
// the threads of the pool (runtime/core/io/io.c), as node runs it on those of libuv. This file is
// the direct layer: digests, HMAC, PBKDF2, HKDF and AES-GCM / CBC / CTR over the hashes and ciphers
// the caller names (crypto_md.c, crypto_aes.c); crypto_subtle_kw.c has AES-KW and
// crypto_subtle_keys.c the full layer. The code of crypto.subtle (library/crypto/webcrypto.tz)
// calls them through the intrinsics __tz_subtle_* (sema_crypto.cpp), each with the promise it
// settles first.
//
// The main thread copies what the thread reads, as the data may change meanwhile (the spec has
// the bytes copied too), and makes the ArrayBuffer the thread writes the result into; the thread
// touches nothing else of the runtime. A job that fails fulfills its promise with undefined (a
// verification with -1), which the library turns into node's OperationError.

#include "runtime/library/crypto/crypto/crypto.h"

#include <stdlib.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Jobs
//===----------------------------------------------------------------------===//

enum { DATA, KEY, EXTRA, MORE, PARTS };

typedef struct SubtleJob {
  TzWork work; // first: the pool knows the job by it
  TzPromise *promise;
  void (*run)(struct SubtleJob *j);
  const TzMd *md;
  const TzCipherAlg *alg;
  TzCipher *cipher;
  // Copies of the data, the key, and of the salt, IV or signature and the info or AAD.
  uint8_t *bytes[PARTS];
  size_t lengths[PARTS];
  TzArrayBuffer *out; // the result, which `written` bytes of are made
  size_t written;
  double count;  // iterations, or the length of the output
  int32_t flag;  // decrypt
  int32_t tag;   // bytes of the tag of AES-GCM
  int32_t bits;  // of the counter of AES-CTR
  int32_t result; // of a verification: 1, 0, or -1 for a failure
  int failed;
} SubtleJob;

static SubtleJob *job_new(TzPromise *p) {
  SubtleJob *j = (SubtleJob *)tz_alloc(sizeof(SubtleJob));
  memset(j, 0, sizeof *j);
  j->promise = p;
  tz_ref_retain(p);
  return j;
}

// A copy of the bytes of `value` as part `part` of the job: false with the promise rejected
// (the value was no bytes, which the library's types rule out) and the job freed.
static void job_free(SubtleJob *j);
static int job_copy(SubtleJob *j, int part, const TzType *type, const void *value) {
  TzCryptoData d;
  if (!tz_crypto_data(&d, type, value, TZ_ENC_UTF8, "data")) {
    void *error = tz_error_take();
    tz_promise_reject(j->promise, error);
    job_free(j);
    return 0;
  }
  j->bytes[part] = (uint8_t *)malloc(d.length ? d.length : 1);
  if (!j->bytes[part])
    tz_panic("out of memory", -1);
  if (d.length)
    memcpy(j->bytes[part], d.data, d.length);
  j->lengths[part] = d.length;
  tz_crypto_data_done(&d);
  return 1;
}

static void job_free(SubtleJob *j) {
  for (int i = 0; i < PARTS; ++i)
    if (j->bytes[i]) {
      memset(j->bytes[i], 0, j->lengths[i]);
      free(j->bytes[i]);
    }
  if (j->cipher) {
    memset(j->cipher, 0, sizeof(TzCipher) + j->alg->ctx_size);
    tz_obj_free(j->cipher);
  }
  if (j->out)
    tz_instance_release((TzInstance *)j->out);
  tz_obj_release((TzObject *)j->promise);
  tz_free(j);
}

static void job_work(TzWork *w) {
  SubtleJob *j = (SubtleJob *)w;
  // One that failed already (its result could not be made) only comes back.
  if (!j->failed)
    j->run(j);
}

// Back on the main thread: the ArrayBuffer of the result, cut to what was written, or
// undefined; a verification gives its number.
static void job_done(TzWork *w) {
  SubtleJob *j = (SubtleJob *)w;
  if (j->out) {
    TzArrayBuffer *out = j->failed ? NULL : j->out;
    if (out) {
      out->length = (int32_t)j->written;
      j->out = NULL;
    }
    memcpy((char *)j->promise + TZ_PROMISE_VALUE, &out, sizeof out);
  } else {
    int32_t result = j->failed ? -1 : j->result;
    memcpy((char *)j->promise + TZ_PROMISE_VALUE, &result, sizeof result);
  }
  tz_promise_fulfill(j->promise);
  job_free(j);
}

// Queues the job, whose result is an ArrayBuffer of `size` bytes at most (or, with -1, a
// verification).
static void job_queue(SubtleJob *j, void (*run)(SubtleJob *), int64_t size) {
  if (size >= 0) {
    j->out = tz_abuf_new(size);
    if (!j->out) {
      // Longer than an ArrayBuffer can be: the job fails, as node's does.
      tz_instance_release((TzInstance *)tz_error_take());
      j->failed = 1;
      j->out = tz_abuf_new(0);
    }
  }
  j->run = run;
  j->work.work = job_work;
  j->work.done = job_done;
  j->work.turns = 1;
  tz_work_queue(&j->work);
}

//===----------------------------------------------------------------------===//
// Digests, HMAC, PBKDF2 and HKDF
//===----------------------------------------------------------------------===//

static void digest_run(SubtleJob *j) {
  tz_crypto_digest_raw(j->md, j->bytes[DATA], j->lengths[DATA], j->out->data);
  j->written = j->md->size;
}

// digest(algorithm, data).
void tz_crypto_subtle_digest(TzPromise *p, const TzMd *md, const TzType *dt, const void *dv) {
  SubtleJob *j = job_new(p);
  j->md = md;
  if (job_copy(j, DATA, dt, dv))
    job_queue(j, digest_run, md->size);
}

static void hmac_run(SubtleJob *j) {
  tz_crypto_hmac_raw(j->md, j->bytes[KEY], j->lengths[KEY], j->bytes[DATA], j->lengths[DATA],
                     j->out->data);
  j->written = j->md->size;
}

static void hmac_verify_run(SubtleJob *j) {
  uint8_t mac[64];
  tz_crypto_hmac_raw(j->md, j->bytes[KEY], j->lengths[KEY], j->bytes[DATA], j->lengths[DATA],
                     mac);
  // In constant time, as CRYPTO_memcmp does it for node.
  int equal = j->lengths[EXTRA] == j->md->size;
  volatile uint8_t diff = 0;
  for (size_t i = 0; equal && i < j->md->size; ++i)
    diff |= mac[i] ^ j->bytes[EXTRA][i];
  j->result = equal && diff == 0;
  memset(mac, 0, sizeof mac);
}

// sign('HMAC', key, data) and verify('HMAC', key, signature, data); the key is a secret
// KeyObject (data, as tz_crypto_data reads it).
void tz_crypto_subtle_hmac(TzPromise *p, const TzMd *md, const TzType *kt, const void *kv,
                           const TzType *dt, const void *dv) {
  SubtleJob *j = job_new(p);
  j->md = md;
  if (job_copy(j, KEY, kt, kv) && job_copy(j, DATA, dt, dv))
    job_queue(j, hmac_run, md->size);
}

void tz_crypto_subtle_hmac_verify(TzPromise *p, const TzMd *md, const TzType *kt, const void *kv,
                                  const TzType *st, const void *sv, const TzType *dt,
                                  const void *dv) {
  SubtleJob *j = job_new(p);
  j->md = md;
  if (job_copy(j, KEY, kt, kv) && job_copy(j, EXTRA, st, sv) && job_copy(j, DATA, dt, dv))
    job_queue(j, hmac_verify_run, -1);
}

static void pbkdf2_run(SubtleJob *j) {
  tz_crypto_pbkdf2_raw(j->md, j->bytes[KEY], j->lengths[KEY], j->bytes[EXTRA], j->lengths[EXTRA],
                       j->count, j->out->data, j->out->length);
  j->written = (size_t)j->out->length;
}

// deriveBits({ name: 'PBKDF2', hash, salt, iterations }, key, length): `length` bytes, which the
// library checked.
void tz_crypto_subtle_pbkdf2(TzPromise *p, const TzMd *md, const TzType *kt, const void *kv,
                             const TzType *st, const void *sv, double iterations, double length) {
  SubtleJob *j = job_new(p);
  j->md = md;
  j->count = iterations;
  if (job_copy(j, KEY, kt, kv) && job_copy(j, EXTRA, st, sv))
    job_queue(j, pbkdf2_run, (int64_t)length);
}

static void hkdf_run(SubtleJob *j) {
  size_t length = (size_t)j->out->length;
  // RFC 5869 makes at most 255 blocks.
  if (length > 255 * (size_t)j->md->size) {
    j->failed = 1;
    return;
  }
  tz_crypto_hkdf_raw(j->md, j->bytes[KEY], j->lengths[KEY], j->bytes[EXTRA], j->lengths[EXTRA],
                     j->bytes[MORE], j->lengths[MORE], j->out->data, length);
  j->written = length;
}

// deriveBits({ name: 'HKDF', hash, salt, info }, key, length).
void tz_crypto_subtle_hkdf(TzPromise *p, const TzMd *md, const TzType *kt, const void *kv,
                           const TzType *st, const void *sv, const TzType *it, const void *iv,
                           double length) {
  SubtleJob *j = job_new(p);
  j->md = md;
  if (job_copy(j, KEY, kt, kv) && job_copy(j, EXTRA, st, sv) && job_copy(j, MORE, it, iv))
    job_queue(j, hkdf_run, (int64_t)length);
}

//===----------------------------------------------------------------------===//
// AES-GCM, AES-CBC and AES-CTR
//===----------------------------------------------------------------------===//

// The cipher, made ready by the thread: false (the job failed) for a key of another length.
static int cipher_start(SubtleJob *j, const uint8_t *iv, size_t iv_length) {
  TzCipher *c = j->cipher;
  if (j->lengths[KEY] != j->alg->key_length)
    return 0;
  memset(c, 0, sizeof(TzCipher) + j->alg->ctx_size);
  c->alg = j->alg;
  c->decrypt = j->flag;
  c->padding = 1;
  c->quiet = 1;
  c->tag_length = j->tag ? j->tag : 16;
  return j->alg->ops->init(c, j->bytes[KEY], j->lengths[KEY], iv, iv_length);
}

static void gcm_run(SubtleJob *j) {
  TzCipher *c = j->cipher;
  const uint8_t *in = j->bytes[DATA];
  size_t length = j->lengths[DATA], tag = (size_t)j->tag;
  if (j->flag) {
    // The tag ends the data; the library checked there is one.
    length -= tag;
  }
  if (!j->lengths[EXTRA] || !cipher_start(j, j->bytes[EXTRA], j->lengths[EXTRA])) {
    j->failed = 1;
    return;
  }
  if (j->flag)
    memcpy(c->tag, in + length, tag);
  if (j->bytes[MORE] && j->lengths[MORE] && !j->alg->ops->aad(c, j->bytes[MORE], j->lengths[MORE], -1)) {
    j->failed = 1;
    return;
  }
  uint8_t *out = j->out->data;
  if (length && j->alg->ops->update(c, in, length, out) < 0) {
    j->failed = 1;
    return;
  }
  if (j->alg->ops->final(c, out + length) < 0) {
    j->failed = 1;
    return;
  }
  j->written = length;
  if (!j->flag) {
    memcpy(out + length, c->tag, tag);
    j->written += tag;
  }
}

static void cbc_run(SubtleJob *j) {
  TzCipher *c = j->cipher;
  if (!cipher_start(j, j->bytes[EXTRA], j->lengths[EXTRA])) {
    j->failed = 1;
    return;
  }
  uint8_t *out = j->out->data;
  int64_t n = j->alg->ops->update(c, j->bytes[DATA], j->lengths[DATA], out);
  int64_t last = n < 0 ? -1 : j->alg->ops->final(c, out + n);
  if (last < 0) {
    j->failed = 1;
    return;
  }
  j->written = (size_t)(n + last);
}

typedef unsigned __int128 u128;

static u128 block_number(const uint8_t *b) {
  u128 v = 0;
  for (int i = 0; i < 16; ++i)
    v = (v << 8) | b[i];
  return v;
}

// AES-CTR whose counter is the rightmost `bits` bits of the counter block, which wraps around
// to zero without touching the rest, as the spec has it; node and Chromium fail when the data
// needs more blocks than the counter has values.
static void ctr_run(SubtleJob *j) {
  size_t length = j->lengths[DATA];
  u128 blocks = (length + 15) / 16;
  u128 mask = j->bits == 128 ? ~(u128)0 : (((u128)1 << j->bits) - 1);
  u128 counter = block_number(j->bytes[EXTRA]) & mask;
  // The values left before the counter wraps: 2^bits - counter.
  u128 left = (mask - counter) + 1;
  if (j->bits < 128 && blocks > mask + 1) {
    j->failed = 1;
    return;
  }
  uint8_t *out = j->out->data;
  size_t first = length;
  if (j->bits < 128 && left < blocks)
    first = (size_t)left * 16;
  if (!cipher_start(j, j->bytes[EXTRA], 16)) {
    j->failed = 1;
    return;
  }
  if (first)
    j->alg->ops->update(j->cipher, j->bytes[DATA], first, out);
  if (first < length) {
    // The counter, wrapped to zero.
    uint8_t zeroed[16];
    memcpy(zeroed, j->bytes[EXTRA], 16);
    for (int k = 0; k < j->bits; ++k)
      zeroed[15 - k / 8] &= (uint8_t) ~(1u << (k % 8));
    cipher_start(j, zeroed, 16);
    j->alg->ops->update(j->cipher, j->bytes[DATA] + first, length - first, out + first);
  }
  j->written = length;
}

// encrypt and decrypt of AES-GCM ({ iv, additionalData, tagLength } with the tag in bytes),
// AES-CBC ({ iv }) and AES-CTR ({ counter, length }) with a cipher of the direct layer; the key
// is a secret KeyObject.
void tz_crypto_subtle_aes(TzPromise *p, const TzCipherAlg *alg, const TzType *kt, const void *kv,
                          int32_t decrypt, const TzType *it, const void *iv, const TzType *at,
                          const void *av, int32_t tag, int32_t bits, const TzType *dt,
                          const void *dv) {
  SubtleJob *j = job_new(p);
  j->alg = alg;
  j->flag = decrypt;
  j->tag = tag;
  j->bits = bits;
  if (!job_copy(j, KEY, kt, kv) || !job_copy(j, EXTRA, it, iv) || !job_copy(j, DATA, dt, dv))
    return;
  if (at && !job_copy(j, MORE, at, av))
    return;
  j->cipher = (TzCipher *)tz_obj_alloc((int64_t)(sizeof(TzCipher) + alg->ctx_size));
  memset(j->cipher, 0, sizeof(TzCipher) + alg->ctx_size);
  size_t length = j->lengths[DATA];
  switch (alg->kind) {
  case TZ_CIPHER_AEAD:
    // Data shorter than its tag, which the library rejects first, fails.
    if (decrypt && length < (size_t)tag)
      j->failed = 1;
    job_queue(j, gcm_run, decrypt ? (j->failed ? 0 : (int64_t)(length - (size_t)tag))
                                  : (int64_t)(length + (size_t)tag));
    break;
  case TZ_CIPHER_BLOCK:
    job_queue(j, cbc_run, (int64_t)(decrypt ? length : length + 16 - length % 16));
    break;
  default:
    job_queue(j, ctr_run, (int64_t)length);
    break;
  }
}
