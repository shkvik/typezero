// The full layer (docs/crypto.md, section 3): asymmetric keys. KeyObjects from PEM, DER and JWK
// and back, sign and verify (RSA, RSA-PSS, ECDSA, Ed25519, Ed448), createSign and createVerify,
// generateKeyPairSync, ECDH and diffieHellman, as node does them over OpenSSL.

#include "runtime/library/crypto/crypto/crypto.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/decoder.h>
#include <openssl/dsa.h>
#include <openssl/ec.h>
#include <openssl/encoder.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <string.h>

void tz_openssl_throw(int32_t kind, const char *otherwise);
const TzMd *tz_openssl_md(void *obj, uint64_t meta, int32_t caller);
EVP_MD *tz_openssl_md_of(const TzMd *md);

extern const TzLayout tz_crypto_sign_layout, tz_crypto_verify_layout;

static TzStr ascii(const char *text) { return tz_str_from_ascii(text, strlen(text)); }

//===----------------------------------------------------------------------===//
// KeyObjects of EVP_PKEYs
//===----------------------------------------------------------------------===//

static void key_release(void *pkey) { EVP_PKEY_free((EVP_PKEY *)pkey); }

// The name node gives the type of a key: 'rsa', 'ec', 'ed25519'...
static const char *key_type_name(const EVP_PKEY *pkey) {
  switch (EVP_PKEY_get_base_id(pkey)) {
  case EVP_PKEY_RSA:
    return "rsa";
  case EVP_PKEY_RSA_PSS:
    return "rsa-pss";
  case EVP_PKEY_DSA:
    return "dsa";
  case EVP_PKEY_DH:
    return "dh";
  case EVP_PKEY_EC:
    return "ec";
  case EVP_PKEY_ED25519:
    return "ed25519";
  case EVP_PKEY_ED448:
    return "ed448";
  case EVP_PKEY_X25519:
    return "x25519";
  case EVP_PKEY_X448:
    return "x448";
  }
  return EVP_PKEY_get0_type_name(pkey);
}

static void key_asymmetric_type(TzStr *out, const TzKey *key) {
  const char *name = key_type_name((const EVP_PKEY *)key->pkey);
  char lower[32];
  size_t n = 0;
  for (; name && name[n] && n < sizeof lower - 1; ++n)
    lower[n] = name[n] >= 'A' && name[n] <= 'Z' ? (char)(name[n] + 32) : name[n];
  lower[n] = 0;
  *out = ascii(lower);
}

static int32_t key_equals(const TzKey *a, const TzKey *b) {
  int equal = EVP_PKEY_eq((const EVP_PKEY *)a->pkey, (const EVP_PKEY *)b->pkey);
  // Keys of two types are not equal; OpenSSL notes an error, which is no error here.
  ERR_clear_error();
  return equal == 1;
}

static void export_pem(TzStr *out, const TzKey *key, const char *type, const char *cipher,
                       const uint8_t *passphrase, size_t passphrase_length);
static TzBytes *export_der(const TzKey *key, const char *type, const char *cipher,
                           const uint8_t *passphrase, size_t passphrase_length);
static TzJson *export_jwk(const TzKey *key);

static const TzKeyOps key_ops = {key_release, key_equals, key_asymmetric_type,
                                 export_pem,  export_der, export_jwk};

// A KeyObject that takes over `pkey`.
TzKey *tz_openssl_key_of(void *pkey, int private_key) {
  TzKey *key = (TzKey *)tz_obj_alloc(sizeof(TzKey));
  memset(key, 0, sizeof *key);
  key->rc = 1;
  key->layout = private_key ? &tz_crypto_private_key_layout : &tz_crypto_public_key_layout;
  key->type = private_key ? TZ_KEY_PRIVATE : TZ_KEY_PUBLIC;
  key->pkey = pkey;
  key->ops = &key_ops;
  return key;
}

// The public half of a key, as a key of its own.
static EVP_PKEY *public_half(const EVP_PKEY *pkey) {
  unsigned char *der = NULL;
  int n = i2d_PUBKEY(pkey, &der);
  if (n <= 0)
    return NULL;
  const unsigned char *p = der;
  EVP_PKEY *pub = d2i_PUBKEY(NULL, &p, n);
  OPENSSL_free(der);
  return pub;
}

// A KeyObject given where a key goes, or NULL.
static const TzKey *key_object(const TzType *type, const void *value) {
  if (!type)
    return NULL;
  type = tz_resolve(type, &value);
  if (type->kind != TZ_T_OBJECT || !*(void *const *)value)
    return NULL;
  const TzKey *key = *(const TzKey *const *)value;
  if (key->layout == &tz_crypto_secret_key_layout || key->layout == &tz_crypto_public_key_layout ||
      key->layout == &tz_crypto_private_key_layout)
    return key;
  return NULL;
}

//===----------------------------------------------------------------------===//
// Reading keys: PEM, DER, JWK
//===----------------------------------------------------------------------===//

static EVP_PKEY *decode(const TzCryptoData *d, const char *input, const char *structure,
                        int selection, const TzCryptoData *passphrase) {
  EVP_PKEY *pkey = NULL;
  OSSL_DECODER_CTX *ctx =
      OSSL_DECODER_CTX_new_for_pkey(&pkey, input, structure, NULL, selection, NULL, NULL);
  if (!ctx)
    return NULL;
  if (passphrase && passphrase->data)
    OSSL_DECODER_CTX_set_passphrase(ctx, passphrase->data, passphrase->length);
  const unsigned char *p = d->data;
  size_t n = d->length;
  OSSL_DECODER_from_data(ctx, &p, &n);
  OSSL_DECODER_CTX_free(ctx);
  return pkey;
}

// The structure of a DER key by node's name of its type.
static const char *der_structure(const char *type) {
  if (strcmp(type, "spki") == 0)
    return "SubjectPublicKeyInfo";
  if (strcmp(type, "pkcs8") == 0)
    return "PrivateKeyInfo";
  if (strcmp(type, "pkcs1") == 0 || strcmp(type, "sec1") == 0)
    return "type-specific";
  return NULL;
}

static TzJson *json_get(const TzJson *j, const char *key) {
  TzStr k = ascii(key);
  TzJson *value = tz_json_get(j, k.obj, k.meta);
  tz_str_release_value(k);
  return value;
}

// A string member of a JWK into `out`, or 0.
static int jwk_string(const TzJson *jwk, const char *key, char *out, size_t size) {
  TzJson *value = json_get(jwk, key);
  int ok = value && value->kind == TZ_JSON_STRING;
  if (ok)
    tz_crypto_cstr(value->u.string.obj, value->u.string.meta, out, size);
  tz_json_release(value);
  return ok;
}

// A base64url member of a JWK as bytes, or NULL.
static TzBytes *jwk_bytes(const TzJson *jwk, const char *key) {
  TzJson *value = json_get(jwk, key);
  TzBytes *b = NULL;
  if (value && value->kind == TZ_JSON_STRING)
    b = tz_buffer_from_string(value->u.string.obj, value->u.string.meta, TZ_ENC_BASE64URL);
  tz_json_release(value);
  return b;
}

static void release_bytes(TzBytes *b) {
  if (b)
    tz_instance_release((TzInstance *)b);
}

static int push_bn(OSSL_PARAM_BLD *bld, const char *name, const TzBytes *b, BIGNUM **keep) {
  if (!b)
    return 1;
  *keep = BN_bin2bn(b->data, b->length, NULL);
  return OSSL_PARAM_BLD_push_BN(bld, name, *keep);
}

static EVP_PKEY *from_params(const char *type, OSSL_PARAM_BLD *bld, int private_key) {
  OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(bld);
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, type, NULL);
  EVP_PKEY *pkey = NULL;
  if (ctx && params && EVP_PKEY_fromdata_init(ctx) > 0)
    EVP_PKEY_fromdata(ctx, &pkey, private_key ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, params);
  EVP_PKEY_CTX_free(ctx);
  OSSL_PARAM_free(params);
  return pkey;
}

// The name OpenSSL gives the curve of a JWK.
static const char *jwk_curve(const char *crv) {
  if (strcmp(crv, "P-256") == 0)
    return "prime256v1";
  if (strcmp(crv, "P-384") == 0)
    return "secp384r1";
  if (strcmp(crv, "P-521") == 0)
    return "secp521r1";
  if (strcmp(crv, "secp256k1") == 0)
    return "secp256k1";
  return NULL;
}

static EVP_PKEY *from_jwk(const TzJson *jwk, int private_key) {
  char kty[16], crv[16];
  if (!jwk || jwk->kind != TZ_JSON_OBJECT || !jwk_string(jwk, "kty", kty, sizeof kty)) {
    tz_throw(TZ_TYPE_ERROR, "The \"key.kty\" property must be of type string.");
    return NULL;
  }
  EVP_PKEY *pkey = NULL;
  if (strcmp(kty, "OKP") == 0) {
    const char *type = NULL;
    if (jwk_string(jwk, "crv", crv, sizeof crv))
      type = strcmp(crv, "Ed25519") == 0  ? "ED25519"
             : strcmp(crv, "Ed448") == 0  ? "ED448"
             : strcmp(crv, "X25519") == 0 ? "X25519"
             : strcmp(crv, "X448") == 0   ? "X448"
                                          : NULL;
    if (!type) {
      tz_throw(TZ_TYPE_ERROR, "The property 'key.crv' must be one of: 'Ed25519', 'Ed448', "
                              "'X25519', 'X448'.");
      return NULL;
    }
    TzBytes *d = private_key ? jwk_bytes(jwk, "d") : NULL, *x = jwk_bytes(jwk, "x");
    if (private_key && d)
      pkey = EVP_PKEY_new_raw_private_key_ex(NULL, type, NULL, d->data, (size_t)d->length);
    else if (!private_key && x)
      pkey = EVP_PKEY_new_raw_public_key_ex(NULL, type, NULL, x->data, (size_t)x->length);
    release_bytes(d);
    release_bytes(x);
    if (!pkey)
      tz_throw(TZ_TYPE_ERROR, "Invalid JWK %s key", crv);
    return pkey;
  }
  OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
  if (strcmp(kty, "RSA") == 0) {
    static const char *const names[] = {"n", "e", "d", "p", "q", "dp", "dq", "qi"};
    static const char *const params[] = {
        OSSL_PKEY_PARAM_RSA_N,         OSSL_PKEY_PARAM_RSA_E,
        OSSL_PKEY_PARAM_RSA_D,         OSSL_PKEY_PARAM_RSA_FACTOR1,
        OSSL_PKEY_PARAM_RSA_FACTOR2,   OSSL_PKEY_PARAM_RSA_EXPONENT1,
        OSSL_PKEY_PARAM_RSA_EXPONENT2, OSSL_PKEY_PARAM_RSA_COEFFICIENT1};
    BIGNUM *bn[8] = {0};
    TzBytes *parts[8] = {0};
    int count = private_key ? 8 : 2, ok = 1;
    for (int i = 0; i < count; ++i)
      parts[i] = jwk_bytes(jwk, names[i]);
    ok = parts[0] && parts[1] && (!private_key || parts[2]);
    for (int i = 0; ok && i < count; ++i)
      ok = push_bn(bld, params[i], parts[i], &bn[i]);
    if (ok)
      pkey = from_params("RSA", bld, private_key);
    for (int i = 0; i < 8; ++i) {
      release_bytes(parts[i]);
      BN_clear_free(bn[i]);
    }
    if (!pkey)
      tz_throw(TZ_TYPE_ERROR, "Invalid JWK RSA key");
  } else if (strcmp(kty, "EC") == 0) {
    const char *group = jwk_string(jwk, "crv", crv, sizeof crv) ? jwk_curve(crv) : NULL;
    if (!group) {
      OSSL_PARAM_BLD_free(bld);
      tz_throw(TZ_TYPE_ERROR, "The property 'key.crv' must be one of: 'P-256', 'secp256k1', "
                              "'P-384', 'P-521'.");
      return NULL;
    }
    TzBytes *x = jwk_bytes(jwk, "x"), *y = jwk_bytes(jwk, "y");
    TzBytes *d = private_key ? jwk_bytes(jwk, "d") : NULL;
    BIGNUM *priv = NULL;
    int ok = x && y && x->length == y->length && (!private_key || d);
    if (ok) {
      size_t n = (size_t)x->length;
      uint8_t *point = (uint8_t *)tz_alloc(1 + 2 * n);
      point[0] = 4;
      memcpy(point + 1, x->data, n);
      memcpy(point + 1 + n, y->data, n);
      ok = OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, group, 0) &&
           OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, point, 1 + 2 * n) &&
           push_bn(bld, OSSL_PKEY_PARAM_PRIV_KEY, d, &priv);
      if (ok)
        pkey = from_params("EC", bld, private_key);
      tz_free(point);
    }
    release_bytes(x);
    release_bytes(y);
    release_bytes(d);
    BN_clear_free(priv);
    if (!pkey)
      tz_throw(TZ_TYPE_ERROR, "Invalid JWK EC key");
  } else {
    tz_throw(TZ_TYPE_ERROR,
             "The property 'key.kty' must be one of: 'RSA', 'EC', 'OKP'. "
             "Received '%s'",
             kty);
  }
  OSSL_PARAM_BLD_free(bld);
  return pkey;
}

// createPublicKey(key) and createPrivateKey(key): the key as a string or bytes (PEM, or DER with
// `format` 1), a JWK (`format` 2, a Json), or a KeyObject; `type` its DER type ('spki'...); a
// passphrase for an encrypted private key (`has`: 1 type, 2 passphrase).
TzKey *tz_openssl_create_key(int32_t private_key, const TzType *kt, const void *kv,
                             int32_t encoding, int32_t format, int32_t has, void *type_obj,
                             uint64_t type_meta, const TzType *pt, const void *pv) {
  const TzKey *given = key_object(kt, kv);
  if (given) {
    if (private_key || given->type == TZ_KEY_SECRET) {
      tz_throw(TZ_TYPE_ERROR,
               "The \"key\" argument must be of type string or an instance of "
               "ArrayBuffer, Buffer, TypedArray, or DataView. Received an instance "
               "of %s",
               given->layout->name);
      return NULL;
    }
    if (given->type == TZ_KEY_PUBLIC) {
      EVP_PKEY_up_ref((EVP_PKEY *)given->pkey);
      return tz_openssl_key_of((EVP_PKEY *)given->pkey, 0);
    }
    EVP_PKEY *pub = public_half((const EVP_PKEY *)given->pkey);
    return pub ? tz_openssl_key_of(pub, 0) : NULL;
  }
  if (format == 2) {
    const void *value = kv;
    const TzType *type = tz_resolve(kt, &value);
    const TzJson *jwk = type->kind == TZ_T_JSON ? *(const TzJson *const *)value : NULL;
    EVP_PKEY *pkey = from_jwk(jwk, private_key);
    return pkey ? tz_openssl_key_of(pkey, private_key) : NULL;
  }
  TzCryptoData d, passphrase;
  if (!tz_crypto_data(&d, kt, kv, encoding, "key"))
    return NULL;
  if (!tz_crypto_data(&passphrase, (has & 2) ? pt : NULL, pv, TZ_ENC_UTF8, "passphrase")) {
    tz_crypto_data_done(&d);
    return NULL;
  }
  char type[16] = "";
  if (has & 1)
    tz_crypto_cstr(type_obj, type_meta, type, sizeof type);
  const char *input = format == 1 ? "DER" : "PEM";
  const char *structure = format == 1 && type[0] ? der_structure(type) : NULL;
  EVP_PKEY *pkey = NULL;
  int is_private = private_key;
  if (private_key) {
    pkey = decode(&d, input, structure, EVP_PKEY_KEYPAIR, &passphrase);
  } else {
    pkey = decode(&d, input, structure, EVP_PKEY_PUBLIC_KEY, NULL);
    if (!pkey && format != 1) { // a certificate: its key
      BIO *bio = BIO_new_mem_buf(d.data, (int)d.length);
      X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
      BIO_free(bio);
      if (cert) {
        ERR_clear_error();
        pkey = X509_get_pubkey(cert);
        X509_free(cert);
      }
    }
    if (!pkey) { // a private key: its public half
      EVP_PKEY *both = decode(&d, input, structure, EVP_PKEY_KEYPAIR, &passphrase);
      if (both) {
        ERR_clear_error();
        pkey = public_half(both);
        EVP_PKEY_free(both);
      }
    }
  }
  tz_crypto_data_done(&passphrase);
  tz_crypto_data_done(&d);
  if (!pkey) {
    tz_openssl_throw(TZ_ERROR, "Failed to read asymmetric key");
    return NULL;
  }
  ERR_clear_error();
  return tz_openssl_key_of(pkey, is_private);
}

// The public key of a private one, of generateKeyPairSync.
TzKey *tz_openssl_public_of(const TzKey *key) {
  EVP_PKEY *pub = public_half((const EVP_PKEY *)key->pkey);
  if (!pub) {
    tz_openssl_throw(TZ_ERROR, "Failed to get the public key");
    return NULL;
  }
  return tz_openssl_key_of(pub, 0);
}

//===----------------------------------------------------------------------===//
// Writing keys
//===----------------------------------------------------------------------===//

// The data of a key in PEM or DER: NULL after throwing node's error.
static unsigned char *encode(const TzKey *key, const char *output, const char *type,
                             const char *cipher, const uint8_t *passphrase,
                             size_t passphrase_length, size_t *length) {
  const EVP_PKEY *pkey = (const EVP_PKEY *)key->pkey;
  int private_key = key->type == TZ_KEY_PRIVATE;
  int id = EVP_PKEY_get_base_id(pkey);
  const char *structure = NULL;
  if (!type[0]) {
    tz_throw(TZ_TYPE_ERROR, "The property 'options.type' is invalid. Received undefined");
    return NULL;
  }
  if (strcmp(type, "pkcs1") == 0) {
    if (id != EVP_PKEY_RSA) {
      tz_throw(TZ_ERROR, "The selected key encoding pkcs1 can only be used for RSA keys.");
      return NULL;
    }
    structure = "type-specific";
  } else if (!private_key && strcmp(type, "spki") == 0) {
    structure = "SubjectPublicKeyInfo";
  } else if (private_key && strcmp(type, "pkcs8") == 0) {
    structure = cipher[0] ? "EncryptedPrivateKeyInfo" : "PrivateKeyInfo";
  } else if (private_key && strcmp(type, "sec1") == 0) {
    if (id != EVP_PKEY_EC) {
      tz_throw(TZ_ERROR, "The selected key encoding sec1 can only be used for EC keys.");
      return NULL;
    }
    structure = "type-specific";
  } else {
    tz_throw(TZ_TYPE_ERROR, "The property 'options.type' is invalid. Received '%s'", type);
    return NULL;
  }
  if (cipher[0] && !private_key) {
    tz_throw(TZ_TYPE_ERROR, "The property 'options.cipher' is invalid. Received '%s'", cipher);
    return NULL;
  }
  int selection = private_key ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY;
  OSSL_ENCODER_CTX *ctx = OSSL_ENCODER_CTX_new_for_pkey(pkey, selection, output, structure, NULL);
  unsigned char *data = NULL;
  int ok = ctx && OSSL_ENCODER_CTX_get_num_encoders(ctx) > 0;
  if (ok && cipher[0])
    ok = OSSL_ENCODER_CTX_set_cipher(ctx, cipher, NULL) &&
         OSSL_ENCODER_CTX_set_passphrase(ctx, passphrase, passphrase_length);
  if (ok)
    ok = OSSL_ENCODER_to_data(ctx, &data, length);
  OSSL_ENCODER_CTX_free(ctx);
  if (!ok) {
    tz_openssl_throw(TZ_ERROR, "Failed to encode the key");
    OPENSSL_free(data);
    return NULL;
  }
  return data;
}

static void export_pem(TzStr *out, const TzKey *key, const char *type, const char *cipher,
                       const uint8_t *passphrase, size_t passphrase_length) {
  size_t length;
  unsigned char *data = encode(key, "PEM", type, cipher, passphrase, passphrase_length, &length);
  if (!data)
    return;
  tz_str_release_value(*out);
  tz_crypto_text(out, data, length, TZ_ENC_UTF8);
  OPENSSL_free(data);
}

static TzBytes *export_der(const TzKey *key, const char *type, const char *cipher,
                           const uint8_t *passphrase, size_t passphrase_length) {
  size_t length;
  unsigned char *data = encode(key, "DER", type, cipher, passphrase, passphrase_length, &length);
  if (!data)
    return NULL;
  TzBytes *b = tz_crypto_buffer(data, length);
  OPENSSL_free(data);
  return b;
}

static void put_text(TzJson *jwk, const char *key, const char *text) {
  TzStr k = ascii(key), v = ascii(text);
  tz_json_object_put(jwk, k.obj, k.meta, tz_json_string(v.obj, v.meta));
  tz_str_release_value(v);
  tz_str_release_value(k);
}

static void put_base64url(TzJson *jwk, const char *key, const uint8_t *data, size_t length) {
  TzStr k = ascii(key), v;
  tz_crypto_text(&v, data, length, TZ_ENC_BASE64URL);
  tz_json_object_put(jwk, k.obj, k.meta, tz_json_string(v.obj, v.meta));
  tz_str_release_value(v);
  tz_str_release_value(k);
}

// A number of the key as base64url, in `size` bytes (0: as few as it takes).
static int put_bn(TzJson *jwk, const char *key, const EVP_PKEY *pkey, const char *param, int size) {
  BIGNUM *bn = NULL;
  if (!EVP_PKEY_get_bn_param(pkey, param, &bn))
    return 0;
  int n = size ? size : BN_num_bytes(bn);
  uint8_t *bytes = (uint8_t *)tz_alloc((size_t)n + 1);
  BN_bn2binpad(bn, bytes, n);
  put_base64url(jwk, key, bytes, (size_t)n);
  memset(bytes, 0, (size_t)n);
  tz_free(bytes);
  BN_clear_free(bn);
  return 1;
}

// export({ format: 'jwk' }), with the members in node's order.
static TzJson *export_jwk(const TzKey *key) {
  const EVP_PKEY *pkey = (const EVP_PKEY *)key->pkey;
  int private_key = key->type == TZ_KEY_PRIVATE;
  int id = EVP_PKEY_get_base_id(pkey);
  TzJson *jwk = tz_json_object_new();
  if (id == EVP_PKEY_RSA) {
    put_text(jwk, "kty", "RSA");
    put_bn(jwk, "n", pkey, OSSL_PKEY_PARAM_RSA_N, 0);
    put_bn(jwk, "e", pkey, OSSL_PKEY_PARAM_RSA_E, 0);
    if (private_key) {
      put_bn(jwk, "d", pkey, OSSL_PKEY_PARAM_RSA_D, 0);
      put_bn(jwk, "p", pkey, OSSL_PKEY_PARAM_RSA_FACTOR1, 0);
      put_bn(jwk, "q", pkey, OSSL_PKEY_PARAM_RSA_FACTOR2, 0);
      put_bn(jwk, "dp", pkey, OSSL_PKEY_PARAM_RSA_EXPONENT1, 0);
      put_bn(jwk, "dq", pkey, OSSL_PKEY_PARAM_RSA_EXPONENT2, 0);
      put_bn(jwk, "qi", pkey, OSSL_PKEY_PARAM_RSA_COEFFICIENT1, 0);
    }
    return jwk;
  }
  if (id == EVP_PKEY_EC) {
    char group[64] = "";
    EVP_PKEY_get_utf8_string_param(pkey, OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof group, NULL);
    const char *crv = strcmp(group, "prime256v1") == 0  ? "P-256"
                      : strcmp(group, "secp384r1") == 0 ? "P-384"
                      : strcmp(group, "secp521r1") == 0 ? "P-521"
                      : strcmp(group, "secp256k1") == 0 ? "secp256k1"
                                                        : NULL;
    if (!crv) {
      tz_json_release(jwk);
      tz_throw(TZ_ERROR, "Unsupported JWK EC curve: %s.", group);
      return NULL;
    }
    int size = (EVP_PKEY_get_bits(pkey) + 7) / 8;
    uint8_t point[256];
    size_t length = 0;
    EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, point, sizeof point,
                                    &length);
    if (length == (size_t)(1 + 2 * size) && point[0] == 4) {
      put_text(jwk, "kty", "EC");
      put_base64url(jwk, "x", point + 1, (size_t)size);
      put_base64url(jwk, "y", point + 1 + size, (size_t)size);
    } else { // compressed: the point in full
      EC_GROUP *g = EC_GROUP_new_by_curve_name(OBJ_sn2nid(group));
      EC_POINT *p = EC_POINT_new(g);
      uint8_t full[256];
      EC_POINT_oct2point(g, p, point, length, NULL);
      EC_POINT_point2oct(g, p, POINT_CONVERSION_UNCOMPRESSED, full, sizeof full, NULL);
      put_text(jwk, "kty", "EC");
      put_base64url(jwk, "x", full + 1, (size_t)size);
      put_base64url(jwk, "y", full + 1 + size, (size_t)size);
      EC_POINT_free(p);
      EC_GROUP_free(g);
    }
    put_text(jwk, "crv", crv);
    if (private_key)
      put_bn(jwk, "d", pkey, OSSL_PKEY_PARAM_PRIV_KEY, size);
    return jwk;
  }
  const char *crv = id == EVP_PKEY_ED25519  ? "Ed25519"
                    : id == EVP_PKEY_ED448  ? "Ed448"
                    : id == EVP_PKEY_X25519 ? "X25519"
                    : id == EVP_PKEY_X448   ? "X448"
                                            : NULL;
  if (!crv) {
    tz_json_release(jwk);
    tz_throw(TZ_ERROR, "Unsupported JWK key type");
    return NULL;
  }
  uint8_t raw[64];
  size_t length = sizeof raw;
  put_text(jwk, "crv", crv);
  if (private_key && EVP_PKEY_get_raw_private_key(pkey, raw, &length)) {
    put_base64url(jwk, "d", raw, length);
    memset(raw, 0, sizeof raw);
  }
  length = sizeof raw;
  if (EVP_PKEY_get_raw_public_key(pkey, raw, &length))
    put_base64url(jwk, "x", raw, length);
  put_text(jwk, "kty", "OKP");
  return jwk;
}

//===----------------------------------------------------------------------===//
// Signatures
//===----------------------------------------------------------------------===//

// The key a signature takes: a KeyObject of the right type, or PEM or DER data of one.
static EVP_PKEY *signing_key(const TzType *kt, const void *kv, int private_key, int *owned) {
  *owned = 0;
  const TzKey *key = key_object(kt, kv);
  if (key) {
    if (key->type == TZ_KEY_SECRET || (private_key && key->type != TZ_KEY_PRIVATE)) {
      tz_throw(TZ_TYPE_ERROR, "Invalid key object type %s, expected %s.",
               key->type == TZ_KEY_SECRET ? "secret" : "public",
               private_key ? "private" : "public");
      return NULL;
    }
    return (EVP_PKEY *)key->pkey;
  }
  TzKey *made = tz_openssl_create_key(private_key, kt, kv, TZ_ENC_UTF8, 0, 0, NULL, 0, NULL, NULL);
  if (!made)
    return NULL;
  EVP_PKEY *pkey = (EVP_PKEY *)made->pkey;
  made->pkey = NULL;
  tz_instance_release((TzInstance *)made);
  *owned = 1;
  return pkey;
}

// The options of a signature: RSA padding and salt length, and the encoding of DSA and ECDSA
// signatures (0 'der', 1 'ieee-p1363'); `has` says which were given (1 padding, 2 saltLength).
typedef struct {
  int32_t padding, salt_length, p1363, has;
} SignOptions;

static int apply_options(EVP_PKEY_CTX *pctx, const EVP_PKEY *pkey, const SignOptions *o) {
  int id = EVP_PKEY_get_base_id(pkey);
  if (id != EVP_PKEY_RSA && id != EVP_PKEY_RSA_PSS)
    return 1;
  if ((o->has & 1) && EVP_PKEY_CTX_set_rsa_padding(pctx, o->padding) <= 0)
    return 0;
  if ((o->has & 2) && EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, o->salt_length) <= 0)
    return 0;
  return 1;
}

// The size of each half of a P1363 signature of an ECDSA or DSA key.
static int p1363_half(const EVP_PKEY *pkey) {
  int id = EVP_PKEY_get_base_id(pkey);
  if (id == EVP_PKEY_EC)
    return (EVP_PKEY_get_bits(pkey) + 7) / 8;
  if (id == EVP_PKEY_DSA) {
    BIGNUM *q = NULL;
    EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_FFC_Q, &q);
    int n = q ? BN_num_bytes(q) : 0;
    BN_free(q);
    return n;
  }
  return 0;
}

// A DER signature (`sig`, freed) as r and s of `half` bytes each.
static TzBytes *der_to_p1363(unsigned char *sig, size_t length, int half) {
  const unsigned char *p = sig;
  ECDSA_SIG *s = d2i_ECDSA_SIG(NULL, &p, (long)length);
  TzBytes *b = NULL;
  if (s) {
    b = tz_buffer_alloc(2 * half, 1);
    const BIGNUM *r, *ss;
    ECDSA_SIG_get0(s, &r, &ss);
    BN_bn2binpad(r, b->data, half);
    BN_bn2binpad(ss, b->data + half, half);
    ECDSA_SIG_free(s);
  }
  return b;
}

// r and s of `half` bytes each as a DER signature (OPENSSL_free), or NULL.
static unsigned char *p1363_to_der(const uint8_t *sig, size_t length, int half, size_t *out) {
  if (length != (size_t)(2 * half))
    return NULL;
  ECDSA_SIG *s = ECDSA_SIG_new();
  ECDSA_SIG_set0(s, BN_bin2bn(sig, half, NULL), BN_bin2bn(sig + half, half, NULL));
  unsigned char *der = NULL;
  int n = i2d_ECDSA_SIG(s, &der);
  ECDSA_SIG_free(s);
  *out = n > 0 ? (size_t)n : 0;
  return n > 0 ? der : NULL;
}

// The signature of data, or of a digest already made (`digest`, of `md`).
static TzBytes *sign_with(EVP_PKEY *pkey, const char *md_name, const EVP_MD *md,
                          const uint8_t *data, size_t length, const SignOptions *o, int digested) {
  unsigned char *sig = NULL;
  size_t sig_length = 0;
  int ok;
  EVP_MD_CTX *ctx = NULL;
  EVP_PKEY_CTX *pctx = NULL;
  if (digested) {
    pctx = EVP_PKEY_CTX_new(pkey, NULL);
    ok = pctx && EVP_PKEY_sign_init(pctx) > 0 && apply_options(pctx, pkey, o) &&
         EVP_PKEY_CTX_set_signature_md(pctx, md) > 0 &&
         EVP_PKEY_sign(pctx, NULL, &sig_length, data, length) > 0;
    if (ok) {
      sig = (unsigned char *)OPENSSL_malloc(sig_length);
      ok = EVP_PKEY_sign(pctx, sig, &sig_length, data, length) > 0;
    }
    EVP_PKEY_CTX_free(pctx);
  } else {
    ctx = EVP_MD_CTX_new();
    ok = EVP_DigestSignInit_ex(ctx, &pctx, md_name, NULL, NULL, pkey, NULL) > 0 &&
         apply_options(pctx, pkey, o) && EVP_DigestSign(ctx, NULL, &sig_length, data, length) > 0;
    if (ok) {
      sig = (unsigned char *)OPENSSL_malloc(sig_length);
      ok = EVP_DigestSign(ctx, sig, &sig_length, data, length) > 0;
    }
    EVP_MD_CTX_free(ctx);
  }
  if (!ok) {
    OPENSSL_free(sig);
    tz_openssl_throw(TZ_ERROR, "Signing failed");
    return NULL;
  }
  TzBytes *b;
  int half = o->p1363 ? p1363_half(pkey) : 0;
  b = half ? der_to_p1363(sig, sig_length, half) : tz_crypto_buffer(sig, sig_length);
  OPENSSL_free(sig);
  return b;
}

static int32_t verify_with(EVP_PKEY *pkey, const char *md_name, const EVP_MD *md,
                           const uint8_t *data, size_t length, const uint8_t *sig,
                           size_t sig_length, const SignOptions *o, int digested) {
  unsigned char *der = NULL;
  int half = o->p1363 ? p1363_half(pkey) : 0;
  if (half) {
    der = p1363_to_der(sig, sig_length, half, &sig_length);
    if (!der)
      return 0;
    sig = der;
  }
  int ok;
  EVP_PKEY_CTX *pctx = NULL;
  if (digested) {
    pctx = EVP_PKEY_CTX_new(pkey, NULL);
    ok = pctx && EVP_PKEY_verify_init(pctx) > 0 && apply_options(pctx, pkey, o) &&
         EVP_PKEY_CTX_set_signature_md(pctx, md) > 0 &&
         EVP_PKEY_verify(pctx, sig, sig_length, data, length) == 1;
    EVP_PKEY_CTX_free(pctx);
  } else {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    ok = EVP_DigestVerifyInit_ex(ctx, &pctx, md_name, NULL, NULL, pkey, NULL) > 0 &&
         apply_options(pctx, pkey, o) && EVP_DigestVerify(ctx, sig, sig_length, data, length) == 1;
    EVP_MD_CTX_free(ctx);
  }
  OPENSSL_free(der);
  ERR_clear_error();
  return ok;
}

// The name of the hash of crypto.sign and verify: NULL for undefined (Ed25519, or the default of
// the key); `name` holds it.
static const char *algorithm(const TzType *at, const void *av, char *name, size_t size) {
  if (!at)
    return NULL;
  const void *value = av;
  const TzType *type = tz_resolve(at, &value);
  if (type->kind != TZ_T_STRING)
    return NULL;
  const TzStr *s = (const TzStr *)value;
  tz_crypto_cstr(s->obj, s->meta, name, size);
  return name;
}

// crypto.sign(algorithm, data, key) and crypto.verify(algorithm, data, key, signature).
TzBytes *tz_openssl_sign(const TzType *at, const void *av, const TzType *dt, const void *dv,
                         const TzType *kt, const void *kv, int32_t padding, int32_t salt_length,
                         int32_t p1363, int32_t has) {
  SignOptions o = {padding, salt_length, p1363, has};
  char name[64];
  const char *md = algorithm(at, av, name, sizeof name);
  int owned;
  EVP_PKEY *pkey = signing_key(kt, kv, 1, &owned);
  if (!pkey)
    return NULL;
  TzCryptoData d;
  TzBytes *sig = NULL;
  if (tz_crypto_data(&d, dt, dv, TZ_ENC_UTF8, "data")) {
    sig = sign_with(pkey, md, NULL, d.data, d.length, &o, 0);
    tz_crypto_data_done(&d);
  }
  if (owned)
    EVP_PKEY_free(pkey);
  return sig;
}

int32_t tz_openssl_verify(const TzType *at, const void *av, const TzType *dt, const void *dv,
                          const TzType *kt, const void *kv, const TzType *st, const void *sv,
                          int32_t padding, int32_t salt_length, int32_t p1363, int32_t has) {
  SignOptions o = {padding, salt_length, p1363, has};
  char name[64];
  const char *md = algorithm(at, av, name, sizeof name);
  int owned;
  EVP_PKEY *pkey = signing_key(kt, kv, 0, &owned);
  if (!pkey)
    return 0;
  TzCryptoData d, s;
  int32_t ok = 0;
  if (tz_crypto_data(&d, dt, dv, TZ_ENC_UTF8, "data")) {
    if (tz_crypto_data(&s, st, sv, TZ_ENC_UTF8, "signature")) {
      ok = verify_with(pkey, md, NULL, d.data, d.length, s.data, s.length, &o, 0);
      tz_crypto_data_done(&s);
    }
    tz_crypto_data_done(&d);
  }
  if (owned)
    EVP_PKEY_free(pkey);
  return ok;
}

// createSign(algorithm) and createVerify(algorithm): a hash of the data, which sign() and
// verify() sign.
TzHash *tz_openssl_sign_new(void *obj, uint64_t meta, int32_t verify) {
  const TzMd *md = tz_openssl_md(obj, meta, 0);
  if (!md)
    return NULL;
  TzHash *h = tz_crypto_hash_alloc(md, 0);
  h->layout = verify ? &tz_crypto_verify_layout : &tz_crypto_sign_layout;
  tz_crypto_md_start(md, h->ctx);
  return h;
}

// The digest of a Sign or a Verify, once: 0 after throwing.
static int sign_digest(TzHash *h, uint8_t *out) {
  if (h->done) {
    tz_throw(TZ_ERROR, "Not initialised");
    return 0;
  }
  h->md->ops->final(h->ctx, out, h->md->size);
  tz_crypto_md_end(h->md, h->ctx);
  h->done = 1;
  return 1;
}

TzBytes *tz_openssl_sign_final(TzHash *h, const TzType *kt, const void *kv, int32_t padding,
                               int32_t salt_length, int32_t p1363, int32_t has) {
  SignOptions o = {padding, salt_length, p1363, has};
  int owned;
  EVP_PKEY *pkey = signing_key(kt, kv, 1, &owned);
  if (!pkey)
    return NULL;
  TzBytes *sig = NULL;
  int id = EVP_PKEY_get_base_id(pkey);
  uint8_t digest[64];
  if (id == EVP_PKEY_ED25519 || id == EVP_PKEY_ED448)
    tz_throw(TZ_ERROR, "Unsupported crypto operation");
  else if (sign_digest(h, digest))
    sig = sign_with(pkey, NULL, tz_openssl_md_of(h->md), digest, h->md->size, &o, 1);
  if (owned)
    EVP_PKEY_free(pkey);
  return sig;
}

int32_t tz_openssl_verify_final(TzHash *h, const TzType *kt, const void *kv, const TzType *st,
                                const void *sv, int32_t encoding, int32_t padding,
                                int32_t salt_length, int32_t p1363, int32_t has) {
  SignOptions o = {padding, salt_length, p1363, has};
  int owned;
  EVP_PKEY *pkey = signing_key(kt, kv, 0, &owned);
  if (!pkey)
    return 0;
  int32_t ok = 0;
  uint8_t digest[64];
  TzCryptoData s;
  if (tz_crypto_data(&s, st, sv, encoding, "signature")) {
    if (sign_digest(h, digest))
      ok = verify_with(pkey, NULL, tz_openssl_md_of(h->md), digest, h->md->size, s.data, s.length,
                       &o, 1);
    tz_crypto_data_done(&s);
  }
  if (owned)
    EVP_PKEY_free(pkey);
  return ok;
}

//===----------------------------------------------------------------------===//
// generateKeyPairSync
//===----------------------------------------------------------------------===//

// The NID of a curve as node takes its name: a NIST name ("P-256") or OpenSSL's.
static int curve_nid(const char *name) {
  int nid = EC_curve_nist2nid(name);
  if (nid == NID_undef)
    nid = OBJ_sn2nid(name);
  return nid;
}

enum {
  PAIR_RSA,
  PAIR_RSA_PSS,
  PAIR_DSA,
  PAIR_EC,
  PAIR_ED25519,
  PAIR_ED448,
  PAIR_X25519,
  PAIR_X448
};

// generateKeyPairSync(type, options): the private key; the type as a PAIR_*, or -1 for a name
// known only now; the curve of 'ec' (`has` 1); the modulus of 'rsa', 'rsa-pss' and 'dsa', with
// the public exponent (`has` 2) or the divisor of 'dsa'.
TzKey *tz_openssl_generate_key_pair(int32_t type, void *name_obj, uint64_t name_meta,
                                    double modulus_length, double exponent, int32_t has,
                                    void *curve_obj, uint64_t curve_meta) {
  if (type < 0) {
    char name[16];
    tz_crypto_cstr(name_obj, name_meta, name, sizeof name);
    static const char *const names[] = {"rsa",     "rsa-pss", "dsa",    "ec",
                                        "ed25519", "ed448",   "x25519", "x448"};
    for (int i = 0; i < 8; ++i)
      if (strcmp(name, names[i]) == 0)
        type = i;
    if (type < 0) {
      tz_throw(TZ_TYPE_ERROR, "The argument 'type' must be a supported key type. Received '%s'",
               name);
      return NULL;
    }
  }
  static const char *const algorithms[] = {"RSA",     "RSA-PSS", "DSA",    "EC",
                                           "ED25519", "ED448",   "X25519", "X448"};
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, algorithms[type], NULL);
  int ok = ctx && EVP_PKEY_keygen_init(ctx) > 0;
  if (ok && (type == PAIR_RSA || type == PAIR_RSA_PSS)) {
    if (!tz_crypto_check_integer(modulus_length, "options.modulusLength", 0, 4294967295.0)) {
      EVP_PKEY_CTX_free(ctx);
      return NULL;
    }
    ok = EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, (int)modulus_length) > 0;
    if (ok && (has & 2)) {
      BIGNUM *e = BN_new();
      BN_set_word(e, (BN_ULONG)exponent);
      ok = EVP_PKEY_CTX_set1_rsa_keygen_pubexp(ctx, e) > 0;
      BN_free(e);
    }
  } else if (ok && type == PAIR_DSA) {
    ok = EVP_PKEY_CTX_set_dsa_paramgen_bits(ctx, (int)modulus_length) > 0;
  } else if (ok && type == PAIR_EC) {
    char curve[64] = "";
    if (has & 1)
      tz_crypto_cstr(curve_obj, curve_meta, curve, sizeof curve);
    int nid = curve_nid(curve);
    if (nid == NID_undef) {
      EVP_PKEY_CTX_free(ctx);
      tz_throw(TZ_TYPE_ERROR, "Invalid EC curve name");
      return NULL;
    }
    ok = EVP_PKEY_CTX_set_group_name(ctx, OBJ_nid2sn(nid)) > 0;
  }
  EVP_PKEY *pkey = NULL;
  if (ok)
    ok = EVP_PKEY_generate(ctx, &pkey) > 0;
  EVP_PKEY_CTX_free(ctx);
  if (!ok) {
    tz_openssl_throw(TZ_ERROR, "Key generation job failed");
    return NULL;
  }
  return tz_openssl_key_of(pkey, 1);
}

//===----------------------------------------------------------------------===//
// ECDH and diffieHellman
//===----------------------------------------------------------------------===//

typedef struct {
  int64_t rc;
  const TzLayout *layout;
  int nid;
  EC_GROUP *group;
  EVP_PKEY *key; // NULL before generateKeys or setPrivateKey
} Ecdh;

static void ecdh_drop(void *object) {
  Ecdh *e = (Ecdh *)object;
  EVP_PKEY_free(e->key);
  EC_GROUP_free(e->group);
  tz_obj_free(e);
}

static const TzLayout ecdh_layout = TZ_CRYPTO_LAYOUT(ecdh_drop, "ECDH");

// createECDH(curveName).
Ecdh *tz_openssl_ecdh_new(void *obj, uint64_t meta) {
  char name[64];
  tz_crypto_cstr(obj, meta, name, sizeof name);
  int nid = curve_nid(name);
  EC_GROUP *group = nid == NID_undef ? NULL : EC_GROUP_new_by_curve_name(nid);
  if (!group) {
    ERR_clear_error();
    tz_throw(TZ_TYPE_ERROR, "Invalid EC curve name");
    return NULL;
  }
  Ecdh *e = (Ecdh *)tz_obj_alloc(sizeof(Ecdh));
  e->rc = 1;
  e->layout = &ecdh_layout;
  e->nid = nid;
  e->group = group;
  e->key = NULL;
  return e;
}

static point_conversion_form_t form_of(int32_t format) {
  return format == 1   ? POINT_CONVERSION_COMPRESSED
         : format == 2 ? POINT_CONVERSION_HYBRID
                       : POINT_CONVERSION_UNCOMPRESSED;
}

// An EC key of the curve from its private number, with the public point computed.
static EVP_PKEY *ec_key(const Ecdh *e, const BIGNUM *priv, const EC_POINT *pub) {
  uint8_t point[256];
  size_t n =
      EC_POINT_point2oct(e->group, pub, POINT_CONVERSION_UNCOMPRESSED, point, sizeof point, NULL);
  OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
  EVP_PKEY *pkey = NULL;
  if (OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, OBJ_nid2sn(e->nid), 0) &&
      OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, point, n) &&
      (!priv || OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, priv)))
    pkey = from_params("EC", bld, priv != NULL);
  OSSL_PARAM_BLD_free(bld);
  return pkey;
}

// The public point of the key in `format`, into out (at least 256 bytes): its length, or 0.
static size_t public_point(const Ecdh *e, int32_t format, uint8_t *out) {
  uint8_t raw[256];
  size_t n = 0;
  if (!e->key || !EVP_PKEY_get_octet_string_param(e->key, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, raw,
                                                  sizeof raw, &n))
    return 0;
  EC_POINT *p = EC_POINT_new(e->group);
  size_t length = 0;
  if (EC_POINT_oct2point(e->group, p, raw, n, NULL))
    length = EC_POINT_point2oct(e->group, p, form_of(format), out, 256, NULL);
  EC_POINT_free(p);
  return length;
}

TzBytes *tz_openssl_ecdh_get_public_key(Ecdh *e, int32_t format) {
  uint8_t point[256];
  size_t n = public_point(e, format, point);
  if (!n) {
    tz_throw(TZ_ERROR, "Failed to get ECDH public key");
    return NULL;
  }
  return tz_crypto_buffer(point, n);
}

TzBytes *tz_openssl_ecdh_generate_keys(Ecdh *e, int32_t format) {
  EVP_PKEY *key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", OBJ_nid2sn(e->nid));
  if (!key) {
    tz_openssl_throw(TZ_ERROR, "Failed to generate key");
    return NULL;
  }
  EVP_PKEY_free(e->key);
  e->key = key;
  return tz_openssl_ecdh_get_public_key(e, format);
}

// getPrivateKey(): the private number, in as few bytes as it takes, as node gives it.
TzBytes *tz_openssl_ecdh_get_private_key(Ecdh *e) {
  BIGNUM *priv = NULL;
  if (!e->key || !EVP_PKEY_get_bn_param(e->key, OSSL_PKEY_PARAM_PRIV_KEY, &priv)) {
    tz_throw(TZ_ERROR, "Failed to get ECDH private key");
    return NULL;
  }
  int n = BN_num_bytes(priv);
  TzBytes *b = tz_buffer_alloc(n, 0);
  BN_bn2bin(priv, b->data);
  BN_clear_free(priv);
  return b;
}

// setPrivateKey(privateKey, encoding): the receiver is handed back.
void tz_openssl_ecdh_set_private_key(Ecdh *e, const TzType *kt, const void *kv, int32_t encoding) {
  TzCryptoData d;
  if (!tz_crypto_data(&d, kt, kv, encoding, "key"))
    return;
  BIGNUM *priv = BN_bin2bn(d.data, (int)d.length, NULL);
  tz_crypto_data_done(&d);
  const BIGNUM *order = EC_GROUP_get0_order(e->group);
  EVP_PKEY *key = NULL;
  if (!BN_is_zero(priv) && BN_cmp(priv, order) < 0) {
    EC_POINT *pub = EC_POINT_new(e->group);
    if (EC_POINT_mul(e->group, pub, priv, NULL, NULL, NULL))
      key = ec_key(e, priv, pub);
    EC_POINT_free(pub);
  }
  BN_clear_free(priv);
  if (!key) {
    ERR_clear_error();
    tz_throw(TZ_RANGE_ERROR, "Private key is not valid for specified curve.");
    return;
  }
  EVP_PKEY_free(e->key);
  e->key = key;
}

static TzBytes *derive(EVP_PKEY *mine, EVP_PKEY *theirs) {
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(mine, NULL);
  size_t n = 0;
  TzBytes *b = NULL;
  if (ctx && EVP_PKEY_derive_init(ctx) > 0 && EVP_PKEY_derive_set_peer(ctx, theirs) > 0 &&
      EVP_PKEY_derive(ctx, NULL, &n) > 0) {
    b = tz_buffer_alloc((int64_t)n, 0);
    if (EVP_PKEY_derive(ctx, b->data, &n) > 0) {
      b->length = (int32_t)n;
    } else {
      tz_instance_release((TzInstance *)b);
      b = NULL;
    }
  }
  EVP_PKEY_CTX_free(ctx);
  if (!b)
    tz_openssl_throw(TZ_ERROR, "Failed to compute the shared secret");
  return b;
}

// computeSecret(otherPublicKey, inputEncoding).
TzBytes *tz_openssl_ecdh_compute_secret(Ecdh *e, const TzType *kt, const void *kv,
                                        int32_t encoding) {
  TzCryptoData d;
  if (!tz_crypto_data(&d, kt, kv, encoding, "otherPublicKey"))
    return NULL;
  EC_POINT *point = EC_POINT_new(e->group);
  int valid = EC_POINT_oct2point(e->group, point, d.data, d.length, NULL);
  tz_crypto_data_done(&d);
  EVP_PKEY *theirs = valid ? ec_key(e, NULL, point) : NULL;
  EC_POINT_free(point);
  ERR_clear_error();
  if (!theirs) {
    tz_throw(TZ_ERROR, "Public key is not valid for specified curve");
    return NULL;
  }
  TzBytes *secret = NULL;
  if (!e->key)
    tz_throw(TZ_ERROR, "Failed to compute ECDH key");
  else
    secret = derive(e->key, theirs);
  EVP_PKEY_free(theirs);
  return secret;
}

// diffieHellman({ privateKey, publicKey }).
TzBytes *tz_openssl_diffie_hellman(const TzKey *priv, const TzKey *pub) {
  if (priv->type != TZ_KEY_PRIVATE) {
    tz_throw(TZ_TYPE_ERROR, "Invalid key object type %s, expected private.",
             priv->type == TZ_KEY_SECRET ? "secret" : "public");
    return NULL;
  }
  if (pub->type == TZ_KEY_SECRET) {
    tz_throw(TZ_TYPE_ERROR, "Invalid key object type secret, expected public.");
    return NULL;
  }
  const EVP_PKEY *a = (const EVP_PKEY *)priv->pkey, *b = (const EVP_PKEY *)pub->pkey;
  int id = EVP_PKEY_get_base_id(a);
  if (id != EVP_PKEY_get_base_id(b) ||
      (id != EVP_PKEY_DH && id != EVP_PKEY_EC && id != EVP_PKEY_X25519 && id != EVP_PKEY_X448)) {
    tz_throw(TZ_ERROR, "Incompatible key types for Diffie-Hellman: %s and %s", key_type_name(a),
             key_type_name(b));
    return NULL;
  }
  return derive((EVP_PKEY *)a, (EVP_PKEY *)b);
}
