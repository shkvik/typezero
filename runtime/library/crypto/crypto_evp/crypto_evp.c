#include "runtime/library/crypto/crypto/crypto.h"
#include "runtime/core/array/array.h"
#include "runtime/core/error/error.h"
#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *library_of(unsigned long error) {
  switch (ERR_GET_LIB(error)) {
  case ERR_LIB_SYS:
    return "SYS_";
  case ERR_LIB_BN:
    return "BN_";
  case ERR_LIB_RSA:
    return "RSA_";
  case ERR_LIB_DH:
    return "DH_";
  case ERR_LIB_EVP:
    return "EVP_";
  case ERR_LIB_BUF:
    return "BUF_";
  case ERR_LIB_OBJ:
    return "OBJ_";
  case ERR_LIB_PEM:
    return "PEM_";
  case ERR_LIB_DSA:
    return "DSA_";
  case ERR_LIB_X509:
    return "X509_";
  case ERR_LIB_ASN1:
    return "ASN1_";
  case ERR_LIB_CONF:
    return "CONF_";
  case ERR_LIB_CRYPTO:
    return "CRYPTO_";
  case ERR_LIB_EC:
    return "EC_";
  case ERR_LIB_BIO:
    return "BIO_";
  case ERR_LIB_PKCS7:
    return "PKCS7_";
  case ERR_LIB_X509V3:
    return "X509V3_";
  case ERR_LIB_PKCS12:
    return "PKCS12_";
  case ERR_LIB_RAND:
    return "RAND_";
  case ERR_LIB_DSO:
    return "DSO_";
  case ERR_LIB_OCSP:
    return "OCSP_";
  case ERR_LIB_UI:
    return "UI_";
  case ERR_LIB_COMP:
    return "COMP_";
  case ERR_LIB_ECDSA:
    return "ECDSA_";
  case ERR_LIB_ECDH:
    return "ECDH_";
  case ERR_LIB_OSSL_STORE:
    return "OSSL_STORE_";
  case ERR_LIB_CMS:
    return "CMS_";
  case ERR_LIB_TS:
    return "TS_";
  case ERR_LIB_HMAC:
    return "HMAC_";
  case ERR_LIB_CT:
    return "CT_";
  case ERR_LIB_ASYNC:
    return "ASYNC_";
  case ERR_LIB_KDF:
    return "KDF_";
  case ERR_LIB_SM2:
    return "SM2_";
  case ERR_LIB_USER:
    return "USER_";
  default:
    return "";
  }
}

void tz_openssl_fail(const char *code, const char *otherwise) {
  unsigned long error = ERR_get_error();
  ERR_clear_error();
  if (!error) {
    tz_crypto_fail(code, "%s", otherwise);
    return;
  }
  char text[256];
  ERR_error_string_n(error, text, sizeof text);
  const char *reason = ERR_reason_error_string(error);
  char name[128];
  snprintf(name, sizeof name, "ERR_OSSL_%s%s", library_of(error), reason ? reason : "UNKNOWN");
  for (char *at = name; *at; ++at) {
    *at = *at == ' ' ? '_' : (*at >= 'a' && *at <= 'z') ? (char)(*at - 'a' + 'A') : *at;
  }
  tz_crypto_fail(name, "%s", text);
}

static int unsupported(const char *name) {
  if (OBJ_sn2nid(name) == NID_undef && OBJ_ln2nid(name) == NID_undef) {
    return 0;
  }
  tz_crypto_fail("ERR_OSSL_EVP_UNSUPPORTED",
                 "error:0308010C:digital envelope routines::unsupported");

  return 1;
}

typedef struct {
  EVP_MD_CTX *ctx;
  EVP_MD *md;
} EvpMd;

static void evp_md_init(void *c) {
  EvpMd *e = (EvpMd *)c;
  e->ctx = EVP_MD_CTX_new();
  EVP_DigestInit_ex(e->ctx, e->md, NULL);
}
static void evp_md_update(void *c, const uint8_t *p, size_t n) {
  EVP_DigestUpdate(((EvpMd *)c)->ctx, p, n);
}
static void evp_md_final(void *c, uint8_t *out, size_t n) {
  EvpMd *e = (EvpMd *)c;
  if (EVP_MD_get_flags(e->md) & EVP_MD_FLAG_XOF) {
    EVP_DigestFinalXOF(e->ctx, out, n);
  } else {
    EVP_DigestFinal_ex(e->ctx, out, NULL);
  }
}
static void evp_md_copy(void *to, const void *from) {
  EvpMd *t = (EvpMd *)to;
  const EvpMd *f = (const EvpMd *)from;
  t->md = f->md;
  t->ctx = EVP_MD_CTX_new();
  EVP_MD_CTX_copy_ex(t->ctx, f->ctx);
}
static void evp_md_cleanup(void *c) {
  EVP_MD_CTX_free(((EvpMd *)c)->ctx);
  ((EvpMd *)c)->ctx = NULL;
}
static const TzMdOps evp_md_ops = {evp_md_init, evp_md_update, evp_md_final, evp_md_copy,
                                   evp_md_cleanup};

typedef struct Known {
  struct Known *next;
  char name[64];
  void *alg;
} Known;

static Known *known_mds, *known_ciphers;

static void *find_known(Known *list, const char *name) {
  for (Known *k = list; k; k = k->next) {
    if (strcmp(k->name, name) == 0) {
      return k->alg;
    }
  }

  return NULL;
}

static void add_known(Known **list, const char *name, void *alg) {
  Known *k = (Known *)calloc(1, sizeof *k);
  snprintf(k->name, sizeof k->name, "%s", name);
  k->alg = alg;
  k->next = *list;
  *list = k;
}

const TzMd *tz_openssl_md(void *obj, uint64_t meta, int32_t caller) {
  char name[64];
  tz_crypto_cstr(obj, meta, name, sizeof name);
  const TzMd *found = (const TzMd *)find_known(known_mds, name);
  if (found) {
    return found;
  }
  const EVP_MD *legacy = EVP_get_digestbyname(name);
  EVP_MD *md = EVP_MD_fetch(NULL, legacy ? EVP_MD_get0_name(legacy) : name, NULL);
  ERR_clear_error();
  if (!md) {
    if (!unsupported(name)) {
      if (caller == 0) {
        tz_crypto_fail("ERR_CRYPTO_INVALID_DIGEST", "Digest method not supported");
      } else if (caller == 1) {
        tz_crypto_fail("ERR_CRYPTO_INVALID_DIGEST", "Digest method %s is not supported", name);
      } else {
        tz_crypto_fail("ERR_CRYPTO_INVALID_DIGEST", "Invalid digest: %s", name);
      }
    }
    return NULL;
  }
  TzMd *made = (TzMd *)calloc(1, sizeof(TzMd) + sizeof(EvpMd) + strlen(name) + 1);
  EvpMd *seed = (EvpMd *)(made + 1);
  char *label = (char *)(seed + 1);
  strcpy(label, name);
  seed->md = md;
  made->ops = &evp_md_ops;
  made->name = label;
  made->ctx_size = sizeof(EvpMd);
  made->xof = (EVP_MD_get_flags(md) & EVP_MD_FLAG_XOF) != 0;
  made->size = (uint32_t)EVP_MD_get_size(md);
  if (made->xof) {
    made->size = strstr(name, "128") ? 16 : 32;
  }
  made->block = (uint32_t)EVP_MD_get_block_size(md);
  made->seed = seed;
  made->seed_size = sizeof(EvpMd);
  add_known(&known_mds, name, made);

  return made;
}

EVP_MD *tz_openssl_md_of(const TzMd *md) {
  if (md->ops == &evp_md_ops) {
    return ((const EvpMd *)md->seed)->md;
  }

  return NULL;
}

typedef struct {
  EVP_CIPHER_CTX *ctx;
  int ccm;
  int ccm_length_set;
  int ccm_failed;
  int tag_given;
} EvpCipher;

static int evp_init(TzCipher *c, const uint8_t *key, size_t key_length, const uint8_t *iv,
                    size_t iv_length) {
  (void)key_length;
  EvpCipher *e = (EvpCipher *)c->ctx;
  const EVP_CIPHER *cipher = (const EVP_CIPHER *)c->alg->seed;
  e->ctx = EVP_CIPHER_CTX_new();
  int mode = EVP_CIPHER_get_mode(cipher);
  e->ccm = mode == EVP_CIPH_CCM_MODE;
  int ok = EVP_CipherInit_ex(e->ctx, cipher, NULL, NULL, NULL, !c->decrypt);
  if (ok && c->alg->kind == TZ_CIPHER_AEAD) {
    ok = EVP_CIPHER_CTX_ctrl(e->ctx, EVP_CTRL_AEAD_SET_IVLEN, (int)iv_length, NULL) > 0;
  }
  if (ok && !c->decrypt && (e->ccm || mode == EVP_CIPH_OCB_MODE)) {
    ok = EVP_CIPHER_CTX_ctrl(e->ctx, EVP_CTRL_AEAD_SET_TAG, c->tag_length, NULL) > 0;
  }
  if (ok) {
    ok = EVP_CipherInit_ex(e->ctx, NULL, NULL, key, iv_length ? iv : NULL, !c->decrypt);
  }
  if (!ok) {
    tz_openssl_fail("ERR_CRYPTO_INVALID_IV", "Invalid initialization vector");
    EVP_CIPHER_CTX_free(e->ctx);
    e->ctx = NULL;
  }

  return ok;
}

static int give_tag(TzCipher *c, EvpCipher *e) {
  if (!c->decrypt || !c->tag_set || e->tag_given) {
    return 1;
  }
  e->tag_given = 1;

  return EVP_CIPHER_CTX_ctrl(e->ctx, EVP_CTRL_AEAD_SET_TAG, c->tag_length, c->tag) > 0;
}

static int ccm_length(TzCipher *c, EvpCipher *e, double length) {
  int out;
  if (!e->ccm || e->ccm_length_set) {
    return 1;
  }
  e->ccm_length_set = 1;

  return give_tag(c, e) && EVP_CipherUpdate(e->ctx, NULL, &out, NULL, (int)length) > 0;
}

static int evp_aad(TzCipher *c, const uint8_t *aad, size_t length, double plaintext_length) {
  EvpCipher *e = (EvpCipher *)c->ctx;
  if (e->ccm && plaintext_length < 0) {
    tz_crypto_fail("ERR_CRYPTO_INVALID_STATE",
                   "options.plaintextLength required for CCM mode with AAD");
    return 0;
  }
  int out;

  return ccm_length(c, e, plaintext_length) && give_tag(c, e) &&
         EVP_CipherUpdate(e->ctx, NULL, &out, aad, (int)length) > 0;
}

static int64_t evp_update(TzCipher *c, const uint8_t *in, size_t length, uint8_t *out) {
  EvpCipher *e = (EvpCipher *)c->ctx;
  EVP_CIPHER_CTX_set_padding(e->ctx, c->padding);
  int written = 0;
  if (!ccm_length(c, e, (double)length) || !give_tag(c, e)) {
    tz_openssl_fail("ERR_CRYPTO_INVALID_STATE", "Trying to add data in unsupported state");
    return -1;
  }
  if (!EVP_CipherUpdate(e->ctx, out, &written, in, (int)length)) {
    if (e->ccm && c->decrypt) {
      ERR_clear_error();
      e->ccm_failed = 1;
      return 0;
    }
    tz_openssl_fail("ERR_CRYPTO_INVALID_STATE", "Trying to add data in unsupported state");
    return -1;
  }

  return written;
}

static int64_t evp_final(TzCipher *c, uint8_t *out) {
  EvpCipher *e = (EvpCipher *)c->ctx;
  EVP_CIPHER_CTX_set_padding(e->ctx, c->padding);
  int aead = c->alg->kind == TZ_CIPHER_AEAD;
  if (e->ccm && c->decrypt) {
    if (!e->ccm_failed) {
      return 0;
    }
    tz_crypto_fail("ERR_CRYPTO_AUTH_FAILED", "Unsupported state or unable to authenticate data");
    return -1;
  }
  int written = 0;
  if (!give_tag(c, e) || !EVP_CipherFinal_ex(e->ctx, out, &written)) {
    if (aead) {
      ERR_clear_error();
      tz_crypto_fail("ERR_CRYPTO_AUTH_FAILED", "Unsupported state or unable to authenticate data");
    } else {
      tz_openssl_fail("ERR_CRYPTO_INVALID_STATE", "Unsupported state");
    }
    return -1;
  }
  if (aead && !c->decrypt) {
    EVP_CIPHER_CTX_ctrl(e->ctx, EVP_CTRL_AEAD_GET_TAG, c->tag_length, c->tag);
  }

  return written;
}

static void evp_cleanup(TzCipher *c) {
  EvpCipher *e = (EvpCipher *)c->ctx;
  EVP_CIPHER_CTX_free(e->ctx);
  e->ctx = NULL;
}

static const TzCipherOps evp_cipher_ops = {evp_init, evp_aad, evp_update, evp_final, evp_cleanup};

const TzCipherAlg *tz_openssl_cipher(void *obj, uint64_t meta) {
  char name[64];
  tz_crypto_cstr(obj, meta, name, sizeof name);
  const TzCipherAlg *found = (const TzCipherAlg *)find_known(known_ciphers, name);
  if (found) {
    return found;
  }
  const EVP_CIPHER *legacy = EVP_get_cipherbyname(name);
  EVP_CIPHER *cipher = EVP_CIPHER_fetch(NULL, legacy ? EVP_CIPHER_get0_name(legacy) : name, NULL);
  ERR_clear_error();
  if (!cipher) {
    if (!unsupported(name)) {
      tz_crypto_fail("ERR_CRYPTO_UNKNOWN_CIPHER", "Unknown cipher");
    }
    return NULL;
  }
  TzCipherAlg *made = (TzCipherAlg *)calloc(1, sizeof(TzCipherAlg) + strlen(name) + 1);
  char *label = (char *)(made + 1);
  strcpy(label, name);
  int mode = EVP_CIPHER_get_mode(cipher);
  int aead = (EVP_CIPHER_get_flags(cipher) & EVP_CIPH_FLAG_AEAD_CIPHER) != 0;
  made->ops = &evp_cipher_ops;
  made->name = label;
  made->ctx_size = sizeof(EvpCipher);
  made->key_length = (uint32_t)EVP_CIPHER_get_key_length(cipher);
  made->iv_length = (uint32_t)EVP_CIPHER_get_iv_length(cipher);
  made->block = (uint32_t)EVP_CIPHER_get_block_size(cipher);
  made->kind = aead ? TZ_CIPHER_AEAD : made->block > 1 ? TZ_CIPHER_BLOCK : TZ_CIPHER_STREAM;
  made->iv_any = mode == EVP_CIPH_GCM_MODE                                  ? 1
                 : (mode == EVP_CIPH_OCB_MODE || mode == EVP_CIPH_CCM_MODE) ? 2
                                                                            : 0;
  made->seed = cipher;
  add_known(&known_ciphers, name, made);

  return made;
}

typedef struct {
  char **names;
  size_t count, capacity;
} Names;

static void add_name(Names *n, const char *name) {
  if (n->count == n->capacity) {
    n->capacity = n->capacity ? 2 * n->capacity : 64;
    n->names = (char **)realloc(n->names, n->capacity * sizeof(char *));
  }
  size_t length = strlen(name) + 1;
  n->names[n->count] = (char *)malloc(length);
  memcpy(n->names[n->count++], name, length);
}

static void push_md(const EVP_MD *md, const char *from, const char *to, void *arg) {
  (void)md;
  (void)to;
  if (!from) {
    return;
  }
  const EVP_MD *real = EVP_get_digestbyname(from);
  const char *name = real ? EVP_MD_get0_name(real) : NULL;
  EVP_MD *fetched = name ? EVP_MD_fetch(NULL, name, NULL) : NULL;
  ERR_clear_error();
  if (!fetched) {
    return;
  }
  EVP_MD_free(fetched);
  add_name((Names *)arg, from);
}

static void push_cipher(const EVP_CIPHER *cipher, const char *from, const char *to, void *arg) {
  (void)cipher;
  (void)to;
  if (!from) {
    return;
  }
  const EVP_CIPHER *real = EVP_get_cipherbyname(from);
  const char *name = real ? EVP_CIPHER_get0_name(real) : NULL;
  EVP_CIPHER *fetched = name ? EVP_CIPHER_fetch(NULL, name, NULL) : NULL;
  ERR_clear_error();
  if (!fetched) {
    return;
  }
  EVP_CIPHER_free(fetched);
  add_name((Names *)arg, from);
}

static int compare_names(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static int same_lower(const char *a, const char *b) {
  for (; *a && *b; ++a, ++b) {
    char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
    char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
    if (x != y) {
      return 0;
    }
  }

  return *a == *b;
}

static TzArray *name_array(Names *n) {
  size_t kept = 0;
  for (size_t i = 0; i < n->count; ++i) {
    size_t k = 0;
    while (k < kept && !same_lower(n->names[k], n->names[i])) {
      ++k;
    }
    if (k < kept) {
      free(n->names[k]);
      n->names[k] = n->names[i];
    } else {
      n->names[kept++] = n->names[i];
    }
  }
  qsort(n->names, kept, sizeof(char *), compare_names);
  TzArray *array = tz_arr_new(&tz_string_type, 0, (int64_t)kept);
  for (size_t i = 0; i < kept; ++i) {
    TzStr s = tz_str_from_ascii(n->names[i], strlen(n->names[i]));
    tz_arr_push_value(array, &s);
    free(n->names[i]);
  }
  free(n->names);

  return array;
}

TzArray *tz_openssl_get_hashes(void) {
  Names n = {0};
  EVP_MD_do_all_sorted(push_md, &n);

  return name_array(&n);
}

TzArray *tz_openssl_get_ciphers(void) {
  Names n = {0};
  EVP_CIPHER_do_all_sorted(push_cipher, &n);

  return name_array(&n);
}

TzArray *tz_openssl_get_curves(void) {
  Names n = {0};
  size_t count = EC_get_builtin_curves(NULL, 0);
  EC_builtin_curve *curves = (EC_builtin_curve *)calloc(count, sizeof *curves);
  EC_get_builtin_curves(curves, count);
  for (size_t i = 0; i < count; ++i) {
    add_name(&n, OBJ_nid2sn(curves[i].nid));
  }
  free(curves);

  return name_array(&n);
}
