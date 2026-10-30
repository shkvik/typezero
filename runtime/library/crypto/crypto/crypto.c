#include "runtime/library/crypto/crypto/crypto.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/error/error.h"
#include "runtime/core/group/group.h"
#include "runtime/core/output/output.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/bytes/buffer/buffer.h"
#include "runtime/library/bytes/bytes/bytes.h"
#include "runtime/library/json/json/json.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt")
#endif

static void number_text(char *out, size_t size, double value) {
  if (value == floor(value) && fabs(value) < 1e21) {
    char digits[32];
    snprintf(digits, sizeof digits, "%.0f", fabs(value));
    if (fabs(value) <= 4294967296.0) {
      snprintf(out, size, "%s%s", value < 0 ? "-" : "", digits);
      return;
    }
    size_t n = strlen(digits), o = 0;
    if (value < 0 && o + 1 < size) {
      out[o++] = '-';
    }
    for (size_t i = 0; i < n && o + 2 < size; ++i) {
      if (i > 0 && (n - i) % 3 == 0) {
        out[o++] = '_';
      }
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
  for (size_t i = 0; i < n; ++i) {
    out[i] = two ? (char)((const uint16_t *)(const void *)units)[i] : (char)units[i];
  }
  out[n] = 0;
  tz_str_release_value(s);
}

static const char *parameter_name(const char *name) {
  const char *dot = strrchr(name, '.');

  return dot ? dot + 1 : name;
}

static int check_integer(double value, const char *name, double min, double max) {
  if (value == floor(value) && !isinf(value) && value >= min && value <= max) {
    return 1;
  }
  char text[64], low[64], high[64];
  number_text(text, sizeof text, value);
  number_text(low, sizeof low, min);
  number_text(high, sizeof high, max);
  const char *parameter = parameter_name(name);
  tz_crypto_note_parameter(parameter, value);
  tz_crypto_fail("ERR_OUT_OF_RANGE", "%s must be a whole number from %s to %s, not %s", parameter,
                 low, high, text);

  return 0;
}

int tz_crypto_check_integer(double value, const char *name, double min, double max) {
  return check_integer(value, name, min, max);
}

TZ_THREAD_LOCAL TzMaker tz_crypto_maker;
TZ_THREAD_LOCAL TzCipher *tz_crypto_recording;

void tz_crypto_errors(TzMaker make) { tz_crypto_maker = make; }

typedef struct {
  char algorithm[64];
  int64_t length;
  int64_t expected;
  const char *got;
  const char *expected_kind;
  char parameter[48];
  double value;
  char field[48];
  const char *format;
  char openssl[256];
} Notes;

static TZ_THREAD_LOCAL Notes notes;

void tz_crypto_note_algorithm(const char *name) {
  snprintf(notes.algorithm, sizeof notes.algorithm, "%s", name ? name : "");
}

void tz_crypto_note_lengths(int64_t length, int64_t expected) {
  notes.length = length;
  notes.expected = expected;
}

void tz_crypto_note_key_types(const char *got, const char *expected) {
  notes.got = got;
  notes.expected_kind = expected;
}

void tz_crypto_note_parameter(const char *name, double value) {
  snprintf(notes.parameter, sizeof notes.parameter, "%s", name);
  notes.value = value;
}

void tz_crypto_note_field(const char *name) {
  snprintf(notes.field, sizeof notes.field, "%s", name);
}

void tz_crypto_note_format(const char *format) { notes.format = format; }

void tz_crypto_note_openssl(const char *text) {
  snprintf(notes.openssl, sizeof notes.openssl, "%s", text);
}

static TzStr text_of(const char *text) {
  TzBuf buf;
  tz_buf_init(&buf);
  tz_buf_utf8(&buf, text, strlen(text));

  return tz_buf_take(&buf);
}

void tz_crypto_fail(const char *code, const char *format, ...) {
  char text[512];
  va_list args;
  va_start(args, format);
  int len = vsnprintf(text, sizeof text, format, args);
  va_end(args);
  if (len < 0) {
    len = 0;
  }
  if ((size_t)len >= sizeof text) {
    len = (int)sizeof text - 1;
  }
  TzCipher *c = tz_crypto_recording;
  if (c) {
    if (!c->has_problem) {
      c->has_problem = 1;
      c->problem_length = notes.length;
      snprintf(c->problem_code, sizeof c->problem_code, "%s", code);
      snprintf(c->problem, sizeof c->problem, "%s", text);
    }
    memset(&notes, 0, sizeof notes);
    return;
  }
  if (!tz_crypto_maker) {
    tz_panicf("internal error: %s", text);
  }
  TzBuf buf;
  tz_buf_init(&buf);
  tz_buf_utf8(&buf, text, (size_t)len);
  TzCryptoParts parts;
  memset(&parts, 0, sizeof parts);
  parts.code_text = tz_str_from_ascii(code, strlen(code));
  parts.message = tz_buf_take(&buf);
  parts.code = code;
  parts.algorithm = text_of(notes.algorithm);
  parts.length = notes.length;
  parts.expected = notes.expected;
  parts.got = notes.got ? notes.got : "";
  parts.expected_kind = notes.expected_kind ? notes.expected_kind : "";
  parts.parameter = text_of(notes.parameter);
  parts.value = notes.value;
  parts.field = text_of(notes.field);
  parts.format = notes.format ? notes.format : "pem";
  parts.openssl = text_of(notes.openssl);
  memset(&notes, 0, sizeof notes);
  tz_set_failed(tz_crypto_maker(&parts));
}

int tz_crypto_data(TzCryptoData *out, const TzType *type, const void *value, int32_t encoding,
                   const char *what) {
  out->data = NULL;
  out->length = 0;
  out->made = NULL;
  if (!type) {
    return 1;
  }
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
      const char *got = layout == &tz_crypto_public_key_layout ? "public" : "private";
      tz_crypto_note_key_types(got, "secret");
      tz_crypto_fail("ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE",
                     "the %s must be a secret key, but this one is %s", what, got);
      return 0;
    }
  }
  tz_crypto_fail(
      "ERR_INVALID_ARG_TYPE",
      "The \"%s\" argument must be of type string or an instance of ArrayBuffer, Buffer, "
      "TypedArray, or DataView.",
      what);

  return 0;
}

int tz_crypto_looks_like_pem(const uint8_t *data, size_t length) {
  static const char start[] = "-----BEGIN";
  const size_t n = sizeof start - 1;
  for (size_t i = 0; i + n <= length; ++i) {
    if (memcmp(data + i, start, n) == 0) {
      return 1;
    }
  }
  for (size_t i = 0; i < length; ++i) {
    if (data[i] < 9 || data[i] > 126) {
      return 0;
    }
  }

  return 1;
}

void tz_crypto_data_done(TzCryptoData *d) {
  if (d->made) {
    tz_instance_release((TzInstance *)d->made);
  }
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

static void hash_drop(void *object);
static const TzLayout hash_layout = TZ_CRYPTO_LAYOUT(hash_drop, "Hash");
static const TzLayout hmac_layout = TZ_CRYPTO_LAYOUT(hash_drop, "Hmac");
const TzLayout tz_crypto_sign_layout = TZ_CRYPTO_LAYOUT(hash_drop, "Sign");
const TzLayout tz_crypto_verify_layout = TZ_CRYPTO_LAYOUT(hash_drop, "Verify");

void tz_crypto_drop(void *object) { ((TzInstance *)object)->layout->drop(object); }

void tz_crypto_md_start(const TzMd *md, void *ctx) {
  if (md->seed) {
    memcpy(ctx, md->seed, md->seed_size);
  }
  md->ops->init(ctx);
}

void tz_crypto_md_end(const TzMd *md, void *ctx) {
  if (md->ops->cleanup) {
    md->ops->cleanup(ctx);
  }
  memset(ctx, 0, md->ctx_size);
}

static void md_copy(const TzMd *md, void *to, const void *from) {
  if (md->ops->copy) {
    md->ops->copy(to, from);
  } else {
    memcpy(to, from, md->ctx_size);
  }
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
  if (!h->done) {
    for (size_t i = 0; i < contexts; ++i) {
      tz_crypto_md_end(h->md, h->ctx + i * h->md->ctx_size);
    }
  }
  tz_obj_free(h);
}

static void fixed_length(const TzMd *md, double length) {
  char text[64];
  number_text(text, sizeof text, length);
  tz_crypto_note_parameter("outputLength", length);
  tz_crypto_fail("ERR_OSSL_EVP_NOT_XOF_OR_INVALID_LENGTH",
                 "%s always makes %u bytes, so outputLength cannot be %s; only shake128 and "
                 "shake256 take another length",
                 md->name, md->size, text);
}

TzHash *tz_crypto_hash_new(const TzMd *md, double length) {
  if (!md) {
    return NULL;
  }
  if (length >= 0 && !md->xof && length != md->size) {
    fixed_length(md, length);
    return NULL;
  }
  if (length >= 0 && !check_integer(length, "options.outputLength", 0, 4294967295.0)) {
    return NULL;
  }
  TzHash *h = tz_crypto_hash_alloc(md, 0);
  if (length >= 0) {
    h->length = (int32_t)length;
  }
  tz_crypto_md_start(md, h->ctx);

  return h;
}

void tz_crypto_hash_update(TzHash *h, const TzType *type, const void *value, int32_t encoding) {
  TzCryptoData d;
  if (!tz_crypto_data(&d, type, value, encoding, "data")) {
    return;
  }
  if (d.length) {
    h->md->ops->update(h->ctx, d.data, d.length);
  }
  tz_crypto_data_done(&d);
}

static int64_t hash_finish(TzHash *h, uint8_t *out) {
  const TzMd *md = h->md;
  uint8_t *copy = (uint8_t *)tz_alloc(md->ctx_size);
  if (h->layout == &hmac_layout) {
    uint8_t inner[64];
    md_copy(md, copy, h->ctx);
    md->ops->final(copy, inner, md->size);
    tz_crypto_md_end(md, copy);
    md_copy(md, copy, h->ctx + md->ctx_size);
    md->ops->update(copy, inner, md->size);
    md->ops->final(copy, out, md->size);
    tz_crypto_md_end(md, copy);
    tz_free(copy);
    return md->size;
  }
  md_copy(md, copy, h->ctx);
  md->ops->final(copy, out, (size_t)h->length);
  tz_crypto_md_end(md, copy);
  tz_free(copy);

  return h->length;
}

TzBytes *tz_crypto_hash_digest(TzHash *h) {
  uint8_t small[64];
  uint8_t *out = h->length <= 64 ? small : (uint8_t *)tz_alloc((size_t)h->length);
  int64_t n = hash_finish(h, out);
  TzBytes *b = n < 0 ? NULL : tz_crypto_buffer(out, (size_t)n);
  if (out != small) {
    tz_free(out);
  }

  return b;
}

void tz_crypto_hash_digest_text(TzStr *out, TzHash *h, int32_t encoding) {
  uint8_t small[64];
  uint8_t *bytes = h->length <= 64 ? small : (uint8_t *)tz_alloc((size_t)h->length);
  int64_t n = hash_finish(h, bytes);
  if (n < 0) {
    *out = tz_str_from_ascii("", 0);
  } else {
    tz_crypto_text(out, bytes, (size_t)n, encoding);
  }
  if (bytes != small) {
    tz_free(bytes);
  }
}

TzHash *tz_crypto_hash_copy(TzHash *h, double length) {
  if (length >= 0 && !h->md->xof && length != h->md->size) {
    fixed_length(h->md, length);
    return NULL;
  }
  if (length >= 0 && !check_integer(length, "options.outputLength", 0, 4294967295.0)) {
    return NULL;
  }
  TzHash *copy = tz_crypto_hash_alloc(h->md, 0);
  copy->length = length >= 0 ? (int32_t)length : h->length;
  md_copy(h->md, copy->ctx, h->ctx);

  return copy;
}

static void *scratch(size_t size) {
  void *p = malloc(size ? size : 1);
  if (!p) {
    tz_panic("out of memory", -1);
  }

  return p;
}

static void digest_once(const TzMd *md, const uint8_t *data, size_t length, uint8_t *out) {
  _Alignas(16) uint8_t stack[512];
  void *ctx = md->ctx_size <= sizeof stack ? stack : scratch(md->ctx_size);
  tz_crypto_md_start(md, ctx);
  if (length) {
    md->ops->update(ctx, data, length);
  }
  md->ops->final(ctx, out, md->size);
  tz_crypto_md_end(md, ctx);
  if (ctx != stack) {
    free(ctx);
  }
}

void tz_crypto_digest_raw(const TzMd *md, const uint8_t *data, size_t length, uint8_t *out) {
  digest_once(md, data, length, out);
}

TzBytes *tz_crypto_hash_once(const TzMd *md, const TzType *type, const void *value) {
  if (!md) {
    return NULL;
  }
  TzCryptoData d;
  if (!tz_crypto_data(&d, type, value, TZ_ENC_UTF8, "data")) {
    return NULL;
  }
  uint8_t out[64];
  digest_once(md, d.data, d.length, out);
  tz_crypto_data_done(&d);

  return tz_crypto_buffer(out, md->size);
}

void tz_crypto_hash_once_text(TzStr *out, const TzMd *md, const TzType *type, const void *value,
                              int32_t encoding) {
  *out = tz_str_from_ascii("", 0);
  if (!md) {
    return;
  }
  TzCryptoData d;
  if (!tz_crypto_data(&d, type, value, TZ_ENC_UTF8, "data")) {
    return;
  }
  uint8_t bytes[64];
  digest_once(md, d.data, d.length, bytes);
  tz_crypto_data_done(&d);
  tz_crypto_text(out, bytes, md->size, encoding);
}

static void hmac_key(const TzMd *md, uint8_t *ctx, const uint8_t *key, size_t length) {
  uint8_t block[256], pad[256];
  memset(block, 0, md->block);
  if (length > md->block) {
    digest_once(md, key, length, block);
  } else if (length) {
    memcpy(block, key, length);
  }
  for (int k = 0; k < 2; ++k) {
    uint8_t *c = ctx + (size_t)k * md->ctx_size;
    for (uint32_t i = 0; i < md->block; ++i) {
      pad[i] = block[i] ^ (k == 0 ? 0x36 : 0x5c);
    }
    tz_crypto_md_start(md, c);
    md->ops->update(c, pad, md->block);
  }
  memset(block, 0, sizeof block);
  memset(pad, 0, sizeof pad);
}

static void fixed_hash_only(const TzMd *md, const char *fn) {
  tz_crypto_note_algorithm(md->name);
  tz_crypto_fail("ERR_CRYPTO_INVALID_DIGEST",
                 "%s cannot use %s: it has no fixed length; use a hash such as sha256", fn,
                 md->name);
}

static int hmac_takes(const TzMd *md) {
  if (md->xof || md->block == 0 || md->block > 256) {
    fixed_hash_only(md, "createHmac");
    return 0;
  }

  return 1;
}

TzHash *tz_crypto_hmac_new(const TzMd *md, const TzType *type, const void *value,
                           int32_t encoding) {
  if (!md || !hmac_takes(md)) {
    return NULL;
  }
  TzCryptoData key;
  if (!tz_crypto_data(&key, type, value, encoding, "key")) {
    return NULL;
  }
  TzHash *h = tz_crypto_hash_alloc(md, 1);
  hmac_key(md, h->ctx, key.data, key.length);
  tz_crypto_data_done(&key);

  return h;
}

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
  if (keyed && md->ops->cleanup) {
    for (size_t i = 0; i < count; ++i) {
      md->ops->cleanup(ctx + i * md->ctx_size);
    }
  }
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

void tz_crypto_pbkdf2_raw(const TzMd *md, const uint8_t *password, size_t password_length,
                          const uint8_t *salt, size_t salt_length, double iterations, uint8_t *out,
                          size_t length) {
  uint8_t *keyed = contexts(md, 4), *work = keyed + 2 * md->ctx_size;
  hmac_key(md, keyed, password, password_length);
  uint8_t *salted = (uint8_t *)scratch(salt_length + 4);
  if (salt_length) {
    memcpy(salted, salt, salt_length);
  }
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
      for (size_t k = 0; k < size; ++k) {
        t[k] ^= u[k];
      }
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

TzBytes *tz_crypto_pbkdf2(const TzType *pt, const void *pv, const TzType *st, const void *sv,
                          double iterations, double keylen, const TzMd *md) {
  if (!md) {
    return NULL;
  }
  if (!check_integer(iterations, "iterations", 1, 2147483647) ||
      !check_integer(keylen, "keylen", 1, 2147483647)) {
    return NULL;
  }
  if (md->xof) {
    fixed_hash_only(md, "pbkdf2");
    return NULL;
  }
  TzCryptoData password, salt;
  if (!tz_crypto_data(&password, pt, pv, TZ_ENC_UTF8, "password")) {
    return NULL;
  }
  if (!tz_crypto_data(&salt, st, sv, TZ_ENC_UTF8, "salt")) {
    tz_crypto_data_done(&password);
    return NULL;
  }
  TzBytes *result = tz_buffer_alloc((int64_t)keylen, 0);
  if (result) {
    tz_crypto_pbkdf2_raw(md, password.data, password.length, salt.data, salt.length, iterations,
                         result->data, (size_t)keylen);
  }
  tz_crypto_data_done(&salt);
  tz_crypto_data_done(&password);

  return result;
}

TzArrayBuffer *tz_crypto_hkdf(const TzMd *md, const TzType *kt, const void *kv, const TzType *st,
                              const void *sv, const TzType *it, const void *iv, double keylen) {
  if (!md) {
    return NULL;
  }
  if (!check_integer(keylen, "keylen", 1, 2147483647)) {
    return NULL;
  }
  if (md->xof) {
    fixed_hash_only(md, "hkdf");
    return NULL;
  }
  if (keylen > 255.0 * md->size) {
    char text[64];
    number_text(text, sizeof text, keylen);
    tz_crypto_note_parameter("keylen", keylen);
    tz_crypto_fail("ERR_CRYPTO_INVALID_KEYLEN", "hkdf with %s makes at most %u bytes, not %s",
                   md->name, 255 * md->size, text);
    return NULL;
  }
  TzCryptoData key, salt, info;
  if (!tz_crypto_data(&key, kt, kv, TZ_ENC_UTF8, "ikm")) {
    return NULL;
  }
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
    tz_crypto_note_parameter("info", (double)info.length);
    tz_crypto_fail("ERR_OUT_OF_RANGE", "info must be at most 1024 bytes, not %zu", info.length);
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
  uint8_t zeros[64] = {0};
  hmac_key(md, keyed, salt_length ? salt : zeros, salt_length ? salt_length : size);
  hmac_with(md, keyed, work, key ? key : zeros, key_length, prk);
  free_contexts(md, keyed, 2, 1);
  keyed = contexts(md, 4);
  work = keyed + 2 * md->ctx_size;
  hmac_key(md, keyed, prk, size);
  uint8_t *message = (uint8_t *)scratch(size + info_length + 1);
  size_t done = 0, previous = 0;
  for (uint32_t i = 1; done < length; ++i) {
    if (previous) {
      memcpy(message, t, previous);
    }
    if (info_length) {
      memcpy(message + previous, info, info_length);
    }
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

void tz_crypto_random(uint8_t *out, size_t length) {
#ifdef _WIN32
  while (length) {
    ULONG n = length > 0x40000000 ? 0x40000000 : (ULONG)length;
    if (BCryptGenRandom(NULL, out, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
      tz_panic("the system gave no random bytes", -1);
    }
    out += n;
    length -= n;
  }
#else
#error "random bytes are only implemented for Windows"
#endif
}

TzBytes *tz_crypto_random_bytes(double size) {
  if (isnan(size) || size < 0 || size > 2147483647) {
    char text[64];
    number_text(text, sizeof text, size);
    tz_crypto_fail("ERR_OUT_OF_RANGE",
                   "The value of \"size\" is out of range. It must be >= 0 && <= "
                   "2147483647. Received %s",
                   text);
    return NULL;
  }
  TzBytes *b = tz_buffer_alloc((int64_t)size, 0);
  if (b) {
    tz_crypto_random(b->data, (size_t)b->length);
  }

  return b;
}

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
  if (!check_integer(from, "offset", 0, (double)length)) {
    return;
  }
  double count = (has & 2) ? size : (double)length - from;
  if (!check_integer(count, "size", 0, 2147483647)) {
    return;
  }
  if (from + count > (double)length) {
    char total[64], text[64];
    number_text(total, sizeof total, (double)length);
    number_text(text, sizeof text, from + count);
    tz_crypto_fail("ERR_OUT_OF_RANGE",
                   "The value of \"size + offset\" is out of range. It must be <= %s. "
                   "Received %s",
                   total, text);
    return;
  }
  tz_crypto_random(data + (int64_t)from, (size_t)count);
}

void tz_crypto_get_random_values(TzBytes *view) {
  tz_crypto_random(view->data, (size_t)tz_bytes_byte_length(view));
}

static double random_below(double range) {
  const double limit = 281474976710656.0;
  double top = limit - fmod(limit, range);
  for (;;) {
    uint8_t b[6];
    tz_crypto_random(b, sizeof b);
    double x = 0;
    for (int i = 0; i < 6; ++i) {
      x = x * 256 + b[i];
    }
    if (x < top) {
      return fmod(x, range);
    }
  }
}

double tz_crypto_random_int(double min, double max) {
  const double safe = 9007199254740991.0;
  char text[64];
  if (min != floor(min) || fabs(min) > safe) {
    number_text(text, sizeof text, min);
    tz_crypto_fail("ERR_INVALID_ARG_TYPE",
                   "The \"min\" argument must be a safe integer. Received type number "
                   "(%s)",
                   text);
    return 0;
  }
  if (max != floor(max) || fabs(max) > safe) {
    number_text(text, sizeof text, max);
    tz_crypto_fail("ERR_INVALID_ARG_TYPE",
                   "The \"max\" argument must be a safe integer. Received type number "
                   "(%s)",
                   text);
    return 0;
  }
  if (max <= min) {
    char low[64];
    number_text(low, sizeof low, min);
    number_text(text, sizeof text, max);
    tz_crypto_fail("ERR_OUT_OF_RANGE",
                   "The value of \"max\" is out of range. It must be greater than the "
                   "value of \"min\" (%s). Received %s",
                   low, text);
    return 0;
  }
  double range = max - min;
  if (range > 281474976710655.0) {
    number_text(text, sizeof text, range);
    tz_crypto_fail("ERR_OUT_OF_RANGE",
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

void tz_crypto_random_uuid(TzStr *out) {
  uint8_t b[16];
  tz_crypto_random(b, sizeof b);
  b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);
  b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
  static const char hex[] = "0123456789abcdef";
  char text[36];
  for (int i = 0, o = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      text[o++] = '-';
    }
    text[o++] = hex[b[i] >> 4];
    text[o++] = hex[b[i] & 15];
  }
  *out = tz_str_from_ascii(text, sizeof text);
}

int32_t tz_crypto_timing_safe_equal(const TzType *at, const void *av, const TzType *bt,
                                    const void *bv) {
  TzCryptoData a, b;
  if (!tz_crypto_data(&a, at, av, TZ_ENC_UTF8, "buf1")) {
    return 0;
  }
  if (!tz_crypto_data(&b, bt, bv, TZ_ENC_UTF8, "buf2")) {
    tz_crypto_data_done(&a);
    return 0;
  }
  int32_t equal = 0;
  if (a.length == b.length) {
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < a.length; ++i) {
      diff |= a.data[i] ^ b.data[i];
    }
    equal = diff == 0;
  }
  tz_crypto_data_done(&b);
  tz_crypto_data_done(&a);

  return equal;
}

static void cipher_drop(void *object);
static const TzLayout cipher_layout = TZ_CRYPTO_LAYOUT(cipher_drop, "Cipheriv");
static const TzLayout decipher_layout = TZ_CRYPTO_LAYOUT(cipher_drop, "Decipheriv");

static void cipher_drop(void *object) {
  TzCipher *c = (TzCipher *)object;
  if (c->alg->ops->cleanup) {
    c->alg->ops->cleanup(c);
  }
  memset(c->ctx, 0, c->alg->ctx_size);
  memset(c->tag, 0, sizeof c->tag);
  tz_obj_free(c);
}

static int valid_tag_length(const TzCipherAlg *alg, double length) {
  int n = (int)length;
  if (length != n) {
    return 0;
  }
  if (alg->iv_any == 1) {
    return n == 4 || n == 8 || (n >= 12 && n <= 16);
  }

  return n >= 1 && n <= 16;
}

static const char *tag_lengths(const TzCipherAlg *alg) {
  return alg->iv_any == 1 ? "4, 8 or 12 to 16" : "1 to 16";
}

static const char *cipher_noun(const TzCipher *c) { return c->decrypt ? "decipher" : "cipher"; }

static const char *cipher_maker(const TzCipher *c) {
  return c->decrypt ? "createDecipheriv" : "createCipheriv";
}

static void wrong_order(const TzCipher *c, const char *format) {
  char text[160];
  snprintf(text, sizeof text, format, cipher_noun(c));
  tz_crypto_note_algorithm(c->alg->name);
  tz_crypto_fail("ERR_CRYPTO_INVALID_STATE", "%s; make a new one with %s", text, cipher_maker(c));
}

void tz_crypto_auth_failed(const TzCipher *c) {
  tz_crypto_note_algorithm(c->alg->name);
  tz_crypto_fail("ERR_CRYPTO_AUTH_FAILED",
                 "could not decrypt with %s: the data was changed or the key is wrong (the "
                 "authentication tag does not match)",
                 c->alg->name);
}

TzCipher *tz_crypto_cipher_new(const TzCipherAlg *alg, const TzType *kt, const void *kv,
                               const TzType *it, const void *iv, double tag_length,
                               int32_t decrypt) {
  if (!alg) {
    return NULL;
  }
  TzCryptoData key, nonce;
  if (!tz_crypto_data(&key, kt, kv, TZ_ENC_UTF8, "key")) {
    return NULL;
  }
  if (!tz_crypto_data(&nonce, it, iv, TZ_ENC_UTF8, "iv")) {
    tz_crypto_data_done(&key);
    return NULL;
  }
  TzCipher *c = NULL;
  int iv_ok = alg->iv_any == 1   ? nonce.length >= 1
              : alg->iv_any == 2 ? nonce.length >= 1 && nonce.length <= 15
                                 : nonce.length == alg->iv_length;
  tz_crypto_note_algorithm(alg->name);
  if (key.length != alg->key_length) {
    tz_crypto_note_lengths((int64_t)key.length, alg->key_length);
    tz_crypto_fail("ERR_CRYPTO_INVALID_KEYLEN", "%s needs a %u-byte key, but the key has %zu bytes",
                   alg->name, alg->key_length, key.length);
  } else if (!iv_ok) {
    tz_crypto_note_lengths((int64_t)nonce.length, alg->iv_length);
    if (alg->iv_any == 1) {
      tz_crypto_fail("ERR_CRYPTO_INVALID_IV", "%s needs an IV of 1 byte or more, but it is empty",
                     alg->name);
    } else if (alg->iv_any == 2) {
      tz_crypto_fail("ERR_CRYPTO_INVALID_IV",
                     "%s needs an IV of 1 to 15 bytes, but it has %zu bytes", alg->name,
                     nonce.length);
    } else if (alg->iv_length == 0) {
      tz_crypto_fail("ERR_CRYPTO_INVALID_IV", "%s takes no IV, but it has %zu bytes", alg->name,
                     nonce.length);
    } else {
      tz_crypto_fail("ERR_CRYPTO_INVALID_IV", "%s needs a %u-byte IV, but it has %zu bytes",
                     alg->name, alg->iv_length, nonce.length);
    }
  } else if (tag_length >= 0 && alg->kind == TZ_CIPHER_AEAD && !valid_tag_length(alg, tag_length)) {
    char text[64];
    number_text(text, sizeof text, tag_length);
    tz_crypto_note_lengths((int64_t)tag_length, 0);
    tz_crypto_fail("ERR_CRYPTO_INVALID_AUTH_TAG",
                   "%s takes an authentication tag of %s bytes, not %s", alg->name,
                   tag_lengths(alg), text);
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

static int64_t cipher_update(TzCipher *c, const uint8_t *in, size_t length, uint8_t *out) {
  if (c->state == 2) {
    wrong_order(c, "update() was called after final() on this %s");
    return -1;
  }
  c->state = 1;
  int64_t written = c->alg->ops->update(c, in, length, out);
  if (written >= 0) {
    c->total += (int64_t)length;
  }

  return written;
}

static TzCipher *record_problems(TzCipher *c) {
  TzCipher *outer = tz_crypto_recording;
  tz_crypto_recording = c;

  return outer;
}

TzBytes *tz_crypto_cipher_update(TzCipher *c, const TzType *type, const void *value,
                                 int32_t encoding) {
  TzCryptoData d;
  if (!tz_crypto_data(&d, type, value, encoding, "data")) {
    return NULL;
  }
  TzBytes *b = tz_buffer_alloc((int64_t)d.length + 32, 0);
  if (b) {
    TzCipher *outer = record_problems(c);
    int64_t n = cipher_update(c, d.data, d.length, b->data);
    tz_crypto_recording = outer;
    b->length = n >= 0 ? (int32_t)n : 0;
  }
  tz_crypto_data_done(&d);

  return b;
}

TzBytes *tz_crypto_cipher_final(TzCipher *c) {
  if (c->has_problem && c->state != 2) {
    c->state = 2;
    tz_crypto_note_algorithm(c->alg->name);
    tz_crypto_note_lengths(c->problem_length, 0);
    tz_crypto_fail(c->problem_code, "%s", c->problem);
    return NULL;
  }
  if (c->state == 2) {
    wrong_order(c, "final() was already called on this %s");
    return NULL;
  }
  if (c->decrypt && c->alg->kind == TZ_CIPHER_AEAD && !c->tag_set) {
    c->state = 2;
    tz_crypto_note_algorithm(c->alg->name);
    tz_crypto_fail("ERR_CRYPTO_INVALID_STATE",
                   "%s needs the authentication tag: call setAuthTag() before final()",
                   c->alg->name);
    return NULL;
  }
  uint8_t out[32];
  int64_t n = c->alg->ops->final(c, out);
  c->state = 2;

  return n < 0 ? NULL : tz_crypto_buffer(out, (size_t)n);
}

static void set_aad(TzCipher *c, const TzType *type, const void *value, double plaintext_length);

void tz_crypto_cipher_set_aad(TzCipher *c, const TzType *type, const void *value,
                              double plaintext_length) {
  TzCipher *outer = record_problems(c);
  set_aad(c, type, value, plaintext_length);
  tz_crypto_recording = outer;
}

static void no_aead(const TzCipher *c, const char *method) {
  tz_crypto_note_algorithm(c->alg->name);
  tz_crypto_fail("ERR_CRYPTO_INVALID_STATE",
                 "%s works only with GCM, CCM, OCB and chacha20-poly1305, not with %s", method,
                 c->alg->name);
}

static void set_aad(TzCipher *c, const TzType *type, const void *value, double plaintext_length) {
  if (c->alg->kind != TZ_CIPHER_AEAD || !c->alg->ops->aad) {
    no_aead(c, "setAAD()");
    return;
  }
  if (c->state != 0) {
    wrong_order(c, "setAAD() was called after update() or final() on this %s");
    return;
  }
  TzCryptoData d;
  if (!tz_crypto_data(&d, type, value, TZ_ENC_UTF8, "buffer")) {
    return;
  }
  if (!c->alg->ops->aad(c, d.data, d.length, plaintext_length) && !c->has_problem) {
    tz_crypto_note_algorithm(c->alg->name);
    tz_crypto_fail("ERR_CRYPTO_OPERATION_FAILED", "could not add the AAD to %s", c->alg->name);
  }
  tz_crypto_data_done(&d);
}

TzBytes *tz_crypto_cipher_get_auth_tag(TzCipher *c) {
  if (c->decrypt || c->alg->kind != TZ_CIPHER_AEAD) {
    no_aead(c, "getAuthTag()");
    return NULL;
  }
  if (c->state != 2) {
    tz_crypto_note_algorithm(c->alg->name);
    tz_crypto_fail("ERR_CRYPTO_INVALID_STATE",
                   "getAuthTag() works only after final(); call final() first");
    return NULL;
  }

  return tz_crypto_buffer(c->tag, (size_t)c->tag_length);
}

static void set_auth_tag(TzCipher *c, const TzType *type, const void *value, int32_t encoding);

void tz_crypto_cipher_set_auth_tag(TzCipher *c, const TzType *type, const void *value,
                                   int32_t encoding) {
  TzCipher *outer = record_problems(c);
  set_auth_tag(c, type, value, encoding);
  tz_crypto_recording = outer;
}

static void set_auth_tag(TzCipher *c, const TzType *type, const void *value, int32_t encoding) {
  if (c->alg->kind != TZ_CIPHER_AEAD) {
    no_aead(c, "setAuthTag()");
    return;
  }
  if (c->state == 2) {
    wrong_order(c, "setAuthTag() was called after final() on this %s");
    return;
  }
  if (c->tag_set) {
    wrong_order(c, "setAuthTag() was already called on this %s");
    return;
  }
  TzCryptoData d;
  if (!tz_crypto_data(&d, type, value, encoding, "buffer")) {
    return;
  }
  int custom = c->tag_length != 16;
  if (!valid_tag_length(c->alg, (double)d.length) ||
      (custom && d.length != (size_t)c->tag_length)) {
    tz_crypto_note_lengths((int64_t)d.length, 0);
    if (custom) {
      tz_crypto_fail("ERR_CRYPTO_INVALID_AUTH_TAG",
                     "this decipher was made with authTagLength %d, but the tag has %zu bytes",
                     c->tag_length, d.length);
    } else {
      tz_crypto_fail("ERR_CRYPTO_INVALID_AUTH_TAG",
                     "%s takes an authentication tag of %s bytes, not %zu", c->alg->name,
                     tag_lengths(c->alg), d.length);
    }
  } else {
    memcpy(c->tag, d.data, d.length);
    c->tag_length = (int32_t)d.length;
    c->tag_set = 1;
  }
  tz_crypto_data_done(&d);
}

void tz_crypto_cipher_set_auto_padding(TzCipher *c, int32_t padding) {
  if (c->state == 2) {
    TzCipher *outer = record_problems(c);
    wrong_order(c, "setAutoPadding() was called after final() on this %s");
    tz_crypto_recording = outer;
    return;
  }
  c->padding = padding;
}

int64_t tz_crypto_block_update(TzCipher *c, const uint8_t *in, size_t length, uint8_t *out,
                               TzBlocks blocks) {
  const size_t size = c->alg->block;
  int64_t written = 0;
  size_t next;
  if (c->pending) {
    size_t room = size - (size_t)c->pending;
    if (length < room) {
      room = length;
    }
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
    if (c->decrypt && c->padding && next == length) {
      next -= size;
    }
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

void tz_crypto_block_length_failed(const TzCipher *c) {
  if (c->quiet) {
    return;
  }
  tz_crypto_note_algorithm(c->alg->name);
  tz_crypto_note_lengths(c->total, c->alg->block);
  tz_crypto_fail("ERR_OSSL_WRONG_FINAL_BLOCK_LENGTH",
                 "could not %s with %s: %lld bytes is not a multiple of %u%s",
                 c->decrypt ? "decrypt" : "encrypt", c->alg->name, (long long)c->total,
                 c->alg->block, c->padding ? "" : " (padding is off)");
}

void tz_crypto_bad_decrypt(const TzCipher *c) {
  if (c->quiet) {
    return;
  }
  tz_crypto_note_algorithm(c->alg->name);
  tz_crypto_fail("ERR_OSSL_BAD_DECRYPT",
                 "could not decrypt with %s: the key is wrong or the data is damaged",
                 c->alg->name);
}

int64_t tz_crypto_block_final(TzCipher *c, uint8_t *out, TzBlocks blocks) {
  const size_t size = c->alg->block;
  if (!c->decrypt) {
    if (!c->padding) {
      if (c->pending) {
        tz_crypto_block_length_failed(c);
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
      tz_crypto_block_length_failed(c);
      return -1;
    }
    return 0;
  }
  if ((size_t)c->pending != size) {
    tz_crypto_block_length_failed(c);
    return -1;
  }
  blocks(c, c->block, size, out);
  c->pending = 0;
  uint8_t pad = out[size - 1];
  int bad = pad == 0 || pad > size;
  for (size_t i = 0; !bad && i < pad; ++i) {
    bad |= out[size - 1 - i] != pad;
  }
  if (bad) {
    tz_crypto_bad_decrypt(c);
    return -1;
  }

  return (int64_t)(size - pad);
}

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
  if (key->pkey) {
    key->ops->release(key->pkey);
  }
  tz_obj_free(key);
}

TzKey *tz_crypto_secret_key(const TzType *type, const void *value, int32_t encoding) {
  TzCryptoData d;
  if (!tz_crypto_data(&d, type, value, encoding, "key")) {
    return NULL;
  }
  TzKey *key = (TzKey *)tz_obj_alloc(sizeof(TzKey));
  memset(key, 0, sizeof *key);
  key->rc = 1;
  key->layout = &tz_crypto_secret_key_layout;
  key->type = TZ_KEY_SECRET;
  key->length = (int32_t)d.length;
  key->secret = (uint8_t *)tz_alloc(d.length ? d.length : 1);
  if (d.length) {
    memcpy(key->secret, d.data, d.length);
  }
  tz_crypto_data_done(&d);

  return key;
}

TzKey *tz_crypto_generate_secret_key(int32_t aes, double bits) {
  if (aes) {
    if (bits != 128 && bits != 192 && bits != 256) {
      char text[64];
      number_text(text, sizeof text, bits);
      tz_crypto_note_parameter("length", bits);
      tz_crypto_fail("ERR_OUT_OF_RANGE", "length must be 128, 192 or 256, not %s", text);
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

void tz_crypto_key_type(TzStr *out, const TzKey *key) {
  const char *name = key->type == TZ_KEY_SECRET   ? "secret"
                     : key->type == TZ_KEY_PUBLIC ? "public"
                                                  : "private";
  *out = tz_str_from_ascii(name, strlen(name));
}

int32_t tz_crypto_key_symmetric_size(const TzKey *key) {
  return key->type == TZ_KEY_SECRET ? key->length : -1;
}

static TzStr ascii(const char *text) { return tz_str_from_ascii(text, strlen(text)); }

int32_t tz_crypto_key_asymmetric_type(TzStr *out, const TzKey *key) {
  if (key->type == TZ_KEY_SECRET) {
    *out = tz_str_from_ascii("", 0);
    return 0;
  }
  key->ops->asymmetric_type(out, key);

  return 1;
}

TzBytes *tz_crypto_key_export_secret(const TzKey *key) {
  if (key->type != TZ_KEY_SECRET) {
    tz_crypto_fail("ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS",
                   "a %s key is exported with options: { type, format }",
                   key->type == TZ_KEY_PUBLIC ? "public" : "private");
    return NULL;
  }

  return tz_crypto_buffer(key->secret, (size_t)key->length);
}

typedef struct {
  char type[16], cipher[64];
  TzCryptoData passphrase;
} ExportOptions;

static int export_options(ExportOptions *o, int32_t has, void *type_obj, uint64_t type_meta,
                          void *cipher_obj, uint64_t cipher_meta, const TzType *pt,
                          const void *pv) {
  o->type[0] = o->cipher[0] = 0;
  if (has & 1) {
    tz_crypto_cstr(type_obj, type_meta, o->type, sizeof o->type);
  }
  if (has & 2) {
    tz_crypto_cstr(cipher_obj, cipher_meta, o->cipher, sizeof o->cipher);
  }

  return tz_crypto_data(&o->passphrase, (has & 4) ? pt : NULL, pv, TZ_ENC_UTF8, "passphrase");
}

static int asymmetric_only(const TzKey *key, const char *format) {
  if (key->type != TZ_KEY_SECRET) {
    return 1;
  }
  tz_crypto_fail("ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS",
                 "a secret key cannot be exported as %s; use export() or { format: \"jwk\" }",
                 format);

  return 0;
}

void tz_crypto_key_export_pem(TzStr *out, const TzKey *key, int32_t has, void *type_obj,
                              uint64_t type_meta, void *cipher_obj, uint64_t cipher_meta,
                              const TzType *pt, const void *pv) {
  *out = tz_str_from_ascii("", 0);
  ExportOptions o;
  if (!asymmetric_only(key, "pem") ||
      !export_options(&o, has, type_obj, type_meta, cipher_obj, cipher_meta, pt, pv)) {
    return;
  }
  key->ops->export_pem(out, key, o.type, o.cipher, o.passphrase.data, o.passphrase.length);
  tz_crypto_data_done(&o.passphrase);
}

TzBytes *tz_crypto_key_export_der(const TzKey *key, int32_t has, void *type_obj, uint64_t type_meta,
                                  void *cipher_obj, uint64_t cipher_meta, const TzType *pt,
                                  const void *pv) {
  ExportOptions o;
  if (!asymmetric_only(key, "der") ||
      !export_options(&o, has, type_obj, type_meta, cipher_obj, cipher_meta, pt, pv)) {
    return NULL;
  }
  TzBytes *b = key->ops->export_der(key, o.type, o.cipher, o.passphrase.data, o.passphrase.length);
  tz_crypto_data_done(&o.passphrase);

  return b;
}

TzJson *tz_crypto_key_export_jwk(const TzKey *key) {
  if (key->type != TZ_KEY_SECRET) {
    return key->ops->export_jwk(key);
  }
  TzJson *jwk = tz_json_object_new();
  TzStr kty = ascii("kty"), oct = ascii("oct"), k = ascii("k"), value;
  tz_json_object_put(jwk, kty.obj, kty.meta, tz_json_string(oct.obj, oct.meta));
  tz_crypto_text(&value, key->secret, (size_t)key->length, TZ_ENC_BASE64URL);
  tz_json_object_put(jwk, k.obj, k.meta, tz_json_string(value.obj, value.meta));
  tz_str_release_value(value);

  return jwk;
}

int32_t tz_crypto_key_equals(const TzKey *a, const TzKey *b) {
  if (a->type != b->type) {
    return 0;
  }
  if (a->type != TZ_KEY_SECRET) {
    return a->ops->equals(a, b);
  }
  if (a->length != b->length) {
    return 0;
  }
  volatile uint8_t diff = 0;
  for (int32_t i = 0; i < a->length; ++i) {
    diff |= a->secret[i] ^ b->secret[i];
  }

  return diff == 0;
}
