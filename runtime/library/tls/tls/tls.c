#include "runtime/library/crypto/crypto/crypto.h"
#include "runtime/library/net/net/net.h"

#ifdef _WIN32
#include <wincrypt.h>
#else
#include <arpa/inet.h>
#endif

#include <errno.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "runtime/library/tls/tls/tls_roots.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/async/async.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/error/error.h"
#include "runtime/core/group/group.h"
#include "runtime/core/output/output.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/bytes/buffer/buffer.h"
#include "runtime/library/bytes/bytes/bytes.h"
#include "runtime/library/events/signal/signal.h"

static const char kSuites[] =
    "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256";
static const char kCiphers[] =
    "ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:"
    "ECDHE-ECDSA-AES256-GCM-SHA384:DHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-SHA256:"
    "DHE-RSA-AES128-SHA256:ECDHE-RSA-AES256-SHA384:DHE-RSA-AES256-SHA384:ECDHE-RSA-AES256-SHA256:"
    "DHE-RSA-AES256-SHA256:HIGH:!aNULL:!eNULL:!EXPORT:!DES:!RC4:!MD5:!PSK:!SRP:!CAMELLIA";
static const unsigned char kAlpn[] = "\x08http/1.1";

SSL_CTX *tz_tls_client;
BIO_METHOD *tz_tls_bio_method;
int32_t tz_tls_system_roots;

typedef struct Tls {
  NetConn *conn;
  SSL *ssl;
  uint8_t server;
  uint8_t handshaken;
  uint8_t failed;
  uint8_t shut;
  unsigned long failure;
  TzPromise *promise;
  TzMaker make;
  TzSignal *signal;
  TzAbortListener abort;
  char host[256];
} Tls;

void tz_err_begin(void);
void tz_err_end(void);

static TzStr ascii(const char *text) { return tz_str_from_ascii(text, strlen(text)); }

static void *tls_error(TzMaker make, const char *code, const char *message, int32_t number) {
  TzSystemErrorParts parts;
  memset(&parts, 0, sizeof parts);
  parts.message = net_utf8(message);
  parts.errno_value = number;
  parts.code = ascii(code);
  parts.syscall = ascii("");

  return make(&parts);
}

static void *ssl_error(TzMaker make, unsigned long error) {
  static const struct {
    int lib;
    const char *name;
  } kLibs[] = {
      {ERR_LIB_SYS, "SYS_"},       {ERR_LIB_BN, "BN_"},
      {ERR_LIB_RSA, "RSA_"},       {ERR_LIB_DH, "DH_"},
      {ERR_LIB_EVP, "EVP_"},       {ERR_LIB_BUF, "BUF_"},
      {ERR_LIB_OBJ, "OBJ_"},       {ERR_LIB_PEM, "PEM_"},
      {ERR_LIB_DSA, "DSA_"},       {ERR_LIB_X509, "X509_"},
      {ERR_LIB_ASN1, "ASN1_"},     {ERR_LIB_CONF, "CONF_"},
      {ERR_LIB_CRYPTO, "CRYPTO_"}, {ERR_LIB_EC, "EC_"},
      {ERR_LIB_SSL, "SSL_"},       {ERR_LIB_BIO, "BIO_"},
      {ERR_LIB_PKCS7, "PKCS7_"},   {ERR_LIB_X509V3, "X509V3_"},
      {ERR_LIB_PKCS12, "PKCS12_"}, {ERR_LIB_RAND, "RAND_"},
      {ERR_LIB_DSO, "DSO_"},       {ERR_LIB_OCSP, "OCSP_"},
      {ERR_LIB_UI, "UI_"},         {ERR_LIB_ECDSA, "ECDSA_"},
      {ERR_LIB_ECDH, "ECDH_"},     {ERR_LIB_OSSL_STORE, "OSSL_STORE_"},
      {ERR_LIB_FIPS, "FIPS_"},     {ERR_LIB_TS, "TS_"},
      {ERR_LIB_HMAC, "HMAC_"},     {ERR_LIB_CT, "CT_"},
      {ERR_LIB_ASYNC, "ASYNC_"},   {ERR_LIB_KDF, "KDF_"},
      {ERR_LIB_SM2, "SM2_"},       {ERR_LIB_USER, "USER_"},
  };
  char message[256];
  ERR_error_string_n(error, message, sizeof message);
  const char *lib = "";
  for (size_t i = 0; i < sizeof kLibs / sizeof kLibs[0]; ++i) {
    if (kLibs[i].lib == ERR_GET_LIB(error)) {
      lib = kLibs[i].name;
    }
  }
  char code[160];
  int n = snprintf(code, sizeof code, "ERR_%s%s", strcmp(lib, "SSL_") == 0 ? "" : "OSSL_", lib);
  const char *reason = ERR_reason_error_string(error);
  for (const char *r = reason ? reason : "unknown"; *r && n + 1 < (int)sizeof code; ++r) {
    code[n++] = *r == ' ' ? '_' : *r >= 'a' && *r <= 'z' ? (char)(*r - 32) : *r;
  }
  code[n] = 0;

  return tls_error(make, code, message, 0);
}

static void *disconnected(TzMaker make) {
  return tls_error(make, "ECONNRESET",
                   "Client network socket disconnected before secure TLS connection was "
                   "established",
                   -4077);
}

static const char *verify_code(long result) {
  switch (result) {
#define CODE(name)                                                                                 \
  case X509_V_ERR_##name:                                                                          \
    return #name;
    CODE(UNABLE_TO_GET_ISSUER_CERT)
    CODE(UNABLE_TO_GET_CRL)
    CODE(UNABLE_TO_DECRYPT_CERT_SIGNATURE)
    CODE(UNABLE_TO_DECRYPT_CRL_SIGNATURE)
    CODE(UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY)
    CODE(CERT_SIGNATURE_FAILURE)
    CODE(CRL_SIGNATURE_FAILURE)
    CODE(CERT_NOT_YET_VALID)
    CODE(CERT_HAS_EXPIRED)
    CODE(CRL_NOT_YET_VALID)
    CODE(CRL_HAS_EXPIRED)
    CODE(ERROR_IN_CERT_NOT_BEFORE_FIELD)
    CODE(ERROR_IN_CERT_NOT_AFTER_FIELD)
    CODE(ERROR_IN_CRL_LAST_UPDATE_FIELD)
    CODE(ERROR_IN_CRL_NEXT_UPDATE_FIELD)
    CODE(OUT_OF_MEM)
    CODE(DEPTH_ZERO_SELF_SIGNED_CERT)
    CODE(SELF_SIGNED_CERT_IN_CHAIN)
    CODE(UNABLE_TO_GET_ISSUER_CERT_LOCALLY)
    CODE(UNABLE_TO_VERIFY_LEAF_SIGNATURE)
    CODE(CERT_CHAIN_TOO_LONG)
    CODE(CERT_REVOKED)
    CODE(INVALID_CA)
    CODE(PATH_LENGTH_EXCEEDED)
    CODE(INVALID_PURPOSE)
    CODE(CERT_UNTRUSTED)
    CODE(CERT_REJECTED)
    CODE(HOSTNAME_MISMATCH)
#undef CODE
  default:
    return "UNSPECIFIED";
  }
}

static void *verify_error(TzMaker make, long result) {
  char message[512];
  int hint = !tz_tls_system_roots && (result == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT ||
                                      result == X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE);
  snprintf(message, sizeof message, "%s%s", X509_verify_cert_error_string(result),
           hint ? "; if the root CA is installed locally, try running the program with "
                  "NODE_USE_SYSTEM_CA=1"
                : "");

  return tls_error(make, verify_code(result), message, 0);
}

typedef struct Text {
  char *data;
  size_t length, cap;
} Text;

static void text_add(Text *t, const char *s, size_t n) {
  if (t->length + n + 1 > t->cap) {
    size_t cap = t->cap * 2 + n + 64;
    char *data = (char *)tz_alloc(cap);
    if (t->length) {
      memcpy(data, t->data, t->length);
    }
    tz_free(t->data);
    t->data = data;
    t->cap = cap;
  }
  memcpy(t->data + t->length, s, n);
  t->length += n;
  t->data[t->length] = 0;
}

static void text_put(Text *t, const char *s) { text_add(t, s, strlen(s)); }

static const char *text_of(const Text *t) { return t->data ? t->data : ""; }

static TzBytes *bytes_of(void *obj, uint64_t meta) {
  return tz_buffer_from_string(obj, meta, TZ_ENC_UTF8);
}

static int bio_write(BIO *bio, const char *data, size_t length, size_t *written) {
  NetConn *c = (NetConn *)BIO_get_data(bio);
  memcpy(net_seal_reserve(c, length), data, length);
  c->sealed_len += (uint32_t)length;
  *written = length;

  return 1;
}

static int bio_read(BIO *bio, char *out, size_t length, size_t *got) {
  NetConn *c = (NetConn *)BIO_get_data(bio);
  BIO_clear_retry_flags(bio);
  size_t have = c->raw_end - c->raw_start;
  *got = 0;
  if (!have) {
    if (!c->raw_eof) {
      BIO_set_retry_read(bio);
    }
    return 0;
  }
  if (length > have) {
    length = have;
  }
  memcpy(out, c->raw + c->raw_start, length);
  c->raw_start += (uint32_t)length;
  *got = length;

  return 1;
}

static long bio_ctrl(BIO *bio, int cmd, long num, void *ptr) {
  (void)num;
  (void)ptr;
  NetConn *c = (NetConn *)BIO_get_data(bio);
  switch (cmd) {
  case BIO_CTRL_FLUSH:
    return 1;
  case BIO_CTRL_EOF:
    return c->raw_eof && c->raw_start == c->raw_end;
  case BIO_CTRL_PENDING:
    return (long)(c->raw_end - c->raw_start);
  default:
    return 0;
  }
}

static int bio_create(BIO *bio) {
  BIO_set_init(bio, 1);

  return 1;
}

static BIO_METHOD *bio_method(void) {
  if (!tz_tls_bio_method) {
    BIO_METHOD *m = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "tz connection");
    BIO_meth_set_write_ex(m, bio_write);
    BIO_meth_set_read_ex(m, bio_read);
    BIO_meth_set_ctrl(m, bio_ctrl);
    BIO_meth_set_create(m, bio_create);
    tz_tls_bio_method = m;
  }

  return tz_tls_bio_method;
}

static void tls_seal(NetConn *c);

static void fail(NetConn *c, unsigned long error) {
  Tls *t = (Tls *)c->tls;
  if (t->failed) {
    return;
  }
  t->failed = 1;
  t->failure = error;
  ERR_clear_error();
  c->out_len = 0;
  net_flush(c);
  c->error = NET_TLS_FAILED;
}

static unsigned long handshake_error(NetConn *c) {
  unsigned long error = ERR_peek_error();
  if (c->raw_eof && c->raw_start == c->raw_end &&
      (!error || ERR_GET_REASON(error) == SSL_R_UNEXPECTED_EOF_WHILE_READING)) {
    return 0;
  }

  return error ? error : ERR_PACK(ERR_LIB_SSL, 0, SSL_R_UNEXPECTED_EOF_WHILE_READING);
}

static int tls_open(NetConn *c) {
  Tls *t = (Tls *)c->tls;
  if (t->failed) {
    return 1;
  }
  if (!t->handshaken) {
    ERR_clear_error();
    int r = SSL_do_handshake(t->ssl);
    if (r != 1) {
      int e = SSL_get_error(t->ssl, r);
      if ((e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) && !c->raw_eof) {
        return 0;
      }
      fail(c, handshake_error(c));
      return 1;
    }
    t->handshaken = 1;
    if (!t->server) {
      return 1;
    }
  }
  int produced = 0;
  for (;;) {
    net_input_room(c, 16384);
    size_t got = 0;
    ERR_clear_error();
    int r = SSL_read_ex(t->ssl, c->in + c->in_end, c->in_cap - c->in_end, &got);
    if (r == 1) {
      c->in_end += (uint32_t)got;
      produced = 1;
      continue;
    }
    int e = SSL_get_error(t->ssl, r);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
      if (!c->raw_eof || c->raw_start != c->raw_end) {
        return produced;
      }
      c->eof = 1;
      return 1;
    }
    if (e == SSL_ERROR_ZERO_RETURN) {
      c->eof = 1;
      return 1;
    }
    fail(c, ERR_peek_error());
    return 1;
  }
}

static void tls_seal(NetConn *c) {
  Tls *t = (Tls *)c->tls;
  if (!t->handshaken || t->failed || t->shut || !c->out_len) {
    return;
  }
  size_t written = 0;
  ERR_clear_error();
  if (SSL_write_ex(t->ssl, c->out, c->out_len, &written) != 1) {
    fail(c, ERR_peek_error());
    return;
  }
  c->out_len = 0;
}

static void tls_shutdown(NetConn *c) {
  Tls *t = (Tls *)c->tls;
  if (t->shut) {
    return;
  }
  if (!t->handshaken || t->failed) {
    c->out_len = 0;
    t->shut = 1;
    return;
  }
  tls_seal(c);
  t->shut = 1;
  if (!t->failed) {
    ERR_clear_error();
    SSL_shutdown(t->ssl);
    ERR_clear_error();
  }
}

static void *tls_failure(NetConn *c, TzMaker make) {
  Tls *t = (Tls *)c->tls;

  return t->failure ? ssl_error(make, t->failure) : disconnected(make);
}

static void stop_waiting(Tls *t) {
  if (t->signal) {
    tz_signal_unlisten(&t->abort);
    tz_instance_release((TzInstance *)t->signal);
    t->signal = NULL;
  }
}

static void tls_release(NetConn *c) {
  Tls *t = (Tls *)c->tls;
  c->tls = NULL;
  stop_waiting(t);
  if (t->promise) {
    TzPromise *p = t->promise;
    t->promise = NULL;
    tz_obj_release((TzObject *)p);
  }
  SSL_free(t->ssl);
  tz_free(t);
}

static const NetTlsOps tls_ops = {tls_open, tls_seal, tls_shutdown, tls_failure, tls_release};

static Tls *tls_new(NetConn *c, SSL_CTX *ctx) {
  Tls *t = (Tls *)tz_alloc(sizeof(Tls));
  memset(t, 0, sizeof *t);
  t->conn = c;
  t->ssl = SSL_new(ctx);
  BIO *bio = BIO_new(bio_method());
  BIO_set_data(bio, c);
  SSL_set_bio(t->ssl, bio, bio);
  c->tls = t;
  c->tls_ops = &tls_ops;

  return t;
}

static void add_mozilla_roots(X509_STORE *store) {
  uint8_t *der = (uint8_t *)tz_alloc(TLS_ROOTS_SIZE);
  z_stream z;
  memset(&z, 0, sizeof z);
  inflateInit2(&z, -15);
  z.next_in = (Bytef *)tls_roots;
  z.avail_in = (uInt)sizeof tls_roots;
  z.next_out = der;
  z.avail_out = TLS_ROOTS_SIZE;
  int r = inflate(&z, Z_FINISH);
  size_t length = z.total_out;
  inflateEnd(&z);
  if (r != Z_STREAM_END || length != TLS_ROOTS_SIZE) {
    tz_panicf("the root certificates of TLS are damaged");
  }
  const unsigned char *p = der, *end = der + length;
  while (p < end) {
    X509 *cert = d2i_X509(NULL, &p, (long)(end - p));
    if (!cert) {
      tz_panicf("the root certificates of TLS are damaged");
    }
    X509_STORE_add_cert(store, cert);
    X509_free(cert);
  }
  tz_free(der);
}

static void warn(const char *message) {
  tz_err_begin();
  tz_out_write("Warning: ", 9);
  tz_out_write(message, strlen(message));
  tz_out_write("\n", 1);
  tz_err_end();
}

static void add_extra_roots(X509_STORE *store) {
  char name[1024];
  FILE *f = NULL;
#ifdef _WIN32
  const wchar_t *wide = _wgetenv(L"NODE_EXTRA_CA_CERTS");
  if (!wide || !*wide) {
    return;
  }
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, name, sizeof name, NULL, NULL);
  f = _wfopen(wide, L"rb");
#else
  const char *given = getenv("NODE_EXTRA_CA_CERTS");
  if (!given || !*given) {
    return;
  }
  snprintf(name, sizeof name, "%s", given);
  f = fopen(given, "rb");
#endif
  if (!f) {
    char message[1400];
    snprintf(message, sizeof message,
             "Ignoring extra certs from `%s`, load failed: error:%08lX:system library::%s", name,
             (unsigned long)ERR_PACK(ERR_LIB_SYS, 0, errno), strerror(errno));
    warn(message);
    return;
  }
  Text data = {NULL, 0, 0};
  char chunk[16384];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
    text_add(&data, chunk, n);
  }
  fclose(f);
  BIO *bio = BIO_new_mem_buf(text_of(&data), (int)data.length);
  ERR_clear_error();
  X509 *cert;
  while ((cert = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL) {
    X509_STORE_add_cert(store, cert);
    X509_free(cert);
  }
  unsigned long error = ERR_peek_error();
  ERR_clear_error();
  BIO_free(bio);
  tz_free(data.data);
  if (error &&
      !(ERR_GET_LIB(error) == ERR_LIB_PEM && ERR_GET_REASON(error) == PEM_R_NO_START_LINE)) {
    char text[256], message[1400];
    ERR_error_string_n(error, text, sizeof text);
    snprintf(message, sizeof message, "Ignoring extra certs from `%s`, load failed: %s", name,
             text);
    warn(message);
  }
}

#ifdef _WIN32
static int for_servers(PCCERT_CONTEXT cert) {
  DWORD size = 0;
  if (!CertGetEnhancedKeyUsage(cert, 0, NULL, &size)) {
    return 0;
  }
  CERT_ENHKEY_USAGE *usage = (CERT_ENHKEY_USAGE *)tz_alloc(size);
  int ok = 0;
  if (CertGetEnhancedKeyUsage(cert, 0, usage, &size)) {
    if (usage->cUsageIdentifier == 0) {
      ok = GetLastError() == (DWORD)CRYPT_E_NOT_FOUND;
    }
    for (DWORD i = 0; i < usage->cUsageIdentifier; ++i) {
      ok |= strcmp(usage->rgpszUsageIdentifier[i], szOID_PKIX_KP_SERVER_AUTH) == 0;
    }
  }
  tz_free(usage);

  return ok;
}
#endif

static void add_system_roots(X509_STORE *store) {
#ifdef _WIN32
  static const wchar_t *const kStores[] = {L"ROOT", L"CA", L"TrustedPeople"};
  for (size_t i = 0; i < sizeof kStores / sizeof kStores[0]; ++i) {
    HCERTSTORE system = CertOpenSystemStoreW(0, kStores[i]);
    if (!system) {
      continue;
    }
    PCCERT_CONTEXT cert = NULL;
    while ((cert = CertEnumCertificatesInStore(system, cert)) != NULL) {
      if (!for_servers(cert)) {
        continue;
      }
      const unsigned char *p = cert->pbCertEncoded;
      X509 *x = d2i_X509(NULL, &p, (long)cert->cbCertEncoded);
      if (x) {
        X509_STORE_add_cert(store, x);
        X509_free(x);
      }
    }
    CertCloseStore(system, 0);
  }
  ERR_clear_error();
#else
  (void)store;
#endif
}

static X509_STORE *trusted_roots(void) {
  X509_STORE *store = X509_STORE_new();
  add_mozilla_roots(store);
  const char *system = getenv("NODE_USE_SYSTEM_CA");
  tz_tls_system_roots = system && strcmp(system, "1") == 0;
  if (tz_tls_system_roots) {
    add_system_roots(store);
  }
  add_extra_roots(store);
  ERR_clear_error();

  return store;
}

static void configure(SSL_CTX *ctx) {
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
  SSL_CTX_set_cipher_list(ctx, kCiphers);
  SSL_CTX_set_ciphersuites(ctx, kSuites);
  SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION | SSL_OP_IGNORE_UNEXPECTED_EOF);
}

static int verify_all(int ok, X509_STORE_CTX *store) {
  (void)ok;
  (void)store;

  return 1;
}

static SSL_CTX *client_context(void) {
  if (tz_tls_client) {
    return tz_tls_client;
  }
  SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
  configure(ctx);
  SSL_CTX_set_cert_store(ctx, trusted_roots());
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, verify_all);
  SSL_CTX_set_alpn_protos(ctx, kAlpn, sizeof kAlpn - 1);
  tz_tls_client = ctx;

  return ctx;
}

static int select_alpn(SSL *ssl, const unsigned char **out, unsigned char *out_length,
                       const unsigned char *in, unsigned int in_length, void *arg) {
  (void)ssl;
  (void)arg;
  unsigned char *chosen = NULL;
  int status = SSL_select_next_proto(&chosen, out_length, kAlpn, sizeof kAlpn - 1, in, in_length);
  *out = chosen;

  return status == OPENSSL_NPN_NEGOTIATED ? SSL_TLSEXT_ERR_OK : SSL_TLSEXT_ERR_ALERT_FATAL;
}

static void fail_openssl(TzMaker make, const char *code, const char *otherwise) {
  unsigned long error = ERR_peek_error();
  ERR_clear_error();
  char text[256];
  if (error) {
    ERR_error_string_n(error, text, sizeof text);
  } else {
    snprintf(text, sizeof text, "%s", otherwise);
  }
  TzSystemErrorParts parts;
  memset(&parts, 0, sizeof parts);
  parts.message = net_utf8(text);
  parts.code = net_utf8(code);
  parts.syscall = net_utf8("listen");
  tz_set_failed(make(&parts));
}

static SSL_CTX *server_context(void *cert_obj, uint64_t cert_meta, void *key_obj, uint64_t key_meta,
                               TzMaker make) {
  SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
  configure(ctx);
  SSL_CTX_set_alpn_select_cb(ctx, select_alpn, NULL);
  ERR_clear_error();
  TzBytes *cert = bytes_of(cert_obj, cert_meta);
  BIO *bio = BIO_new_mem_buf(cert->data, cert->length);
  X509 *leaf = PEM_read_bio_X509_AUX(bio, NULL, NULL, NULL);
  int ok = leaf && SSL_CTX_use_certificate(ctx, leaf) == 1;
  X509_free(leaf);
  if (ok) {
    X509 *ca;
    while ((ca = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL) {
      if (SSL_CTX_add0_chain_cert(ctx, ca) != 1) {
        X509_free(ca);
      }
    }
    ERR_clear_error();
  }
  BIO_free(bio);
  tz_instance_release((TzInstance *)cert);
  if (!ok) {
    fail_openssl(make, "ERR_TLS_CERT", "the certificate of tls cannot be read");
    SSL_CTX_free(ctx);
    return NULL;
  }
  TzBytes *key = bytes_of(key_obj, key_meta);
  bio = BIO_new_mem_buf(key->data, key->length);
  EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
  ok = pkey && SSL_CTX_use_PrivateKey(ctx, pkey) == 1;
  EVP_PKEY_free(pkey);
  BIO_free(bio);
  tz_instance_release((TzInstance *)key);
  if (!ok) {
    fail_openssl(make, "ERR_TLS_KEY", "the key of tls cannot be read");
    SSL_CTX_free(ctx);
    return NULL;
  }

  return ctx;
}

static int address_of(const char *host, unsigned char *out) {
  if (inet_pton(AF_INET, host, out) == 1) {
    return 4;
  }
  if (inet_pton(AF_INET6, host, out) == 1) {
    return 16;
  }

  return 0;
}

static int split_dots(char *s, char **parts, int max) {
  int count = 0;
  for (char *p = s;;) {
    if (count == max) {
      return -1;
    }
    parts[count++] = p;
    char *dot = strchr(p, '.');
    if (!dot) {
      return count;
    }
    *dot = 0;
    p = dot + 1;
  }
}

static int name_matches(const char *host, const unsigned char *given, size_t length) {
  char pattern[256], name[256];
  if (length == 0 || length >= sizeof pattern || strlen(host) >= sizeof name) {
    return 0;
  }
  for (size_t i = 0; i < length; ++i) {
    unsigned char ch = given[i];
    if (ch < 0x21 || ch > 0x7F) {
      return 0;
    }
    pattern[i] = ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : (char)ch;
  }
  pattern[length] = 0;
  if (pattern[length - 1] == '.') {
    pattern[length - 1] = 0;
  }
  snprintf(name, sizeof name, "%s", host);
  char *hosts[128], *patterns[128];
  int h = split_dots(name, hosts, 128), p = split_dots(pattern, patterns, 128);
  if (h < 0 || p < 0 || h != p) {
    return 0;
  }
  for (int i = 0; i < p; ++i) {
    if (!*patterns[i]) {
      return 0;
    }
  }
  for (int i = h - 1; i > 0; --i) {
    if (strcmp(hosts[i], patterns[i]) != 0) {
      return 0;
    }
  }
  const char *sub = hosts[0], *wild = patterns[0];
  const char *star = strchr(wild, '*');
  if (!star || strstr(wild, "xn--")) {
    return strcmp(sub, wild) == 0;
  }
  if (strchr(star + 1, '*') || p <= 2) {
    return 0;
  }
  size_t prefix = (size_t)(star - wild), suffix = strlen(star + 1), n = strlen(sub);

  return prefix + suffix <= n && strncmp(sub, wild, prefix) == 0 &&
         strcmp(sub + n - suffix, star + 1) == 0;
}

static void put_alt_name(Text *t, const char *prefix, const unsigned char *s, size_t n) {
  text_put(t, prefix);
  int safe = 1;
  for (size_t i = 0; i < n && safe; ++i) {
    safe = s[i] >= ' ' && s[i] <= '~' && s[i] != '"' && s[i] != '\\' && s[i] != ',' && s[i] != '\'';
  }
  if (safe) {
    text_add(t, (const char *)s, n);
    return;
  }
  text_put(t, "\"");
  for (size_t i = 0; i < n; ++i) {
    char escaped[8];
    if (s[i] == '"' || s[i] == '\\') {
      snprintf(escaped, sizeof escaped, "\\%c", s[i]);
    } else if (s[i] < ' ' || s[i] == 0x7F) {
      snprintf(escaped, sizeof escaped, "\\u%04x", s[i]);
    } else {
      escaped[0] = (char)s[i];
      escaped[1] = 0;
    }
    text_put(t, escaped);
  }
  text_put(t, "\"");
}

static void put_general_name(Text *t, const GENERAL_NAME *g) {
  if (g->type == GEN_DNS || g->type == GEN_EMAIL || g->type == GEN_URI) {
    const ASN1_IA5STRING *s = g->type == GEN_DNS     ? g->d.dNSName
                              : g->type == GEN_EMAIL ? g->d.rfc822Name
                                                     : g->d.uniformResourceIdentifier;
    put_alt_name(t,
                 g->type == GEN_DNS     ? "DNS:"
                 : g->type == GEN_EMAIL ? "email:"
                                        : "URI:",
                 ASN1_STRING_get0_data(s), (size_t)ASN1_STRING_length(s));
    return;
  }
  if (g->type == GEN_IPADD) {
    const unsigned char *b = g->d.ip->data;
    char text[64];
    if (g->d.ip->length == 4) {
      snprintf(text, sizeof text, "IP Address:%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
    } else if (g->d.ip->length == 16) {
      int n = snprintf(text, sizeof text, "IP Address:");
      for (int j = 0; j < 8; ++j) {
        n += snprintf(text + n, sizeof text - (size_t)n, j ? ":%X" : "%X",
                      (b[2 * j] << 8) | b[2 * j + 1]);
      }
    } else {
      snprintf(text, sizeof text, "IP Address:<invalid length=%d>", g->d.ip->length);
    }
    text_put(t, text);
    return;
  }
  BIO *bio = BIO_new(BIO_s_mem());
  GENERAL_NAME_print(bio, (GENERAL_NAME *)g);
  char *data = NULL;
  long n = BIO_get_mem_data(bio, &data);
  if (n > 0) {
    text_add(t, data, (size_t)n);
  }
  BIO_free(bio);
}

static void *check_identity(TzMaker make, const char *given, X509 *cert) {
  char host[256];
  snprintf(host, sizeof host, "%s", given);
  size_t length = strlen(host);
  if (length && host[length - 1] == '.') {
    host[--length] = 0;
  }
  char lowered[256];
  for (size_t i = 0; i <= length; ++i) {
    lowered[i] = host[i] >= 'A' && host[i] <= 'Z' ? (char)(host[i] + 32) : host[i];
  }
  unsigned char address[16];
  int address_length = address_of(host, address);

  Text altnames = {NULL, 0, 0}, ips = {NULL, 0, 0}, reason = {NULL, 0, 0};
  int dns = 0, valid = 0;
  GENERAL_NAMES *names = (GENERAL_NAMES *)X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
  for (int i = 0; i < sk_GENERAL_NAME_num(names); ++i) {
    const GENERAL_NAME *g = sk_GENERAL_NAME_value(names, i);
    if (i) {
      text_put(&altnames, ", ");
    }
    put_general_name(&altnames, g);
    if (g->type == GEN_DNS) {
      ++dns;
      if (!address_length && name_matches(lowered, ASN1_STRING_get0_data(g->d.dNSName),
                                          (size_t)ASN1_STRING_length(g->d.dNSName))) {
        valid = 1;
      }
    } else if (g->type == GEN_IPADD && (g->d.ip->length == 4 || g->d.ip->length == 16)) {
      char text[64];
      inet_ntop(g->d.ip->length == 4 ? AF_INET : AF_INET6, g->d.ip->data, text, sizeof text);
      if (ips.length) {
        text_put(&ips, ", ");
      }
      text_put(&ips, text);
      if (address_length == g->d.ip->length &&
          memcmp(address, g->d.ip->data, (size_t)address_length) == 0) {
        valid = 1;
      }
    }
  }
  GENERAL_NAMES_free(names);

  if (address_length) {
    if (!valid) {
      text_put(&reason, "IP: ");
      text_put(&reason, host);
      text_put(&reason, " is not in the cert's list: ");
      text_put(&reason, text_of(&ips));
    }
  } else if (dns) {
    if (!valid) {
      text_put(&reason, "Host: ");
      text_put(&reason, host);
      text_put(&reason, ". is not in the cert's altnames: ");
      text_put(&reason, text_of(&altnames));
    }
  } else {
    X509_NAME *subject = X509_get_subject_name(cert);
    Text cn = {NULL, 0, 0};
    int count = 0;
    for (int at = -1; (at = X509_NAME_get_index_by_NID(subject, NID_commonName, at)) >= 0;) {
      unsigned char *utf8 = NULL;
      int n =
          ASN1_STRING_to_UTF8(&utf8, X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject, at)));
      if (n < 0) {
        continue;
      }
      if (count++) {
        text_put(&cn, ",");
      }
      text_add(&cn, (const char *)utf8, (size_t)n);
      if (name_matches(lowered, utf8, (size_t)n)) {
        valid = 1;
      }
      OPENSSL_free(utf8);
    }
    if (!count) {
      text_put(&reason, "Cert does not contain a DNS name");
    } else if (!valid) {
      text_put(&reason, "Host: ");
      text_put(&reason, host);
      text_put(&reason, ". is not cert's CN: ");
      text_put(&reason, text_of(&cn));
    }
    tz_free(cn.data);
  }
  void *error = NULL;
  if (!valid) {
    Text message = {NULL, 0, 0};
    text_put(&message, "Hostname/IP does not match certificate's altnames: ");
    text_put(&message, text_of(&reason));
    error = tls_error(make, "ERR_TLS_CERT_ALTNAME_INVALID", text_of(&message), 0);
    tz_free(message.data);
  }
  tz_free(altnames.data);
  tz_free(ips.data);
  tz_free(reason.data);

  return error;
}

static void *check_peer(Tls *t) {
  X509 *cert = SSL_get1_peer_certificate(t->ssl);
  long result = X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT;
  if (cert) {
    result = SSL_get_verify_result(t->ssl);
  } else if (SSL_version(t->ssl) == TLS1_3_VERSION && SSL_session_reused(t->ssl)) {
    result = X509_V_OK;
  }
  void *error = NULL;
  if (result != X509_V_OK) {
    error = verify_error(t->make, result);
  } else if (cert && !SSL_session_reused(t->ssl)) {
    error = check_identity(t->make, t->host, cert);
  }
  X509_free(cert);

  return error;
}

static void handshake_input(NetConn *c) {
  Tls *t = (Tls *)c->tls;
  if (!t || !t->promise) {
    return;
  }
  void *error;
  if (c->error == NET_TLS_FAILED) {
    error = tls_failure(c, t->make);
  } else if (c->error) {
    error = net_system_error(t->make, c->error, "read", NULL, 0);
  } else if (!t->handshaken) {
    error = disconnected(t->make);
  } else {
    error = check_peer(t);
  }
  stop_waiting(t);
  if (error) {
    net_reject(&t->promise, error);
    net_abort(c);
  } else {
    net_fulfill(&t->promise, NULL, 0);
  }
}

static void abort_handshake(TzAbortListener *l) {
  Tls *t = CONTAINING_RECORD(l, Tls, abort);
  TzSignal *signal = t->signal;
  t->signal = NULL;
  void *reason = net_aborted();
  tz_instance_release((TzInstance *)signal);
  if (t->promise) {
    net_reject(&t->promise, reason);
  } else {
    tz_instance_release((TzInstance *)reason);
  }
  net_abort(t->conn);
}

void tz_tls_connect(TzPromise *p, void *conn, void *host_obj, uint64_t host_meta, void *signal,
                    TzMaker make) {
  NetConn *c = (NetConn *)conn;
  tz_net_make = make;
  tz_ref_retain(p);
  Tls *t = tls_new(c, client_context());
  t->promise = p;
  t->make = make;
  TzBytes *host = bytes_of(host_obj, host_meta);
  size_t n = (size_t)host->length < sizeof t->host - 1 ? (size_t)host->length : sizeof t->host - 1;
  memcpy(t->host, host->data, n);
  t->host[n] = 0;
  tz_instance_release((TzInstance *)host);
  unsigned char address[16];
  if (!address_of(t->host, address)) {
    SSL_set_tlsext_host_name(t->ssl, t->host);
  }
  SSL_set_connect_state(t->ssl);
  c->on_input = handshake_input;
  if (signal) {
    if (tz_signal_aborted(signal)) {
      net_reject(&t->promise, net_aborted());
      net_abort(c);
      return;
    }
    tz_ref_retain(signal);
    t->signal = (TzSignal *)signal;
    t->abort.run = abort_handshake;
    tz_signal_listen(t->signal, &t->abort);
  }
  net_read(c);
}

static void accept_tls(NetListener *l, NetConn *c) {
  Tls *t = tls_new(c, (SSL_CTX *)l->tls);
  t->server = 1;
  SSL_set_accept_state(t->ssl);
}

static void release_context(void *tls) { SSL_CTX_free((SSL_CTX *)tls); }

void *tz_tls_listen(void *host_obj, uint64_t host_meta, int64_t port, double max_body,
                    void *cert_obj, uint64_t cert_meta, void *key_obj, uint64_t key_meta,
                    TzMaker make) {
  SSL_CTX *ctx = server_context(cert_obj, cert_meta, key_obj, key_meta, make);
  if (!ctx) {
    return NULL;
  }
  NetListener *l = (NetListener *)tz_net_listen(host_obj, host_meta, port, max_body, make);
  if (!l) {
    SSL_CTX_free(ctx);
    return NULL;
  }
  l->tls = ctx;
  l->tls_accept = accept_tls;
  l->tls_release = release_context;

  return l;
}
