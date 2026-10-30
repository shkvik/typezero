#include "runtime/library/crypto/crypto/crypto.h"
#include "runtime/core/array/array.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/error/error.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/bytes/bytes/bytes.h"
#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <string.h>
#include <time.h>

typedef struct {
  int64_t rc;
  const TzLayout *layout;
  X509 *cert;
} Certificate;

static void certificate_drop(void *object) {
  Certificate *c = (Certificate *)object;
  X509_free(c->cert);
  tz_obj_free(c);
}

static const TzLayout certificate_layout = TZ_CRYPTO_LAYOUT(certificate_drop, "X509Certificate");

Certificate *tz_openssl_x509_new(const TzType *type, const void *value) {
  TzCryptoData d;
  if (!tz_crypto_data(&d, type, value, TZ_ENC_UTF8, "buffer")) {
    return NULL;
  }
  X509 *cert = NULL;
  if (tz_crypto_looks_like_pem(d.data, d.length)) {
    BIO *bio = BIO_new_mem_buf(d.data, (int)d.length);
    cert = PEM_read_bio_X509_AUX(bio, NULL, NULL, NULL);
    BIO_free(bio);
  } else {
    const unsigned char *p = d.data;
    cert = d2i_X509(NULL, &p, (long)d.length);
  }
  tz_crypto_data_done(&d);
  if (!cert) {
    tz_openssl_fail("ERR_CRYPTO_OPERATION_FAILED", "could not read the certificate");
    return NULL;
  }
  ERR_clear_error();
  Certificate *c = (Certificate *)tz_obj_alloc(sizeof(Certificate));
  c->rc = 1;
  c->layout = &certificate_layout;
  c->cert = cert;

  return c;
}

static void bio_text(TzStr *out, BIO *bio) {
  char *data = NULL;
  long n = BIO_get_mem_data(bio, &data);
  tz_crypto_text(out, (const uint8_t *)data, n > 0 ? (size_t)n : 0, TZ_ENC_UTF8);
  BIO_free(bio);
}

static void name_text(TzStr *out, const X509_NAME *name) {
  BIO *bio = BIO_new(BIO_s_mem());
  X509_NAME_print_ex(bio, name, 0,
                     ASN1_STRFLGS_ESC_2253 | ASN1_STRFLGS_ESC_CTRL | ASN1_STRFLGS_UTF8_CONVERT |
                         XN_FLAG_SEP_MULTILINE | XN_FLAG_FN_SN);
  bio_text(out, bio);
}

void tz_openssl_x509_subject(TzStr *out, const Certificate *c) {
  name_text(out, X509_get_subject_name(c->cert));
}

void tz_openssl_x509_issuer(TzStr *out, const Certificate *c) {
  name_text(out, X509_get_issuer_name(c->cert));
}

static void time_text(TzStr *out, const ASN1_TIME *t) {
  BIO *bio = BIO_new(BIO_s_mem());
  ASN1_TIME_print(bio, t);
  bio_text(out, bio);
}

void tz_openssl_x509_valid_from(TzStr *out, const Certificate *c) {
  time_text(out, X509_get0_notBefore(c->cert));
}

void tz_openssl_x509_valid_to(TzStr *out, const Certificate *c) {
  time_text(out, X509_get0_notAfter(c->cert));
}

static int64_t time_ms(const ASN1_TIME *t) {
  struct tm tm;
  if (!ASN1_TIME_to_tm(t, &tm)) {
    return 0;
  }
  int64_t y = tm.tm_year + 1900, m = tm.tm_mon + 1;
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + tm.tm_mday - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = era * 146097 + doe - 719468;

  return (((days * 24 + tm.tm_hour) * 60 + tm.tm_min) * 60 + tm.tm_sec) * 1000;
}

int64_t tz_openssl_x509_valid_from_date(const Certificate *c) {
  return time_ms(X509_get0_notBefore(c->cert));
}

int64_t tz_openssl_x509_valid_to_date(const Certificate *c) {
  return time_ms(X509_get0_notAfter(c->cert));
}

void tz_openssl_x509_serial_number(TzStr *out, const Certificate *c) {
  BIGNUM *bn = ASN1_INTEGER_to_BN(X509_get0_serialNumber(c->cert), NULL);
  char *hex = bn ? BN_bn2hex(bn) : NULL;
  *out = tz_str_from_ascii(hex ? hex : "", hex ? strlen(hex) : 0);
  OPENSSL_free(hex);
  BN_free(bn);
}

void tz_openssl_x509_fingerprint(TzStr *out, const Certificate *c, int32_t bits) {
  const EVP_MD *md = bits == 512 ? EVP_sha512() : bits == 256 ? EVP_sha256() : EVP_sha1();
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int n = 0;
  X509_digest(c->cert, md, digest, &n);
  static const char hex[] = "0123456789ABCDEF";
  char text[3 * EVP_MAX_MD_SIZE];
  size_t o = 0;
  for (unsigned int i = 0; i < n; ++i) {
    if (i) {
      text[o++] = ':';
    }
    text[o++] = hex[digest[i] >> 4];
    text[o++] = hex[digest[i] & 15];
  }
  *out = tz_str_from_ascii(text, o);
}

int32_t tz_openssl_x509_subject_alt_name(TzStr *out, const Certificate *c) {
  GENERAL_NAMES *names =
      (GENERAL_NAMES *)X509_get_ext_d2i(c->cert, NID_subject_alt_name, NULL, NULL);
  *out = tz_str_from_ascii("", 0);
  if (!names) {
    return 0;
  }
  BIO *bio = BIO_new(BIO_s_mem());
  for (int i = 0; i < sk_GENERAL_NAME_num(names); ++i) {
    const GENERAL_NAME *name = sk_GENERAL_NAME_value(names, i);
    if (i) {
      BIO_puts(bio, ", ");
    }
    if (name->type == GEN_DNS || name->type == GEN_EMAIL || name->type == GEN_URI) {
      const ASN1_IA5STRING *s = name->type == GEN_DNS     ? name->d.dNSName
                                : name->type == GEN_EMAIL ? name->d.rfc822Name
                                                          : name->d.uniformResourceIdentifier;
      BIO_puts(bio, name->type == GEN_DNS ? "DNS:" : name->type == GEN_EMAIL ? "email:" : "URI:");
      BIO_write(bio, ASN1_STRING_get0_data(s), ASN1_STRING_length(s));
    } else {
      GENERAL_NAME_print(bio, (GENERAL_NAME *)name);
    }
  }
  GENERAL_NAMES_free(names);
  tz_str_release_value(*out);
  bio_text(out, bio);

  return 1;
}

int32_t tz_openssl_x509_info_access(TzStr *out, const Certificate *c) {
  AUTHORITY_INFO_ACCESS *info =
      (AUTHORITY_INFO_ACCESS *)X509_get_ext_d2i(c->cert, NID_info_access, NULL, NULL);
  *out = tz_str_from_ascii("", 0);
  if (!info) {
    return 0;
  }
  BIO *bio = BIO_new(BIO_s_mem());
  for (int i = 0; i < sk_ACCESS_DESCRIPTION_num(info); ++i) {
    const ACCESS_DESCRIPTION *d = sk_ACCESS_DESCRIPTION_value(info, i);
    char method[80];
    i2t_ASN1_OBJECT(method, sizeof method, d->method);
    BIO_printf(bio, "%s%s - ", i ? "\n" : "", method);
    GENERAL_NAME_print(bio, d->location);
  }
  AUTHORITY_INFO_ACCESS_free(info);
  tz_str_release_value(*out);
  bio_text(out, bio);

  return 1;
}

TzArray *tz_openssl_x509_key_usage(const Certificate *c) {
  EXTENDED_KEY_USAGE *usage =
      (EXTENDED_KEY_USAGE *)X509_get_ext_d2i(c->cert, NID_ext_key_usage, NULL, NULL);
  if (!usage) {
    return NULL;
  }
  TzArray *array = tz_arr_new(&tz_string_type, 0, sk_ASN1_OBJECT_num(usage));
  for (int i = 0; i < sk_ASN1_OBJECT_num(usage); ++i) {
    char oid[80];
    OBJ_obj2txt(oid, sizeof oid, sk_ASN1_OBJECT_value(usage, i), 1);
    TzStr s = tz_str_from_ascii(oid, strlen(oid));
    tz_arr_push_value(array, &s);
  }
  EXTENDED_KEY_USAGE_free(usage);

  return array;
}

int32_t tz_openssl_x509_ca(const Certificate *c) { return X509_check_ca(c->cert) == 1; }

TzBytes *tz_openssl_x509_raw(const Certificate *c) {
  unsigned char *der = NULL;
  int n = i2d_X509(c->cert, &der);
  TzBytes *b = tz_crypto_buffer(der, n > 0 ? (size_t)n : 0);
  OPENSSL_free(der);

  return b;
}

void tz_openssl_x509_pem(TzStr *out, const Certificate *c) {
  BIO *bio = BIO_new(BIO_s_mem());
  PEM_write_bio_X509(bio, c->cert);
  bio_text(out, bio);
}

TzKey *tz_openssl_key_of(void *pkey, int private_key);

TzKey *tz_openssl_x509_public_key(const Certificate *c) {
  EVP_PKEY *pkey = X509_get_pubkey(c->cert);
  if (!pkey) {
    tz_openssl_fail("ERR_CRYPTO_OPERATION_FAILED",
                    "could not get the public key of the certificate");
    return NULL;
  }

  return tz_openssl_key_of(pkey, 0);
}

static unsigned int check_flags(int32_t subject, int32_t has, int32_t wildcards, int32_t partial,
                                int32_t multi, int32_t single) {
  unsigned int flags = 0;
  if (subject == 1) {
    flags |= X509_CHECK_FLAG_ALWAYS_CHECK_SUBJECT;
  } else if (subject == 2) {
    flags |= X509_CHECK_FLAG_NEVER_CHECK_SUBJECT;
  }
  if ((has & 1) && !wildcards) {
    flags |= X509_CHECK_FLAG_NO_WILDCARDS;
  }
  if ((has & 2) && !partial) {
    flags |= X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS;
  }
  if ((has & 4) && multi) {
    flags |= X509_CHECK_FLAG_MULTI_LABEL_WILDCARDS;
  }
  if ((has & 8) && single) {
    flags |= X509_CHECK_FLAG_SINGLE_LABEL_SUBDOMAINS;
  }

  return flags;
}

int32_t tz_openssl_x509_check_host(TzStr *out, const Certificate *c, void *obj, uint64_t meta,
                                   int32_t subject, int32_t has, int32_t wildcards, int32_t partial,
                                   int32_t multi, int32_t single) {
  char name[512];
  size_t n = tz_crypto_cstr(obj, meta, name, sizeof name);
  char *peer = NULL;
  int ok =
      X509_check_host(c->cert, name, n,
                      check_flags(subject, has, wildcards, partial, multi, single), &peer) == 1;
  *out = tz_str_from_ascii("", 0);
  if (ok && peer) {
    tz_crypto_text(out, (const uint8_t *)peer, strlen(peer), TZ_ENC_UTF8);
  }
  OPENSSL_free(peer);

  return ok;
}

int32_t tz_openssl_x509_check_email(TzStr *out, const Certificate *c, void *obj, uint64_t meta,
                                    int32_t subject) {
  char email[512];
  size_t n = tz_crypto_cstr(obj, meta, email, sizeof email);
  int ok = X509_check_email(c->cert, email, n, check_flags(subject, 0, 0, 0, 0, 0)) == 1;
  *out = tz_str_from_ascii("", 0);
  if (ok) {
    tz_crypto_text(out, (const uint8_t *)email, n, TZ_ENC_UTF8);
  }

  return ok;
}

int32_t tz_openssl_x509_check_ip(TzStr *out, const Certificate *c, void *obj, uint64_t meta) {
  char ip[128];
  size_t n = tz_crypto_cstr(obj, meta, ip, sizeof ip);
  int ok = X509_check_ip_asc(c->cert, ip, 0) == 1;
  *out = tz_str_from_ascii("", 0);
  if (ok) {
    tz_crypto_text(out, (const uint8_t *)ip, n, TZ_ENC_UTF8);
  }

  return ok;
}

int32_t tz_openssl_x509_check_issued(const Certificate *c, const Certificate *issuer) {
  int ok = X509_check_issued(issuer->cert, c->cert) == X509_V_OK;
  ERR_clear_error();

  return ok;
}

int32_t tz_openssl_x509_check_private_key(const Certificate *c, const TzKey *key) {
  if (key->type != TZ_KEY_PRIVATE) {
    const char *got = key->type == TZ_KEY_SECRET ? "secret" : "public";
    tz_crypto_note_key_types(got, "private");
    tz_crypto_fail("ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE",
                   "checkPrivateKey needs a private key, but this one is %s", got);
    return 0;
  }
  int ok = X509_check_private_key(c->cert, (EVP_PKEY *)key->pkey) == 1;
  ERR_clear_error();

  return ok;
}

int32_t tz_openssl_x509_verify(const Certificate *c, const TzKey *key) {
  if (key->type != TZ_KEY_PUBLIC) {
    const char *got = key->type == TZ_KEY_SECRET ? "secret" : "private";
    tz_crypto_note_key_types(got, "public");
    tz_crypto_fail("ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE",
                   "verify needs the public key of the issuer, but this one is %s", got);
    return 0;
  }
  int ok = X509_verify(c->cert, (EVP_PKEY *)key->pkey) > 0;
  ERR_clear_error();

  return ok;
}
