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
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/rsa.h>
#include <stdlib.h>
#include <string.h>

TzKey *tz_openssl_key_of(void *pkey, int private_key);

enum { SIGN_PKCS1 = 0, SIGN_PSS = 1, SIGN_ECDSA = 2, SIGN_EDDSA = 3 };
enum { MAKES_BYTES, MAKES_VERDICT, MAKES_KEY };

typedef struct KeyJob {
  TzWork work;
  TzPromise *promise;
  void (*run)(struct KeyJob *j);
  int makes;
  EVP_PKEY *key, *peer;
  char md[32];
  char curve[32];
  int32_t kind, salt_length, flag;
  double bits, exponent;
  uint8_t *data, *extra;
  size_t data_length, extra_length;
  int has_extra;
  uint8_t *out;
  size_t out_length;
  EVP_PKEY *made;
  int32_t result;
  int failed;
} KeyJob;

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

static KeyJob *job_new(TzPromise *p, int makes, const TzKey *key) {
  KeyJob *j = (KeyJob *)tz_alloc(sizeof(KeyJob));
  memset(j, 0, sizeof *j);
  j->promise = p;
  tz_ref_retain(p);
  j->makes = makes;
  if (key && key->pkey) {
    j->key = (EVP_PKEY *)key->pkey;
    EVP_PKEY_up_ref(j->key);
  }

  return j;
}

static void job_free(KeyJob *j) {
  if (j->data) {
    memset(j->data, 0, j->data_length);
    free(j->data);
  }
  if (j->extra) {
    memset(j->extra, 0, j->extra_length);
    free(j->extra);
  }
  if (j->out) {
    OPENSSL_cleanse(j->out, j->out_length);
    free(j->out);
  }
  EVP_PKEY_free(j->key);
  EVP_PKEY_free(j->peer);
  EVP_PKEY_free(j->made);
  tz_obj_release((TzObject *)j->promise);
  tz_free(j);
}

static int job_take(KeyJob *j, const TzType *dt, const void *dv, const TzType *xt, const void *xv) {
  TzCryptoData d, x = {NULL, 0, NULL};
  if (!tz_crypto_data(&d, dt, dv, TZ_ENC_UTF8, "data") ||
      (xt && !tz_crypto_data(&x, xt, xv, TZ_ENC_UTF8, "data"))) {
    void *error = tz_error_take();
    tz_promise_reject(j->promise, error);
    job_free(j);
    return 0;
  }
  j->data = copy(d.data, d.length);
  j->data_length = d.length;
  if (xt) {
    j->extra = copy(x.data, x.length);
    j->extra_length = x.length;
    j->has_extra = 1;
  }
  tz_crypto_data_done(&x);
  tz_crypto_data_done(&d);

  return 1;
}

static void job_work(TzWork *w) {
  KeyJob *j = (KeyJob *)w;
  j->run(j);
  ERR_clear_error();
}

static void job_done(TzWork *w) {
  KeyJob *j = (KeyJob *)w;
  char *value = (char *)j->promise + TZ_PROMISE_VALUE;
  if (j->makes == MAKES_VERDICT) {
    int32_t result = j->failed ? -1 : j->result;
    memcpy(value, &result, sizeof result);
  } else if (j->makes == MAKES_KEY) {
    TzKey *key = NULL;
    if (!j->failed && j->made) {
      key = tz_openssl_key_of(j->made, 1);
      j->made = NULL;
    }
    memcpy(value, &key, sizeof key);
  } else {
    TzArrayBuffer *out = NULL;
    if (!j->failed) {
      out = tz_abuf_new((int64_t)j->out_length);
      if (j->out_length) {
        memcpy(out->data, j->out, j->out_length);
      }
    }
    memcpy(value, &out, sizeof out);
  }
  tz_promise_fulfill(j->promise);
  job_free(j);
}

static void job_queue(KeyJob *j, void (*run)(KeyJob *)) {
  j->run = run;
  j->work.work = job_work;
  j->work.done = job_done;
  j->work.turns = 1;
  tz_work_queue(&j->work);
}

static void name_of(char *out, size_t size, void *obj, uint64_t meta) {
  tz_crypto_cstr(obj, meta, out, size);
}

static int p1363_half(const EVP_PKEY *pkey) { return (EVP_PKEY_get_bits(pkey) + 7) / 8; }

static EVP_MD_CTX *sign_context(KeyJob *j, int verify) {
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  EVP_PKEY_CTX *pctx = NULL;
  const char *md = j->md[0] ? j->md : NULL;
  int ok = ctx && (verify ? EVP_DigestVerifyInit_ex(ctx, &pctx, md, NULL, NULL, j->key, NULL)
                          : EVP_DigestSignInit_ex(ctx, &pctx, md, NULL, NULL, j->key, NULL)) > 0;
  if (ok && (j->kind == SIGN_PKCS1 || j->kind == SIGN_PSS)) {
    int pss = j->kind == SIGN_PSS;
    ok = EVP_PKEY_CTX_set_rsa_padding(pctx, pss ? RSA_PKCS1_PSS_PADDING : RSA_PKCS1_PADDING) > 0 &&
         (!pss || EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, j->salt_length) > 0);
  }
  if (!ok) {
    EVP_MD_CTX_free(ctx);
    return NULL;
  }

  return ctx;
}

static void sign_run(KeyJob *j) {
  EVP_MD_CTX *ctx = sign_context(j, 0);
  size_t length = 0;
  if (!ctx || EVP_DigestSign(ctx, NULL, &length, j->data, j->data_length) <= 0) {
    EVP_MD_CTX_free(ctx);
    j->failed = 1;
    return;
  }
  uint8_t *sig = (uint8_t *)malloc(length ? length : 1);
  if (!sig) {
    tz_panic("out of memory", -1);
  }
  int ok = EVP_DigestSign(ctx, sig, &length, j->data, j->data_length) > 0;
  EVP_MD_CTX_free(ctx);
  if (ok && j->kind == SIGN_ECDSA) {
    int half = p1363_half(j->key);
    const unsigned char *p = sig;
    ECDSA_SIG *s = d2i_ECDSA_SIG(NULL, &p, (long)length);
    uint8_t *raw = (uint8_t *)malloc((size_t)(2 * half));
    ok = s && raw;
    if (ok) {
      const BIGNUM *r, *ss;
      ECDSA_SIG_get0(s, &r, &ss);
      ok = BN_bn2binpad(r, raw, half) == half && BN_bn2binpad(ss, raw + half, half) == half;
    }
    ECDSA_SIG_free(s);
    free(sig);
    sig = raw;
    length = (size_t)(2 * half);
  }
  if (!ok) {
    free(sig);
    j->failed = 1;
    return;
  }
  j->out = sig;
  j->out_length = length;
}

static void verify_run(KeyJob *j) {
  EVP_MD_CTX *ctx = sign_context(j, 1);
  if (!ctx) {
    j->failed = 1;
    return;
  }
  const uint8_t *sig = j->extra;
  size_t length = j->extra_length;
  unsigned char *der = NULL;
  if (j->kind == SIGN_ECDSA) {
    int half = p1363_half(j->key);
    if (length != (size_t)(2 * half)) {
      EVP_MD_CTX_free(ctx);
      j->result = 0;
      return;
    }
    ECDSA_SIG *s = ECDSA_SIG_new();
    ECDSA_SIG_set0(s, BN_bin2bn(sig, half, NULL), BN_bin2bn(sig + half, half, NULL));
    int n = i2d_ECDSA_SIG(s, &der);
    ECDSA_SIG_free(s);
    if (n <= 0) {
      EVP_MD_CTX_free(ctx);
      j->result = 0;
      return;
    }
    sig = der;
    length = (size_t)n;
  }
  j->result = EVP_DigestVerify(ctx, sig, length, j->data, j->data_length) == 1;
  OPENSSL_free(der);
  EVP_MD_CTX_free(ctx);
}

void tz_openssl_subtle_sign(TzPromise *p, const TzKey *key, int32_t kind, void *md_obj,
                            uint64_t md_meta, int32_t salt_length, const TzType *dt,
                            const void *dv) {
  KeyJob *j = job_new(p, MAKES_BYTES, key);
  j->kind = kind;
  j->salt_length = salt_length;
  name_of(j->md, sizeof j->md, md_obj, md_meta);
  if (job_take(j, dt, dv, NULL, NULL)) {
    job_queue(j, sign_run);
  }
}

void tz_openssl_subtle_verify(TzPromise *p, const TzKey *key, int32_t kind, void *md_obj,
                              uint64_t md_meta, int32_t salt_length, const TzType *st,
                              const void *sv, const TzType *dt, const void *dv) {
  KeyJob *j = job_new(p, MAKES_VERDICT, key);
  j->kind = kind;
  j->salt_length = salt_length;
  name_of(j->md, sizeof j->md, md_obj, md_meta);
  if (job_take(j, dt, dv, st, sv)) {
    job_queue(j, verify_run);
  }
}

static void oaep_run(KeyJob *j) {
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(j->key, NULL);
  const EVP_MD *md = EVP_get_digestbyname(j->md);
  int ok = ctx && md && (j->flag ? EVP_PKEY_decrypt_init(ctx) : EVP_PKEY_encrypt_init(ctx)) > 0 &&
           EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) > 0 &&
           EVP_PKEY_CTX_set_rsa_oaep_md(ctx, md) > 0 && EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, md) > 0;
  if (ok && j->has_extra && j->extra_length) {
    unsigned char *label = (unsigned char *)OPENSSL_malloc(j->extra_length);
    ok = label != NULL;
    if (ok) {
      memcpy(label, j->extra, j->extra_length);
      ok = EVP_PKEY_CTX_set0_rsa_oaep_label(ctx, label, (int)j->extra_length) > 0;
      if (!ok) {
        OPENSSL_free(label);
      }
    }
  }
  size_t length = 0;
  if (ok) {
    ok = (j->flag ? EVP_PKEY_decrypt(ctx, NULL, &length, j->data, j->data_length)
                  : EVP_PKEY_encrypt(ctx, NULL, &length, j->data, j->data_length)) > 0;
  }
  uint8_t *out = NULL;
  if (ok) {
    out = (uint8_t *)malloc(length ? length : 1);
    ok = out && (j->flag ? EVP_PKEY_decrypt(ctx, out, &length, j->data, j->data_length)
                         : EVP_PKEY_encrypt(ctx, out, &length, j->data, j->data_length)) > 0;
  }
  EVP_PKEY_CTX_free(ctx);
  if (!ok) {
    free(out);
    j->failed = 1;
    return;
  }
  j->out = out;
  j->out_length = length;
}

void tz_openssl_subtle_rsa_oaep(TzPromise *p, const TzKey *key, void *md_obj, uint64_t md_meta,
                                const TzType *lt, const void *lv, int32_t decrypt, const TzType *dt,
                                const void *dv) {
  KeyJob *j = job_new(p, MAKES_BYTES, key);
  j->flag = decrypt;
  name_of(j->md, sizeof j->md, md_obj, md_meta);
  if (job_take(j, dt, dv, lt, lv)) {
    job_queue(j, oaep_run);
  }
}

static void derive_run(KeyJob *j) {
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(j->key, NULL);
  size_t length = 0;
  int ok = ctx && EVP_PKEY_derive_init(ctx) > 0 && EVP_PKEY_derive_set_peer(ctx, j->peer) > 0 &&
           EVP_PKEY_derive(ctx, NULL, &length) > 0;
  uint8_t *out = NULL;
  if (ok) {
    out = (uint8_t *)malloc(length ? length : 1);
    ok = out && EVP_PKEY_derive(ctx, out, &length) > 0;
  }
  EVP_PKEY_CTX_free(ctx);
  if (!ok) {
    free(out);
    j->failed = 1;
    return;
  }
  j->out = out;
  j->out_length = length;
}

void tz_openssl_subtle_derive(TzPromise *p, const TzKey *key, const TzKey *peer) {
  KeyJob *j = job_new(p, MAKES_BYTES, key);
  if (peer->pkey) {
    j->peer = (EVP_PKEY *)peer->pkey;
    EVP_PKEY_up_ref(j->peer);
  }
  job_queue(j, derive_run);
}

enum { PAIR_RSA = 0, PAIR_EC = 1, PAIR_ED25519 = 2, PAIR_X25519 = 3 };

static void generate_run(KeyJob *j) {
  static const char *const names[] = {"RSA", "EC", "ED25519", "X25519"};
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, names[j->kind], NULL);
  int ok = ctx && EVP_PKEY_keygen_init(ctx) > 0;
  if (ok && j->kind == PAIR_RSA) {
    BIGNUM *e = BN_new();
    ok = e && BN_set_word(e, (BN_ULONG)j->exponent) &&
         EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, (int)j->bits) > 0 &&
         EVP_PKEY_CTX_set1_rsa_keygen_pubexp(ctx, e) > 0;
    BN_free(e);
  } else if (ok && j->kind == PAIR_EC) {
    int nid = EC_curve_nist2nid(j->curve);
    ok = nid != NID_undef && EVP_PKEY_CTX_set_group_name(ctx, OBJ_nid2sn(nid)) > 0;
  }
  EVP_PKEY *pkey = NULL;
  if (ok) {
    ok = EVP_PKEY_generate(ctx, &pkey) > 0;
  }
  EVP_PKEY_CTX_free(ctx);
  if (!ok) {
    EVP_PKEY_free(pkey);
    j->failed = 1;
    return;
  }
  j->made = pkey;
}

void tz_openssl_subtle_generate(TzPromise *p, int32_t kind, double bits, double exponent,
                                void *curve_obj, uint64_t curve_meta) {
  KeyJob *j = job_new(p, MAKES_KEY, NULL);
  j->kind = kind;
  j->bits = bits;
  j->exponent = exponent;
  name_of(j->curve, sizeof j->curve, curve_obj, curve_meta);
  job_queue(j, generate_run);
}
