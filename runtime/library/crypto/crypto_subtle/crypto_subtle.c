#include "runtime/library/crypto/crypto/crypto.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/async/async.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/error/error.h"
#include "runtime/core/group/group.h"
#include "runtime/core/io/io.h"
#include "runtime/core/output/output.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/bytes/bytes/bytes.h"
#include <stdlib.h>
#include <string.h>

enum { DATA, KEY, EXTRA, MORE, PARTS };

typedef struct SubtleJob {
  TzWork work;
  TzPromise *promise;
  void (*run)(struct SubtleJob *j);
  const TzMd *md;
  const TzCipherAlg *alg;
  TzCipher *cipher;
  uint8_t *bytes[PARTS];
  size_t lengths[PARTS];
  TzArrayBuffer *out;
  size_t written;
  double count;
  int32_t flag;
  int32_t tag;
  int32_t bits;
  int32_t result;
  int failed;
} SubtleJob;

static SubtleJob *job_new(TzPromise *p) {
  SubtleJob *j = (SubtleJob *)tz_alloc(sizeof(SubtleJob));
  memset(j, 0, sizeof *j);
  j->promise = p;
  tz_ref_retain(p);

  return j;
}

static void job_free(SubtleJob *j);
static int job_copy(SubtleJob *j, int part, const TzType *type, const void *value) {
  TzCryptoData d;
  if (!tz_crypto_data(&d, type, value, TZ_ENC_UTF8, "data")) {
    tz_panicf("internal error: crypto data of a wrong type");
    job_free(j);
    return 0;
  }
  j->bytes[part] = (uint8_t *)malloc(d.length ? d.length : 1);
  if (!j->bytes[part]) {
    tz_panic("out of memory", -1);
  }
  if (d.length) {
    memcpy(j->bytes[part], d.data, d.length);
  }
  j->lengths[part] = d.length;
  tz_crypto_data_done(&d);

  return 1;
}

static void job_free(SubtleJob *j) {
  for (int i = 0; i < PARTS; ++i) {
    if (j->bytes[i]) {
      memset(j->bytes[i], 0, j->lengths[i]);
      free(j->bytes[i]);
    }
  }
  if (j->cipher) {
    memset(j->cipher, 0, sizeof(TzCipher) + j->alg->ctx_size);
    tz_obj_free(j->cipher);
  }
  if (j->out) {
    tz_instance_release((TzInstance *)j->out);
  }
  tz_obj_release((TzObject *)j->promise);
  tz_free(j);
}

static void job_work(TzWork *w) {
  SubtleJob *j = (SubtleJob *)w;
  if (!j->failed) {
    j->run(j);
  }
}

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

static void job_queue(SubtleJob *j, void (*run)(SubtleJob *), int64_t size) {
  if (size >= 0) {
    j->out = tz_abuf_new(size);
  }
  j->run = run;
  j->work.work = job_work;
  j->work.done = job_done;
  j->work.turns = 1;
  tz_work_queue(&j->work);
}

static void digest_run(SubtleJob *j) {
  tz_crypto_digest_raw(j->md, j->bytes[DATA], j->lengths[DATA], j->out->data);
  j->written = j->md->size;
}

void tz_crypto_subtle_digest(TzPromise *p, const TzMd *md, const TzType *dt, const void *dv) {
  SubtleJob *j = job_new(p);
  j->md = md;
  if (job_copy(j, DATA, dt, dv)) {
    job_queue(j, digest_run, md->size);
  }
}

static void hmac_run(SubtleJob *j) {
  tz_crypto_hmac_raw(j->md, j->bytes[KEY], j->lengths[KEY], j->bytes[DATA], j->lengths[DATA],
                     j->out->data);
  j->written = j->md->size;
}

static void hmac_verify_run(SubtleJob *j) {
  uint8_t mac[64];
  tz_crypto_hmac_raw(j->md, j->bytes[KEY], j->lengths[KEY], j->bytes[DATA], j->lengths[DATA], mac);
  int equal = j->lengths[EXTRA] == j->md->size;
  volatile uint8_t diff = 0;
  for (size_t i = 0; equal && i < j->md->size; ++i) {
    diff |= mac[i] ^ j->bytes[EXTRA][i];
  }
  j->result = equal && diff == 0;
  memset(mac, 0, sizeof mac);
}

void tz_crypto_subtle_hmac(TzPromise *p, const TzMd *md, const TzType *kt, const void *kv,
                           const TzType *dt, const void *dv) {
  SubtleJob *j = job_new(p);
  j->md = md;
  if (job_copy(j, KEY, kt, kv) && job_copy(j, DATA, dt, dv)) {
    job_queue(j, hmac_run, md->size);
  }
}

void tz_crypto_subtle_hmac_verify(TzPromise *p, const TzMd *md, const TzType *kt, const void *kv,
                                  const TzType *st, const void *sv, const TzType *dt,
                                  const void *dv) {
  SubtleJob *j = job_new(p);
  j->md = md;
  if (job_copy(j, KEY, kt, kv) && job_copy(j, EXTRA, st, sv) && job_copy(j, DATA, dt, dv)) {
    job_queue(j, hmac_verify_run, -1);
  }
}

static void pbkdf2_run(SubtleJob *j) {
  tz_crypto_pbkdf2_raw(j->md, j->bytes[KEY], j->lengths[KEY], j->bytes[EXTRA], j->lengths[EXTRA],
                       j->count, j->out->data, j->out->length);
  j->written = (size_t)j->out->length;
}

void tz_crypto_subtle_pbkdf2(TzPromise *p, const TzMd *md, const TzType *kt, const void *kv,
                             const TzType *st, const void *sv, double iterations, double length) {
  SubtleJob *j = job_new(p);
  j->md = md;
  j->count = iterations;
  if (job_copy(j, KEY, kt, kv) && job_copy(j, EXTRA, st, sv)) {
    job_queue(j, pbkdf2_run, (int64_t)length);
  }
}

static void hkdf_run(SubtleJob *j) {
  size_t length = (size_t)j->out->length;
  if (length > 255 * (size_t)j->md->size) {
    j->failed = 1;
    return;
  }
  tz_crypto_hkdf_raw(j->md, j->bytes[KEY], j->lengths[KEY], j->bytes[EXTRA], j->lengths[EXTRA],
                     j->bytes[MORE], j->lengths[MORE], j->out->data, length);
  j->written = length;
}

void tz_crypto_subtle_hkdf(TzPromise *p, const TzMd *md, const TzType *kt, const void *kv,
                           const TzType *st, const void *sv, const TzType *it, const void *iv,
                           double length) {
  SubtleJob *j = job_new(p);
  j->md = md;
  if (job_copy(j, KEY, kt, kv) && job_copy(j, EXTRA, st, sv) && job_copy(j, MORE, it, iv)) {
    job_queue(j, hkdf_run, (int64_t)length);
  }
}

static int cipher_start(SubtleJob *j, const uint8_t *iv, size_t iv_length) {
  TzCipher *c = j->cipher;
  if (j->lengths[KEY] != j->alg->key_length) {
    return 0;
  }
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
    length -= tag;
  }
  if (!j->lengths[EXTRA] || !cipher_start(j, j->bytes[EXTRA], j->lengths[EXTRA])) {
    j->failed = 1;
    return;
  }
  if (j->flag) {
    memcpy(c->tag, in + length, tag);
  }
  if (j->bytes[MORE] && j->lengths[MORE] &&
      !j->alg->ops->aad(c, j->bytes[MORE], j->lengths[MORE], -1)) {
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
  for (int i = 0; i < 16; ++i) {
    v = (v << 8) | b[i];
  }

  return v;
}

static void ctr_run(SubtleJob *j) {
  size_t length = j->lengths[DATA];
  u128 blocks = (length + 15) / 16;
  u128 mask = j->bits == 128 ? ~(u128)0 : (((u128)1 << j->bits) - 1);
  u128 counter = block_number(j->bytes[EXTRA]) & mask;
  u128 left = (mask - counter) + 1;
  if (j->bits < 128 && blocks > mask + 1) {
    j->failed = 1;
    return;
  }
  uint8_t *out = j->out->data;
  size_t first = length;
  if (j->bits < 128 && left < blocks) {
    first = (size_t)left * 16;
  }
  if (!cipher_start(j, j->bytes[EXTRA], 16)) {
    j->failed = 1;
    return;
  }
  if (first) {
    j->alg->ops->update(j->cipher, j->bytes[DATA], first, out);
  }
  if (first < length) {
    uint8_t zeroed[16];
    memcpy(zeroed, j->bytes[EXTRA], 16);
    for (int k = 0; k < j->bits; ++k) {
      zeroed[15 - k / 8] &= (uint8_t)~(1u << (k % 8));
    }
    cipher_start(j, zeroed, 16);
    j->alg->ops->update(j->cipher, j->bytes[DATA] + first, length - first, out + first);
  }
  j->written = length;
}

void tz_crypto_subtle_aes(TzPromise *p, const TzCipherAlg *alg, const TzType *kt, const void *kv,
                          int32_t decrypt, const TzType *it, const void *iv, const TzType *at,
                          const void *av, int32_t tag, int32_t bits, const TzType *dt,
                          const void *dv) {
  SubtleJob *j = job_new(p);
  j->alg = alg;
  j->flag = decrypt;
  j->tag = tag;
  j->bits = bits;
  if (!job_copy(j, KEY, kt, kv) || !job_copy(j, EXTRA, it, iv) || !job_copy(j, DATA, dt, dv)) {
    return;
  }
  if (at && !job_copy(j, MORE, at, av)) {
    return;
  }
  j->cipher = (TzCipher *)tz_obj_alloc((int64_t)(sizeof(TzCipher) + alg->ctx_size));
  memset(j->cipher, 0, sizeof(TzCipher) + alg->ctx_size);
  size_t length = j->lengths[DATA];
  switch (alg->kind) {
  case TZ_CIPHER_AEAD:
    if (decrypt && length < (size_t)tag) {
      j->failed = 1;
    }
    job_queue(j, gcm_run,
              decrypt ? (j->failed ? 0 : (int64_t)(length - (size_t)tag))
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
