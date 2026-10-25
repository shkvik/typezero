// node:crypto (docs/crypto.md): what the two layers share, without OpenSSL (see crypto.h). The
// objects of node:crypto and their methods, HMAC, PBKDF2 and HKDF over any hash, the random
// numbers of the system, timingSafeEqual and secret keys.

#include "crypto.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// After windows.h, whose types it uses.
#include <bcrypt.h>
#pragma comment(lib, "bcrypt")
#endif

//===----------------------------------------------------------------------===//
// Errors, as node words them
//===----------------------------------------------------------------------===//

// A number as node shows it in a message: an integer above 2^32 with `_` between thousands.
static void number_text(char *out, size_t size, double value) {
  if (value == floor(value) && fabs(value) < 1e21) {
    char digits[32];
    snprintf(digits, sizeof digits, "%.0f", fabs(value));
    if (fabs(value) <= 4294967296.0) {
      snprintf(out, size, "%s%s", value < 0 ? "-" : "", digits);
      return;
    }
    size_t n = strlen(digits), o = 0;
    if (value < 0 && o + 1 < size)
      out[o++] = '-';
    for (size_t i = 0; i < n && o + 2 < size; ++i) {
      if (i > 0 && (n - i) % 3 == 0)
        out[o++] = '_';
      out[o++] = digits[i];
    }
    out[o] = 0;
    return;
  }
  TzStr s;
  tz_str_from_f64(&s, value);
  uint32_t length;
  int two;
  const uint8_t *units = (const uint8_t *)tz_str_units(&s, &length, &two);
  size_t n = length < size - 1 ? length : size - 1;
  for (size_t i = 0; i < n; ++i)
    out[i] = two ? (char)((const uint16_t *)(const void *)units)[i] : (char)units[i];
  out[n] = 0;
  tz_str_release_value(s);
}

// validateInt32 and validateInteger of node: whether `value` is a whole number from min to max.
static int check_integer(double value, const char *name, double min, double max) {
  char text[64];
  number_text(text, sizeof text, value);
  if (value != floor(value) || isinf(value)) {
    tz_throw(TZ_RANGE_ERROR,
             "The value of \"%s\" is out of range. It must be an integer. "
             "Received %s",
             name, text);
    return 0;
  }
  if (value < min || value > max) {
    char low[64], high[64];
    number_text(low, sizeof low, min);
    number_text(high, sizeof high, max);
    tz_throw(TZ_RANGE_ERROR,
             "The value of \"%s\" is out of range. It must be >= %s && <= %s. Received %s", name,
             low, high, text);
    return 0;
  }
  return 1;
}

int tz_crypto_check_integer(double value, const char *name, double min, double max) {
  return check_integer(value, name, min, max);
}

static void hash_finalized(void) { tz_throw(TZ_ERROR, "Digest already called"); }

//===----------------------------------------------------------------------===//
// Data
//===----------------------------------------------------------------------===//

int tz_crypto_data(TzCryptoData *out, const TzType *type, const void *value, int32_t encoding,
                   const char *what) {
  out->data = NULL;
  out->length = 0;
  out->made = NULL;
  if (!type)
    return 1;
  type = tz_resolve(type, &value);
  if (type->kind == TZ_T_STRING) {
    const TzStr *s = (const TzStr *)value;
    out->made = tz_buffer_from_string(s->obj, s->meta, encoding);
    out->data = out->made->data;
    out->length = (size_t)out->made->length;
    return 1;
  }
  if (type->kind == TZ_T_OBJECT && *(void *const *)value) {
    const TzInstance *object = *(const TzInstance *const *)value;
    const TzLayout *layout = object->layout;
    if (layout->flags & TZ_LAYOUT_BYTES) {
      if (tz_bytes_kind(object) == TZ_BYTES_ARRAY_BUFFER) {
        const TzArrayBuffer *b = (const TzArrayBuffer *)object;
        out->data = b->data;
        out->length = (size_t)b->length;
        return 1;
      }
      if (tz_bytes_kind(object) <= TZ_BYTES_DATA_VIEW) {
        const TzBytes *b = (const TzBytes *)object;
        out->data = b->data;
        out->length = (size_t)tz_bytes_byte_length(b);
        return 1;
      }
    }
    if (layout == &tz_crypto_secret_key_layout) {
      const TzKey *key = (const TzKey *)object;
      out->data = key->secret;
      out->length = (size_t)key->length;
      return 1;
    }
    if (layout == &tz_crypto_public_key_layout || layout == &tz_crypto_private_key_layout) {
      tz_throw(TZ_TYPE_ERROR, "Invalid key object type %s, expected secret.",
               layout == &tz_crypto_public_key_layout ? "public" : "private");
      return 0;
    }
  }
  tz_throw(TZ_TYPE_ERROR,
           "The \"%s\" argument must be of type string or an instance of ArrayBuffer, Buffer, "
           "TypedArray, or DataView.",
           what);
  return 0;
}

void tz_crypto_data_done(TzCryptoData *d) {
  if (d->made)
    tz_instance_release((TzInstance *)d->made);
  d->made = NULL;
}

TzBytes *tz_crypto_buffer(const uint8_t *data, size_t length) {
  return tz_buffer_copy_of(data, (int64_t)length);
}

void tz_crypto_text(TzStr *out, const uint8_t *data, size_t length, int32_t encoding) {
  *out = tz_buffer_decode(data, (int64_t)length, encoding);
}

size_t tz_crypto_cstr(void *obj, uint64_t meta, char *out, size_t capacity) {
  TzBytes *b = tz_buffer_from_string(obj, meta, TZ_ENC_UTF8);
  size_t n = (size_t)b->length < capacity - 1 ? (size_t)b->length : capacity - 1;
  memcpy(out, b->data, n);
  out[n] = 0;
  tz_instance_release((TzInstance *)b);
  return n;
}

//===----------------------------------------------------------------------===//
// Hash and Hmac
//===----------------------------------------------------------------------===//

static void hash_drop(void *object);
static const TzLayout hash_layout = TZ_CRYPTO_LAYOUT(hash_drop, "Hash");
static const TzLayout hmac_layout = TZ_CRYPTO_LAYOUT(hash_drop, "Hmac");
// createSign and createVerify of the full layer: a hash of the data, signed at the end.
const TzLayout tz_crypto_sign_layout = TZ_CRYPTO_LAYOUT(hash_drop, "Sign");
const TzLayout tz_crypto_verify_layout = TZ_CRYPTO_LAYOUT(hash_drop, "Verify");

// The objects of node:crypto, which the program frees as any object (CodeGen names this drop
// for all their types): each by its own layout.
void tz_crypto_drop(void *object) { ((TzInstance *)object)->layout->drop(object); }

void tz_crypto_md_start(const TzMd *md, void *ctx) {
  if (md->seed)
    memcpy(ctx, md->seed, md->seed_size);
  md->ops->init(ctx);
}

void tz_crypto_md_end(const TzMd *md, void *ctx) {
  if (md->ops->cleanup)
    md->ops->cleanup(ctx);
  memset(ctx, 0, md->ctx_size);
}

static void md_copy(const TzMd *md, void *to, const void *from) {
  if (md->ops->copy)
    md->ops->copy(to, from);
  else
    memcpy(to, from, md->ctx_size);
}

TzHash *tz_crypto_hash_alloc(const TzMd *md, int hmac) {
  size_t contexts = hmac ? 2 : 1;
  TzHash *h = (TzHash *)tz_obj_alloc((int64_t)(sizeof(TzHash) + contexts * md->ctx_size));
  h->rc = 1;
  h->layout = hmac ? &hmac_layout : &hash_layout;
  h->md = md;
  h->length = (int32_t)md->size;
  h->done = 0;
  return h;
}

static void hash_drop(void *object) {
  TzHash *h = (TzHash *)object;
  size_t contexts = h->layout == &hmac_layout ? 2 : 1;
  if (!h->done)
    for (size_t i = 0; i < contexts; ++i)
      tz_crypto_md_end(h->md, h->ctx + i * h->md->ctx_size);
  tz_obj_free(h);
}

// createHash(algorithm, { outputLength }): -1 for no length.
TzHash *tz_crypto_hash_new(const TzMd *md, double length) {
  if (!md)
    return NULL;
  if (length >= 0 && !md->xof && length != md->size) {
    tz_throw(TZ_ERROR, "error:030000B2:digital envelope routines::not XOF or invalid length");
    return NULL;
  }
  if (length >= 0 && !check_integer(length, "options.outputLength", 0, 4294967295.0))
    return NULL;
  TzHash *h = tz_crypto_hash_alloc(md, 0);
  if (length >= 0)
    h->length = (int32_t)length;
  tz_crypto_md_start(md, h->ctx);
  return h;
}

// update(data, inputEncoding) of a Hash and of an Hmac: the receiver is handed back.
void tz_crypto_hash_update(TzHash *h, const TzType *type, const void *value, int32_t encoding) {
  if (h->done) {
    hash_finalized();
    return;
  }
  TzCryptoData d;
  if (!tz_crypto_data(&d, type, value, encoding, "data"))
    return;
  if (d.length)
    h->md->ops->update(h->ctx, d.data, d.length);
  tz_crypto_data_done(&d);
}

// The digest into `out`, at least 64 bytes or h->length: 0 when there is none to give (an Hmac
// digested before gives an empty one, as in node).
static int64_t hash_finish(TzHash *h, uint8_t *out) {
  const TzMd *md = h->md;
  if (h->layout == &hmac_layout) {
    if (h->done)
      return 0;
    uint8_t inner[64];
    md->ops->final(h->ctx, inner, md->size);
    uint8_t *outer = h->ctx + md->ctx_size;
    md->ops->update(outer, inner, md->size);
    md->ops->final(outer, out, md->size);
    tz_crypto_md_end(md, h->ctx);
    tz_crypto_md_end(md, outer);
    h->done = 1;
    return md->size;
  }
  if (h->done) {
    hash_finalized();
    return -1;
  }
  md->ops->final(h->ctx, out, (size_t)h->length);
  tz_crypto_md_end(md, h->ctx);
  h->done = 1;
  return h->length;
}

TzBytes *tz_crypto_hash_digest(TzHash *h) {
  uint8_t small[64];
  uint8_t *out = h->length <= 64 ? small : (uint8_t *)tz_alloc((size_t)h->length);
  int64_t n = hash_finish(h, out);
  TzBytes *b = n < 0 ? NULL : tz_crypto_buffer(out, (size_t)n);
  if (out != small)
    tz_free(out);
  return b;
}

void tz_crypto_hash_digest_text(TzStr *out, TzHash *h, int32_t encoding) {
  uint8_t small[64];
  uint8_t *bytes = h->length <= 64 ? small : (uint8_t *)tz_alloc((size_t)h->length);
  int64_t n = hash_finish(h, bytes);
  if (n < 0)
    *out = tz_str_from_ascii("", 0);
  else
    tz_crypto_text(out, bytes, (size_t)n, encoding);
  if (bytes != small)
    tz_free(bytes);
}

// copy({ outputLength }): -1 for the length of the original.
TzHash *tz_crypto_hash_copy(TzHash *h, double length) {
  if (h->done) {
    hash_finalized();
    return NULL;
  }
  if (length >= 0 && !h->md->xof && length != h->md->size) {
    tz_throw(TZ_ERROR, "error:030000B2:digital envelope routines::not XOF or invalid length");
    return NULL;
  }
  TzHash *copy = tz_crypto_hash_alloc(h->md, 0);
  copy->length = length >= 0 ? (int32_t)length : h->length;
  md_copy(h->md, copy->ctx, h->ctx);
  return copy;
}

// Memory for the contexts of a computation, which the threads of the pool make too (Web Crypto,
// crypto_subtle.c): from the system, as the allocator of the runtime is the main thread's.
static void *scratch(size_t size) {
  void *p = malloc(size ? size : 1);
  if (!p)
    tz_panic("out of memory", -1);
  return p;
}

// The digest of data at once, into `out` (h->size bytes).
static void digest_once(const TzMd *md, const uint8_t *data, size_t length, uint8_t *out) {
  _Alignas(16) uint8_t stack[512];
  void *ctx = md->ctx_size <= sizeof stack ? stack : scratch(md->ctx_size);
  tz_crypto_md_start(md, ctx);
  if (length)
    md->ops->update(ctx, data, length);
  md->ops->final(ctx, out, md->size);
  tz_crypto_md_end(md, ctx);
  if (ctx != stack)
    free(ctx);
}

void tz_crypto_digest_raw(const TzMd *md, const uint8_t *data, size_t length, uint8_t *out) {
  digest_once(md, data, length, out);
}

// crypto.hash(algorithm, data, outputEncoding): the digest as a Buffer for 'buffer'.
TzBytes *tz_crypto_hash_once(const TzMd *md, const TzType *type, const void *value) {
  if (!md)
    return NULL;
  TzCryptoData d;
  if (!tz_crypto_data(&d, type, value, TZ_ENC_UTF8, "data"))
    return NULL;
  uint8_t out[64];
  digest_once(md, d.data, d.length, out);
  tz_crypto_data_done(&d);
  return tz_crypto_buffer(out, md->size);
}

void tz_crypto_hash_once_text(TzStr *out, const TzMd *md, const TzType *type, const void *value,
                              int32_t encoding) {
  *out = tz_str_from_ascii("", 0);
  if (!md)
    return;
  TzCryptoData d;
  if (!tz_crypto_data(&d, type, value, TZ_ENC_UTF8, "data"))
    return;
  uint8_t bytes[64];
  digest_once(md, d.data, d.length, bytes);
  tz_crypto_data_done(&d);
  tz_crypto_text(out, bytes, md->size, encoding);
}

// The inner and outer contexts of HMAC (RFC 2104) keyed in `ctx` (two contexts of md).
static void hmac_key(const TzMd *md, uint8_t *ctx, const uint8_t *key, size_t length) {
  uint8_t block[256], pad[256];
  memset(block, 0, md->block);
  if (length > md->block)
    digest_once(md, key, length, block);
  else if (length)
    memcpy(block, key, length);
  for (int k = 0; k < 2; ++k) {
    uint8_t *c = ctx + (size_t)k * md->ctx_size;
    for (uint32_t i = 0; i < md->block; ++i)
      pad[i] = block[i] ^ (k == 0 ? 0x36 : 0x5c);
    tz_crypto_md_start(md, c);
    md->ops->update(c, pad, md->block);
  }
  memset(block, 0, sizeof block);
  memset(pad, 0, sizeof pad);
}

// Whether HMAC can use `md`: a hash with a fixed output and a block.
static int hmac_takes(const TzMd *md) {
  if (md->xof || md->block == 0 || md->block > 256) {
    tz_throw(TZ_TYPE_ERROR, "Invalid digest: %s", md->name);
    return 0;
  }
  return 1;
}

// createHmac(algorithm, key): the key is a string (utf8), bytes or a secret KeyObject.
TzHash *tz_crypto_hmac_new(const TzMd *md, const TzType *type, const void *value,
                           int32_t encoding) {
  if (!md || !hmac_takes(md))
    return NULL;
  TzCryptoData key;
  if (!tz_crypto_data(&key, type, value, encoding, "key"))
    return NULL;
  TzHash *h = tz_crypto_hash_alloc(md, 1);
  hmac_key(md, h->ctx, key.data, key.length);
  tz_crypto_data_done(&key);
  return h;
}

// HMAC of `data` with the keyed contexts of `keyed`, into out (md->size bytes); `work` holds
// two contexts.
static void hmac_with(const TzMd *md, const uint8_t *keyed, uint8_t *work, const uint8_t *data,
                      size_t length, uint8_t *out) {
  uint8_t *inner = work, *outer = work + md->ctx_size;
  uint8_t mac[64];
  md_copy(md, inner, keyed);
  md_copy(md, outer, keyed + md->ctx_size);
  md->ops->update(inner, data, length);
  md->ops->final(inner, mac, md->size);
  md->ops->update(outer, mac, md->size);
  md->ops->final(outer, out, md->size);
  if (md->ops->cleanup) {
    md->ops->cleanup(inner);
    md->ops->cleanup(outer);
  }
}

static uint8_t *contexts(const TzMd *md, size_t count) {
  return (uint8_t *)scratch(count * md->ctx_size);
}

static void free_contexts(const TzMd *md, uint8_t *ctx, size_t count, int keyed) {
  if (keyed && md->ops->cleanup)
    for (size_t i = 0; i < count; ++i)
      md->ops->cleanup(ctx + i * md->ctx_size);
  memset(ctx, 0, count * md->ctx_size);
  free(ctx);
}

void tz_crypto_hmac_raw(const TzMd *md, const uint8_t *key, size_t key_length, const uint8_t *data,
                        size_t length, uint8_t *out) {
  uint8_t *keyed = contexts(md, 4), *work = keyed + 2 * md->ctx_size;
  hmac_key(md, keyed, key, key_length);
  hmac_with(md, keyed, work, data, length, out);
  free_contexts(md, keyed, 2, 1);
}

//===----------------------------------------------------------------------===//
// PBKDF2 and HKDF
//===----------------------------------------------------------------------===//

// PBKDF2 (RFC 8018) of `md` into out.
void tz_crypto_pbkdf2_raw(const TzMd *md, const uint8_t *password, size_t password_length,
                          const uint8_t *salt, size_t salt_length, double iterations, uint8_t *out,
                          size_t length) {
  uint8_t *keyed = contexts(md, 4), *work = keyed + 2 * md->ctx_size;
  hmac_key(md, keyed, password, password_length);
  uint8_t *salted = (uint8_t *)scratch(salt_length + 4);
  if (salt_length)
    memcpy(salted, salt, salt_length);
  size_t size = md->size, done = 0;
  uint8_t u[64], t[64];
  for (uint32_t block = 1; done < length; ++block) {
    salted[salt_length] = (uint8_t)(block >> 24);
    salted[salt_length + 1] = (uint8_t)(block >> 16);
    salted[salt_length + 2] = (uint8_t)(block >> 8);
    salted[salt_length + 3] = (uint8_t)block;
    hmac_with(md, keyed, work, salted, salt_length + 4, u);
    memcpy(t, u, size);
    for (double i = 1; i < iterations; ++i) {
      hmac_with(md, keyed, work, u, size, u);
      for (size_t k = 0; k < size; ++k)
        t[k] ^= u[k];
    }
    size_t n = length - done < size ? length - done : size;
    memcpy(out + done, t, n);
    done += n;
  }
  memset(u, 0, sizeof u);
  memset(t, 0, sizeof t);
  free(salted);
  free_contexts(md, keyed, 2, 1);
}

// pbkdf2Sync(password, salt, iterations, keylen, digest).
TzBytes *tz_crypto_pbkdf2(const TzType *pt, const void *pv, const TzType *st, const void *sv,
                          double iterations, double keylen, const TzMd *md) {
  if (!md)
    return NULL;
  if (!check_integer(iterations, "iterations", 1, 2147483647) ||
      !check_integer(keylen, "keylen", 0, 2147483647))
    return NULL;
  if (md->xof) {
    tz_throw(TZ_ERROR, "Deriving bits failed");
    return NULL;
  }
  TzCryptoData password, salt;
  if (!tz_crypto_data(&password, pt, pv, TZ_ENC_UTF8, "password"))
    return NULL;
  if (!tz_crypto_data(&salt, st, sv, TZ_ENC_UTF8, "salt")) {
    tz_crypto_data_done(&password);
    return NULL;
  }
  TzBytes *result = NULL;
  if (keylen == 0)
    tz_throw(TZ_ERROR, "Deriving bits failed"); // as node 24 does
  else if ((result = tz_buffer_alloc((int64_t)keylen, 0)))
    tz_crypto_pbkdf2_raw(md, password.data, password.length, salt.data, salt.length, iterations,
                         result->data, (size_t)keylen);
  tz_crypto_data_done(&salt);
  tz_crypto_data_done(&password);
  return result;
}

// hkdfSync(digest, ikm, salt, info, keylen) (RFC 5869): an ArrayBuffer.
TzArrayBuffer *tz_crypto_hkdf(const TzMd *md, const TzType *kt, const void *kv, const TzType *st,
                              const void *sv, const TzType *it, const void *iv, double keylen) {
  if (!md)
    return NULL;
  if (!check_integer(keylen, "keylen", 0, 2147483647))
    return NULL;
  if (md->xof || keylen == 0) {
    tz_throw(TZ_ERROR, "Deriving bits failed"); // as node 24 does
    return NULL;
  }
  if (keylen > 255.0 * md->size) {
    tz_throw(TZ_RANGE_ERROR, "Invalid key length");
    return NULL;
  }
  TzCryptoData key, salt, info;
  if (!tz_crypto_data(&key, kt, kv, TZ_ENC_UTF8, "ikm"))
    return NULL;
  if (!tz_crypto_data(&salt, st, sv, TZ_ENC_UTF8, "salt")) {
    tz_crypto_data_done(&key);
    return NULL;
  }
  if (!tz_crypto_data(&info, it, iv, TZ_ENC_UTF8, "info")) {
    tz_crypto_data_done(&salt);
    tz_crypto_data_done(&key);
    return NULL;
  }
  TzArrayBuffer *result = NULL;
  if (info.length > 1024) {
    // The words are node's.
    tz_throw(TZ_RANGE_ERROR,
             "The value of \"info\" is out of range. It must be must not contain "
             "more than 1024 bytes. Received %zu",
             info.length);
  } else {
    result = tz_abuf_new((int64_t)keylen);
    tz_crypto_hkdf_raw(md, key.data, key.length, salt.data, salt.length, info.data, info.length,
                       result->data, (size_t)keylen);
  }
  tz_crypto_data_done(&info);
  tz_crypto_data_done(&salt);
  tz_crypto_data_done(&key);
  return result;
}

void tz_crypto_hkdf_raw(const TzMd *md, const uint8_t *key, size_t key_length, const uint8_t *salt,
                        size_t salt_length, const uint8_t *info, size_t info_length, uint8_t *out,
                        size_t length) {
  size_t size = md->size;
  uint8_t *keyed = contexts(md, 4), *work = keyed + 2 * md->ctx_size;
  uint8_t prk[64], t[64];
  // Extract: PRK = HMAC(salt, ikm); no salt is a string of zeros.
  uint8_t zeros[64] = {0};
  hmac_key(md, keyed, salt_length ? salt : zeros, salt_length ? salt_length : size);
  hmac_with(md, keyed, work, key ? key : zeros, key_length, prk);
  free_contexts(md, keyed, 2, 1);
  // Expand: T(i) = HMAC(PRK, T(i-1) | info | i).
  keyed = contexts(md, 4);
  work = keyed + 2 * md->ctx_size;
  hmac_key(md, keyed, prk, size);
  uint8_t *message = (uint8_t *)scratch(size + info_length + 1);
  size_t done = 0, previous = 0;
  for (uint32_t i = 1; done < length; ++i) {
    if (previous)
      memcpy(message, t, previous);
    if (info_length)
      memcpy(message + previous, info, info_length);
    message[previous + info_length] = (uint8_t)i;
    hmac_with(md, keyed, work, message, previous + info_length + 1, t);
    previous = size;
    size_t n = length - done < size ? length - done : size;
    memcpy(out + done, t, n);
    done += n;
  }
  free(message);
  free_contexts(md, keyed, 2, 1);
  memset(prk, 0, sizeof prk);
  memset(t, 0, sizeof t);
}

//===----------------------------------------------------------------------===//
// Random numbers
//===----------------------------------------------------------------------===//

void tz_crypto_random(uint8_t *out, size_t length) {
#ifdef _WIN32
  while (length) {
    ULONG n = length > 0x40000000 ? 0x40000000 : (ULONG)length;
    if (BCryptGenRandom(NULL, out, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
      tz_panic("the system gave no random bytes", -1);
    out += n;
    length -= n;
  }
#else
#error "random bytes are only implemented for Windows"
#endif
}

// randomBytes(size).
TzBytes *tz_crypto_random_bytes(double size) {
  if (isnan(size) || size < 0 || size > 2147483647) {
    char text[64];
    number_text(text, sizeof text, size);
    tz_throw(TZ_RANGE_ERROR,
             "The value of \"size\" is out of range. It must be >= 0 && <= "
             "2147483647. Received %s",
             text);
    return NULL;
  }
  TzBytes *b = tz_buffer_alloc((int64_t)size, 0);
  if (b)
    tz_crypto_random(b->data, (size_t)b->length);
  return b;
}

// randomFillSync(buffer, offset?, size?) (`has`: 1 offset, 2 size): fills it in place.
void tz_crypto_random_fill(TzInstance *view, double offset, double size, int32_t has) {
  uint8_t *data;
  int64_t length;
  if (tz_bytes_kind(view) == TZ_BYTES_ARRAY_BUFFER) {
    data = ((TzArrayBuffer *)view)->data;
    length = ((TzArrayBuffer *)view)->length;
  } else {
    data = ((TzBytes *)view)->data;
    length = tz_bytes_byte_length((TzBytes *)view);
  }
  double from = (has & 1) ? offset : 0;
  if (!check_integer(from, "offset", 0, (double)length))
    return;
  double count = (has & 2) ? size : (double)length - from;
  if (!check_integer(count, "size", 0, 2147483647))
    return;
  if (from + count > (double)length) {
    char total[64], text[64];
    number_text(total, sizeof total, (double)length);
    number_text(text, sizeof text, from + count);
    tz_throw(TZ_RANGE_ERROR,
             "The value of \"size + offset\" is out of range. It must be <= %s. "
             "Received %s",
             total, text);
    return;
  }
  tz_crypto_random(data + (int64_t)from, (size_t)count);
}

// getRandomValues(typedArray): an array of integers of up to 65536 bytes, filled in place.
void tz_crypto_get_random_values(TzBytes *view) {
  int64_t length = tz_bytes_byte_length(view);
  if (length > 65536) {
    tz_throw(TZ_ERROR, "The requested length exceeds 65,536 bytes");
    return;
  }
  tz_crypto_random(view->data, (size_t)length);
}

// A random number below `range` (at most 2^48), without the bias of a plain modulo, as node
// draws it: 6 random bytes, rejected above the largest multiple of the range.
static double random_below(double range) {
  const double limit = 281474976710656.0; // 2^48
  double top = limit - fmod(limit, range);
  for (;;) {
    uint8_t b[6];
    tz_crypto_random(b, sizeof b);
    double x = 0;
    for (int i = 0; i < 6; ++i)
      x = x * 256 + b[i];
    if (x < top)
      return fmod(x, range);
  }
}

// randomInt(min, max): an integer from min (inclusive) to max (exclusive).
double tz_crypto_random_int(double min, double max) {
  const double safe = 9007199254740991.0;
  char text[64];
  if (min != floor(min) || fabs(min) > safe) {
    number_text(text, sizeof text, min);
    tz_throw(TZ_TYPE_ERROR,
             "The \"min\" argument must be a safe integer. Received type number "
             "(%s)",
             text);
    return 0;
  }
  if (max != floor(max) || fabs(max) > safe) {
    number_text(text, sizeof text, max);
    tz_throw(TZ_TYPE_ERROR,
             "The \"max\" argument must be a safe integer. Received type number "
             "(%s)",
             text);
    return 0;
  }
  if (max <= min) {
    char low[64];
    number_text(low, sizeof low, min);
    number_text(text, sizeof text, max);
    tz_throw(TZ_RANGE_ERROR,
             "The value of \"max\" is out of range. It must be greater than the "
             "value of \"min\" (%s). Received %s",
             low, text);
    return 0;
  }
  double range = max - min;
  if (range > 281474976710655.0) {
    number_text(text, sizeof text, range);
    tz_throw(TZ_RANGE_ERROR,
             "The value of \"max - min\" is out of range. It must be <= "
             "281474976710655. Received %s",
             text);
    return 0;
  }
  return min + random_below(range);
}

int32_t tz_crypto_random_int32(int32_t min, int32_t max) {
  return (int32_t)tz_crypto_random_int(min, max);
}

// randomUUID(): a version 4 UUID.
void tz_crypto_random_uuid(TzStr *out) {
  uint8_t b[16];
  tz_crypto_random(b, sizeof b);
  b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);
  b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
  static const char hex[] = "0123456789abcdef";
  char text[36];
  for (int i = 0, o = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      text[o++] = '-';
    text[o++] = hex[b[i] >> 4];
    text[o++] = hex[b[i] & 15];
  }
  *out = tz_str_from_ascii(text, sizeof text);
}

//===----------------------------------------------------------------------===//
// timingSafeEqual
//===----------------------------------------------------------------------===//

int32_t tz_crypto_timing_safe_equal(const TzType *at, const void *av, const TzType *bt,
                                    const void *bv) {
  TzCryptoData a, b;
  if (!tz_crypto_data(&a, at, av, TZ_ENC_UTF8, "buf1"))
    return 0;
  if (!tz_crypto_data(&b, bt, bv, TZ_ENC_UTF8, "buf2")) {
    tz_crypto_data_done(&a);
    return 0;
  }
  int32_t equal = 0;
  if (a.length != b.length) {
    tz_throw(TZ_RANGE_ERROR, "Input buffers must have the same byte length");
  } else {
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < a.length; ++i)
      diff |= a.data[i] ^ b.data[i];
    equal = diff == 0;
  }
  tz_crypto_data_done(&b);
  tz_crypto_data_done(&a);
  return equal;
}

//===----------------------------------------------------------------------===//
// Cipheriv and Decipheriv
//===----------------------------------------------------------------------===//

static void cipher_drop(void *object);
static const TzLayout cipher_layout = TZ_CRYPTO_LAYOUT(cipher_drop, "Cipheriv");
static const TzLayout decipher_layout = TZ_CRYPTO_LAYOUT(cipher_drop, "Decipheriv");

static void cipher_drop(void *object) {
  TzCipher *c = (TzCipher *)object;
  if (c->alg->ops->cleanup)
    c->alg->ops->cleanup(c);
  memset(c->ctx, 0, c->alg->ctx_size);
  memset(c->tag, 0, sizeof c->tag);
  tz_obj_free(c);
}

static int valid_tag_length(const TzCipherAlg *alg, double length) {
  int n = (int)length;
  if (length != n)
    return 0;
  if (alg->iv_any == 1) // GCM
    return n == 4 || n == 8 || (n >= 12 && n <= 16);
  return n >= 1 && n <= 16;
}

// createCipheriv(algorithm, key, iv, { authTagLength }) and createDecipheriv: -1 for no tag
// length.
TzCipher *tz_crypto_cipher_new(const TzCipherAlg *alg, const TzType *kt, const void *kv,
                               const TzType *it, const void *iv, double tag_length,
                               int32_t decrypt) {
  if (!alg)
    return NULL;
  TzCryptoData key, nonce;
  if (!tz_crypto_data(&key, kt, kv, TZ_ENC_UTF8, "key"))
    return NULL;
  if (!tz_crypto_data(&nonce, it, iv, TZ_ENC_UTF8, "iv")) {
    tz_crypto_data_done(&key);
    return NULL;
  }
  TzCipher *c = NULL;
  int iv_ok = alg->iv_any == 1   ? nonce.length >= 1
              : alg->iv_any == 2 ? nonce.length >= 1 && nonce.length <= 15
                                 : nonce.length == alg->iv_length;
  if (key.length != alg->key_length) {
    tz_throw(TZ_RANGE_ERROR, "Invalid key length");
  } else if (!iv_ok) {
    tz_throw(TZ_TYPE_ERROR, "Invalid initialization vector");
  } else if (tag_length >= 0 && alg->kind == TZ_CIPHER_AEAD && !valid_tag_length(alg, tag_length)) {
    char text[64];
    number_text(text, sizeof text, tag_length);
    tz_throw(TZ_TYPE_ERROR, "Invalid authentication tag length: %s", text);
  } else {
    c = (TzCipher *)tz_obj_alloc((int64_t)(sizeof(TzCipher) + alg->ctx_size));
    memset(c, 0, sizeof(TzCipher) + alg->ctx_size);
    c->rc = 1;
    c->layout = decrypt ? &decipher_layout : &cipher_layout;
    c->alg = alg;
    c->decrypt = decrypt;
    c->padding = 1;
    c->tag_length = tag_length >= 0 ? (int32_t)tag_length : 16;
    if (!alg->ops->init(c, key.data, key.length, nonce.data, nonce.length)) {
      tz_obj_free(c);
      c = NULL;
    }
  }
  tz_crypto_data_done(&nonce);
  tz_crypto_data_done(&key);
  return c;
}

// The output of update() into `out` (room for length + 2 blocks).
static int64_t cipher_update(TzCipher *c, const uint8_t *in, size_t length, uint8_t *out) {
  if (c->state == 2) {
    tz_throw(TZ_ERROR, "Trying to add data in unsupported state");
    return -1;
  }
  c->state = 1;
  return c->alg->ops->update(c, in, length, out);
}

TzBytes *tz_crypto_cipher_update(TzCipher *c, const TzType *type, const void *value,
                                 int32_t encoding) {
  TzCryptoData d;
  if (!tz_crypto_data(&d, type, value, encoding, "data"))
    return NULL;
  TzBytes *b = tz_buffer_alloc((int64_t)d.length + 32, 0);
  TzBytes *result = NULL;
  if (b) {
    int64_t n = cipher_update(c, d.data, d.length, b->data);
    if (n >= 0) {
      b->length = (int32_t)n;
      result = b;
    } else {
      tz_instance_release((TzInstance *)b);
    }
  }
  tz_crypto_data_done(&d);
  return result;
}

TzBytes *tz_crypto_cipher_final(TzCipher *c) {
  if (c->state == 2) {
    tz_throw(TZ_ERROR, "Invalid state");
    return NULL;
  }
  if (c->decrypt && c->alg->kind == TZ_CIPHER_AEAD && !c->tag_set) {
    c->state = 2;
    tz_throw(TZ_ERROR, "Unsupported state or unable to authenticate data");
    return NULL;
  }
  uint8_t out[32];
  int64_t n = c->alg->ops->final(c, out);
  c->state = 2;
  return n < 0 ? NULL : tz_crypto_buffer(out, (size_t)n);
}

// setAAD(buffer, { plaintextLength }): the receiver is handed back.
void tz_crypto_cipher_set_aad(TzCipher *c, const TzType *type, const void *value,
                              double plaintext_length) {
  if (c->alg->kind != TZ_CIPHER_AEAD || c->state != 0 || !c->alg->ops->aad) {
    tz_throw(TZ_ERROR, "Invalid state for operation setAAD");
    return;
  }
  TzCryptoData d;
  if (!tz_crypto_data(&d, type, value, TZ_ENC_UTF8, "buffer"))
    return;
  if (!c->alg->ops->aad(c, d.data, d.length, plaintext_length))
    tz_throw(TZ_ERROR, "Invalid state for operation setAAD");
  tz_crypto_data_done(&d);
}

TzBytes *tz_crypto_cipher_get_auth_tag(TzCipher *c) {
  if (c->decrypt || c->alg->kind != TZ_CIPHER_AEAD || c->state != 2) {
    tz_throw(TZ_ERROR, "Invalid state for operation getAuthTag");
    return NULL;
  }
  return tz_crypto_buffer(c->tag, (size_t)c->tag_length);
}

// setAuthTag(tag, encoding) of a Decipheriv: the receiver is handed back.
void tz_crypto_cipher_set_auth_tag(TzCipher *c, const TzType *type, const void *value,
                                   int32_t encoding) {
  if (c->alg->kind != TZ_CIPHER_AEAD || c->state == 2 || c->tag_set) {
    tz_throw(TZ_ERROR, "Invalid state for operation setAuthTag");
    return;
  }
  TzCryptoData d;
  if (!tz_crypto_data(&d, type, value, encoding, "buffer"))
    return;
  int custom = c->tag_length != 16;
  if (!valid_tag_length(c->alg, (double)d.length) || (custom && d.length != (size_t)c->tag_length))
    tz_throw(TZ_TYPE_ERROR, "Invalid authentication tag length: %zu", d.length);
  else {
    memcpy(c->tag, d.data, d.length);
    c->tag_length = (int32_t)d.length;
    c->tag_set = 1;
  }
  tz_crypto_data_done(&d);
}

// setAutoPadding(autoPadding): the receiver is handed back.
void tz_crypto_cipher_set_auto_padding(TzCipher *c, int32_t padding) {
  if (c->state == 2) {
    tz_throw(TZ_ERROR, "Invalid state for operation setAutoPadding");
    return;
  }
  c->padding = padding;
}

// The update of a cipher of whole blocks with padding, as OpenSSL's providers do it
// (ossl_cipher_generic_block_update): a decipher holds back the last block, which may be padding,
// until final. `blocks` ciphers whole blocks.
int64_t tz_crypto_block_update(TzCipher *c, const uint8_t *in, size_t length, uint8_t *out,
                               TzBlocks blocks) {
  const size_t size = c->alg->block;
  int64_t written = 0;
  size_t next;
  if (c->pending) {
    size_t room = size - (size_t)c->pending;
    if (length < room)
      room = length;
    memcpy(c->block + c->pending, in, room);
    c->pending += (int32_t)room;
    in += room;
    length -= room;
    next = length & ~(size - 1);
  } else {
    next = length & ~(size - 1);
  }
  if ((size_t)c->pending == size && (!c->decrypt || length > 0 || !c->padding)) {
    blocks(c, c->block, size, out);
    c->pending = 0;
    written += (int64_t)size;
    out += size;
  }
  if (next > 0) {
    if (c->decrypt && c->padding && next == length)
      next -= size;
    blocks(c, in, next, out);
    written += (int64_t)next;
    in += next;
    length -= next;
  }
  if (length) {
    memcpy(c->block + c->pending, in, length);
    c->pending += (int32_t)length;
  }
  return written;
}

// OpenSSL's error of a final block, unless the cipher is a job's (see TzCipher::quiet).
static void block_fail(const TzCipher *c, const char *message) {
  if (!c->quiet)
    tz_throw(TZ_ERROR, "%s", message);
}

// The final block of such a cipher: the padding added, or checked and taken off.
int64_t tz_crypto_block_final(TzCipher *c, uint8_t *out, TzBlocks blocks) {
  const size_t size = c->alg->block;
  if (!c->decrypt) {
    if (!c->padding) {
      if (c->pending) {
        block_fail(c, "error:1C80006B:Provider routines::wrong final block length");
        return -1;
      }
      return 0;
    }
    uint8_t pad = (uint8_t)(size - (size_t)c->pending);
    memset(c->block + c->pending, pad, pad);
    blocks(c, c->block, size, out);
    c->pending = 0;
    return (int64_t)size;
  }
  if (!c->padding) {
    if (c->pending) {
      block_fail(c, "error:1C80006B:Provider routines::wrong final block length");
      return -1;
    }
    return 0;
  }
  if ((size_t)c->pending != size) {
    block_fail(c, "error:1C80006B:Provider routines::wrong final block length");
    return -1;
  }
  blocks(c, c->block, size, out);
  c->pending = 0;
  uint8_t pad = out[size - 1];
  int bad = pad == 0 || pad > size;
  for (size_t i = 0; !bad && i < pad; ++i)
    bad |= out[size - 1 - i] != pad;
  if (bad) {
    block_fail(c, "error:1C800064:Provider routines::bad decrypt");
    return -1;
  }
  return (int64_t)(size - pad);
}

//===----------------------------------------------------------------------===//
// Secret keys
//===----------------------------------------------------------------------===//

static void key_drop(void *object);
const TzLayout tz_crypto_secret_key_layout = TZ_CRYPTO_LAYOUT(key_drop, "SecretKeyObject");
const TzLayout tz_crypto_public_key_layout = TZ_CRYPTO_LAYOUT(key_drop, "PublicKeyObject");
const TzLayout tz_crypto_private_key_layout = TZ_CRYPTO_LAYOUT(key_drop, "PrivateKeyObject");

static void key_drop(void *object) {
  TzKey *key = (TzKey *)object;
  if (key->secret) {
    memset(key->secret, 0, (size_t)key->length);
    tz_free(key->secret);
  }
  if (key->pkey)
    key->ops->release(key->pkey);
  tz_obj_free(key);
}

// createSecretKey(key, encoding).
TzKey *tz_crypto_secret_key(const TzType *type, const void *value, int32_t encoding) {
  TzCryptoData d;
  if (!tz_crypto_data(&d, type, value, encoding, "key"))
    return NULL;
  TzKey *key = (TzKey *)tz_obj_alloc(sizeof(TzKey));
  memset(key, 0, sizeof *key);
  key->rc = 1;
  key->layout = &tz_crypto_secret_key_layout;
  key->type = TZ_KEY_SECRET;
  key->length = (int32_t)d.length;
  key->secret = (uint8_t *)tz_alloc(d.length ? d.length : 1);
  if (d.length)
    memcpy(key->secret, d.data, d.length);
  tz_crypto_data_done(&d);
  return key;
}

// generateKeySync('hmac' | 'aes', { length }): random bytes of `length` bits.
TzKey *tz_crypto_generate_secret_key(int32_t aes, double bits) {
  if (aes) {
    if (bits != 128 && bits != 192 && bits != 256) {
      char text[64];
      number_text(text, sizeof text, bits);
      tz_throw(TZ_TYPE_ERROR,
               "The property 'options.length' must be one of: 128, 192, 256. "
               "Received %s",
               text);
      return NULL;
    }
  } else if (!check_integer(bits, "options.length", 8, 2147483647)) {
    return NULL;
  }
  size_t length = (size_t)bits / 8;
  TzKey *key = (TzKey *)tz_obj_alloc(sizeof(TzKey));
  memset(key, 0, sizeof *key);
  key->rc = 1;
  key->layout = &tz_crypto_secret_key_layout;
  key->type = TZ_KEY_SECRET;
  key->length = (int32_t)length;
  key->secret = (uint8_t *)tz_alloc(length);
  tz_crypto_random(key->secret, length);
  return key;
}

// key.type.
void tz_crypto_key_type(TzStr *out, const TzKey *key) {
  const char *name = key->type == TZ_KEY_SECRET   ? "secret"
                     : key->type == TZ_KEY_PUBLIC ? "public"
                                                  : "private";
  *out = tz_str_from_ascii(name, strlen(name));
}

// key.symmetricKeySize: undefined (-1) for an asymmetric key.
int32_t tz_crypto_key_symmetric_size(const TzKey *key) {
  return key->type == TZ_KEY_SECRET ? key->length : -1;
}

static TzStr ascii(const char *text) { return tz_str_from_ascii(text, strlen(text)); }

// key.asymmetricKeyType: whether there is one (none for a secret key).
int32_t tz_crypto_key_asymmetric_type(TzStr *out, const TzKey *key) {
  if (key->type == TZ_KEY_SECRET) {
    *out = tz_str_from_ascii("", 0);
    return 0;
  }
  key->ops->asymmetric_type(out, key);
  return 1;
}

// key.export() and key.export({ format: 'buffer' }) of a secret key: its bytes.
TzBytes *tz_crypto_key_export_secret(const TzKey *key) {
  if (key->type != TZ_KEY_SECRET) {
    tz_throw(TZ_TYPE_ERROR, "The \"options.format\" property must be one of: 'pem', 'der', "
                            "'jwk'. Received 'buffer'");
    return NULL;
  }
  return tz_crypto_buffer(key->secret, (size_t)key->length);
}

// The options of export: a type ('spki', 'pkcs8'...), a cipher and its passphrase, as strings of
// the program, with `has` saying which (1 type, 2 cipher, 4 passphrase).
typedef struct {
  char type[16], cipher[64];
  TzCryptoData passphrase;
} ExportOptions;

static int export_options(ExportOptions *o, int32_t has, void *type_obj, uint64_t type_meta,
                          void *cipher_obj, uint64_t cipher_meta, const TzType *pt,
                          const void *pv) {
  o->type[0] = o->cipher[0] = 0;
  if (has & 1)
    tz_crypto_cstr(type_obj, type_meta, o->type, sizeof o->type);
  if (has & 2)
    tz_crypto_cstr(cipher_obj, cipher_meta, o->cipher, sizeof o->cipher);
  return tz_crypto_data(&o->passphrase, (has & 4) ? pt : NULL, pv, TZ_ENC_UTF8, "passphrase");
}

static int asymmetric_only(const TzKey *key, const char *format) {
  if (key->type != TZ_KEY_SECRET)
    return 1;
  tz_throw(TZ_TYPE_ERROR,
           "The property 'options.format' must be one of: undefined, 'buffer', "
           "'jwk'. Received '%s'",
           format);
  return 0;
}

// key.export({ format: 'pem', type, cipher, passphrase }).
void tz_crypto_key_export_pem(TzStr *out, const TzKey *key, int32_t has, void *type_obj,
                              uint64_t type_meta, void *cipher_obj, uint64_t cipher_meta,
                              const TzType *pt, const void *pv) {
  *out = tz_str_from_ascii("", 0);
  ExportOptions o;
  if (!asymmetric_only(key, "pem") ||
      !export_options(&o, has, type_obj, type_meta, cipher_obj, cipher_meta, pt, pv))
    return;
  key->ops->export_pem(out, key, o.type, o.cipher, o.passphrase.data, o.passphrase.length);
  tz_crypto_data_done(&o.passphrase);
}

// key.export({ format: 'der', type, cipher, passphrase }).
TzBytes *tz_crypto_key_export_der(const TzKey *key, int32_t has, void *type_obj, uint64_t type_meta,
                                  void *cipher_obj, uint64_t cipher_meta, const TzType *pt,
                                  const void *pv) {
  ExportOptions o;
  if (!asymmetric_only(key, "der") ||
      !export_options(&o, has, type_obj, type_meta, cipher_obj, cipher_meta, pt, pv))
    return NULL;
  TzBytes *b = key->ops->export_der(key, o.type, o.cipher, o.passphrase.data, o.passphrase.length);
  tz_crypto_data_done(&o.passphrase);
  return b;
}

// key.export({ format: 'jwk' }): { kty: 'oct', k } of a secret key.
TzJson *tz_crypto_key_export_jwk(const TzKey *key) {
  if (key->type != TZ_KEY_SECRET)
    return key->ops->export_jwk(key);
  TzJson *jwk = tz_json_object_new();
  TzStr kty = ascii("kty"), oct = ascii("oct"), k = ascii("k"), value;
  tz_json_object_put(jwk, kty.obj, kty.meta, tz_json_string(oct.obj, oct.meta));
  tz_crypto_text(&value, key->secret, (size_t)key->length, TZ_ENC_BASE64URL);
  tz_json_object_put(jwk, k.obj, k.meta, tz_json_string(value.obj, value.meta));
  tz_str_release_value(value);
  return jwk;
}

// a.equals(b).
int32_t tz_crypto_key_equals(const TzKey *a, const TzKey *b) {
  if (a->type != b->type)
    return 0;
  if (a->type != TZ_KEY_SECRET)
    return a->ops->equals(a, b);
  if (a->length != b->length)
    return 0;
  volatile uint8_t diff = 0;
  for (int32_t i = 0; i < a->length; ++i)
    diff |= a->secret[i] ^ b->secret[i];
  return diff == 0;
}
