// Buffer as in node, and the text encodings (docs/bytes.md, sections 2 and
// 5): Buffer.from(string, encoding) and toString(encoding) with utf8,
// utf16le, latin1, ascii, base64, base64url and hex, the methods of node
// that work on bytes, and TextEncoder / TextDecoder of the WHATWG Encoding
// standard. A Buffer is a TzBytes of the kind TZ_BYTES_BUFFER (bytes.c).
//
// Where node's results depend on its algorithms (what base64 makes of text
// that is not base64, where hex stops), the algorithms are node's (those of
// its deps/nbytes).

#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/bytes/encodings/encodings.h"
#include <stdio.h>
#include <string.h>

TzBytes *tz_bytes_new_unset(int32_t kind, int64_t length);
TzBytes *tz_bytes_new(int32_t kind, int64_t length);
TzBytes *tz_bytes_slice(TzBytes *b, int64_t start, int64_t end, int32_t has);
extern const TzLayout tz_bytes_layouts[];

#define MAX_LENGTH ((int64_t)(1 << 30) - 1)

// Buffer's encodings, as Sema numbers them
// (compiler/library/bytes/sema/sema_bytes.cpp).
enum { ENC_UTF8, ENC_UTF16LE, ENC_LATIN1, ENC_ASCII, ENC_BASE64, ENC_BASE64URL, ENC_HEX };

// The code units of a string, which stays where it is while they are used.
typedef struct {
  TzStr s;
  const void *units;
  uint32_t length;
  int two;
} Units;

static void units_of(Units *u, void *obj, uint64_t meta) {
  u->s.obj = obj;
  u->s.meta = meta;
  u->units = tz_str_units(&u->s, &u->length, &u->two);
}

static inline uint32_t unit_at(const Units *u, uint32_t i) {
  return u->two ? ((const uint16_t *)u->units)[i] : ((const uint8_t *)u->units)[i];
}

//===----------------------------------------------------------------------===//
// Encodings by name
//===----------------------------------------------------------------------===//

static int lower_equal(const Units *u, const char *name) {
  size_t n = strlen(name);
  if (u->length != n) {
    return 0;
  }
  for (size_t i = 0; i < n; ++i) {
    uint32_t c = unit_at(u, (uint32_t)i);
    if (c >= 'A' && c <= 'Z') {
      c += 'a' - 'A';
    }
    if (c != (unsigned char)name[i]) {
      return 0;
    }
  }

  return 1;
}

// The encoding a name stands for, as Buffer.isEncoding reads it; -1 for none.
int32_t tz_buffer_encoding(void *obj, uint64_t meta) {
  static const struct {
    const char *name;
    int32_t encoding;
  } names[] = {
      {"utf8", ENC_UTF8},        {"utf-8", ENC_UTF8},          {"utf16le", ENC_UTF16LE},
      {"utf-16le", ENC_UTF16LE}, {"ucs2", ENC_UTF16LE},        {"ucs-2", ENC_UTF16LE},
      {"latin1", ENC_LATIN1},    {"binary", ENC_LATIN1},       {"ascii", ENC_ASCII},
      {"base64", ENC_BASE64},    {"base64url", ENC_BASE64URL}, {"hex", ENC_HEX},
  };
  Units u;
  units_of(&u, obj, meta);
  for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
    if (lower_equal(&u, names[i].name)) {
      return names[i].encoding;
    }
  }

  return -1;
}

// The same for an encoding the program gives: an unknown one is a TypeError,
// as in node.
int32_t tz_buffer_encoding_checked(void *obj, uint64_t meta) {
  int32_t encoding = tz_buffer_encoding(obj, meta);
  if (encoding < 0) {
    TzStr s = {obj, meta};
    TzBuf text;
    tz_buf_init(&text);
    tz_buf_ascii(&text, "Unknown encoding: ");
    tz_buf_str(&text, s);
    TzStr message = tz_buf_take(&text);
    tz_throw_str(TZ_TYPE_ERROR, message.obj, message.meta);
    tz_str_release_value(message);
  }

  return encoding;
}

//===----------------------------------------------------------------------===//
// From text
//===----------------------------------------------------------------------===//

// The code point at i of UTF-16, and how many units it takes; a lone
// surrogate is U+FFFD, as encoding it in UTF-8 makes it.
static uint32_t code_point(const Units *u, uint32_t i, uint32_t *taken) {
  uint32_t c = unit_at(u, i);
  *taken = 1;
  if (c >= 0xD800 && c <= 0xDBFF && i + 1 < u->length) {
    uint32_t d = unit_at(u, i + 1);
    if (d >= 0xDC00 && d <= 0xDFFF) {
      *taken = 2;
      return 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
    }
  }
  if (c >= 0xD800 && c <= 0xDFFF) {
    return 0xFFFD;
  }

  return c;
}

static int utf8_size(uint32_t cp) { return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4; }

static uint8_t *put_utf8(uint8_t *out, uint32_t cp) {
  if (cp < 0x80) {
    *out++ = (uint8_t)cp;
  } else if (cp < 0x800) {
    *out++ = (uint8_t)(0xC0 | (cp >> 6));
    *out++ = (uint8_t)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    *out++ = (uint8_t)(0xE0 | (cp >> 12));
    *out++ = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    *out++ = (uint8_t)(0x80 | (cp & 0x3F));
  } else {
    *out++ = (uint8_t)(0xF0 | (cp >> 18));
    *out++ = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    *out++ = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    *out++ = (uint8_t)(0x80 | (cp & 0x3F));
  }

  return out;
}

static int64_t utf8_length(const Units *u) {
  if (!u->two) {
    int64_t n = u->length;
    for (uint32_t i = 0; i < u->length; ++i) {
      n += ((const uint8_t *)u->units)[i] >= 0x80;
    }
    return n;
  }
  int64_t n = 0;
  for (uint32_t i = 0; i < u->length;) {
    uint32_t taken;
    n += utf8_size(code_point(u, i, &taken));
    i += taken;
  }

  return n;
}

// The values of the base64 digits of both alphabets, as node reads them,
// and of the hex digits, each plus 1: 0 is what is no digit.
#define D(c, v) [c] = (v) + 1
#define DIGITS4(c, v) D(c, v), D(c + 1, v + 1), D(c + 2, v + 2), D(c + 3, v + 3)
#define DIGITS8(c, v) DIGITS4(c, v), DIGITS4(c + 4, v + 4)
static const uint8_t kBase64Digits[256] = {
    DIGITS8('A', 0),  DIGITS8('I', 8),  DIGITS8('Q', 16), D('Y', 24), D('Z', 25),
    DIGITS8('a', 26), DIGITS8('i', 34), DIGITS8('q', 42), D('y', 50), D('z', 51),
    DIGITS8('0', 52), D('8', 60),       D('9', 61),       D('+', 62), D('-', 62),
    D('/', 63),       D('_', 63)};
static const uint8_t kHexDigits[256] = {DIGITS8('0', 0),  D('8', 8),  D('9', 9),
                                        DIGITS4('a', 10), D('e', 14), D('f', 15),
                                        DIGITS4('A', 10), D('E', 14), D('F', 15)};
#undef DIGITS8
#undef DIGITS4
#undef D

// A digit's value; 255 (and -1) for what is no digit.
static uint8_t unbase64(uint32_t c) { return c < 256 ? (uint8_t)(kBase64Digits[c] - 1) : 255; }

static int unhex(uint32_t c) { return c < 256 ? kHexDigits[c] - 1 : -1; }

// What Buffer.byteLength(string, encoding) gives: for base64, node's
// estimate from the length, which decoding may not reach.
static int64_t byte_length(const Units *u, int32_t encoding) {
  int64_t n = u->length;
  switch (encoding) {
  case ENC_UTF8:
    return utf8_length(u);
  case ENC_UTF16LE:
    return n * 2;
  case ENC_BASE64:
  case ENC_BASE64URL:
    if (n > 0 && unit_at(u, (uint32_t)n - 1) == '=') {
      --n;
    }
    if (n > 1 && unit_at(u, (uint32_t)n - 1) == '=') {
      --n;
    }
    return (n * 3) >> 2;
  case ENC_HEX:
    return n >> 1;
  default:
    return n;
  }
}

int32_t tz_buffer_byte_length(void *obj, uint64_t meta, int32_t encoding) {
  Units u;
  units_of(&u, obj, meta);

  return (int32_t)byte_length(&u, encoding);
}

// nbytes::Base64DecodeGroupSlow: one group of four digits, skipping what is
// not a digit; false when the text ends or a '=' stops it.
static int base64_group(uint8_t *dst, size_t dstlen, const Units *u, size_t *i, size_t *k) {
  size_t n = u->length;
  uint8_t hi = 0, lo = 0;
  for (int step = 0; step < 4; ++step) {
    for (;;) {
      uint32_t c = unit_at(u, (uint32_t)*i);
      lo = unbase64(c);
      *i += 1;
      if (lo < 64) {
        break;
      }
      if (c == '=' || *i >= n) {
        return 0;
      }
    }
    if (step == 1) {
      dst[(*k)++] = (uint8_t)(((hi & 0x3F) << 2) | ((lo & 0x30) >> 4));
    } else if (step == 2) {
      dst[(*k)++] = (uint8_t)(((hi & 0x0F) << 4) | ((lo & 0x3C) >> 2));
    } else if (step == 3) {
      dst[(*k)++] = (uint8_t)(((hi & 0x03) << 6) | (lo & 0x3F));
    }
    if (*i >= n) {
      return 0;
    }
    if (*k >= dstlen) {
      return 0;
    }
    hi = lo;
  }

  return 1;
}

// nbytes::Base64Decode: what node decodes, both alphabets, into at most
// dstlen bytes.
static size_t base64_decode(uint8_t *dst, size_t dstlen, const Units *u) {
  size_t n = u->length;
  size_t size = n;
  if (size >= 2 && unit_at(u, (uint32_t)size - 1) == '=') {
    --size;
    if (unit_at(u, (uint32_t)size - 1) == '=') {
      --size;
    }
  }
  size_t decoded = n < 2 ? 0 : size > 1 ? (size / 4) * 3 + (size % 4 + 1) / 2 : 0;
  size_t available = dstlen < decoded ? dstlen : decoded;
  size_t max_k = available / 3 * 3, max_i = n / 4 * 4, i = 0, k = 0;
  // Groups of four digits of one byte each, the common case, straight from
  // the bytes of the string.
  if (!u->two) {
    const uint8_t *s = (const uint8_t *)u->units;
    while (i < max_i && k < max_k) {
      uint32_t a = kBase64Digits[s[i]], b = kBase64Digits[s[i + 1]];
      uint32_t c = kBase64Digits[s[i + 2]], d = kBase64Digits[s[i + 3]];
      if (!a || !b || !c || !d) {
        break;
      }
      uint32_t v = (a - 1) << 18 | (b - 1) << 12 | (c - 1) << 6 | (d - 1);
      dst[k] = (uint8_t)(v >> 16);
      dst[k + 1] = (uint8_t)(v >> 8);
      dst[k + 2] = (uint8_t)v;
      i += 4;
      k += 3;
    }
  }
  while (i < max_i && k < max_k) {
    uint8_t d[4];
    int slow = 0;
    for (int j = 0; j < 4; ++j) {
      d[j] = unbase64(unit_at(u, (uint32_t)(i + (size_t)j)));
      slow |= d[j] >= 64;
    }
    if (slow) {
      if (!base64_group(dst, dstlen, u, &i, &k)) {
        return k;
      }
      max_i = i + (n - i) / 4 * 4;
    } else {
      dst[k + 0] = (uint8_t)((d[0] << 2) | (d[1] >> 4));
      dst[k + 1] = (uint8_t)((d[1] << 4) | (d[2] >> 2));
      dst[k + 2] = (uint8_t)((d[2] << 6) | d[3]);
      i += 4;
      k += 3;
    }
  }
  if (i < n && k < dstlen) {
    base64_group(dst, dstlen, u, &i, &k);
  }

  return k;
}

// Writes the string in `encoding` into at most `capacity` bytes at `out`;
// returns how many it wrote. UTF-8 writes no part of a character that does
// not fit, as node's write() does.
static int64_t encode_into(uint8_t *out, int64_t capacity, const Units *u, int32_t encoding) {
  uint32_t n = u->length;
  switch (encoding) {
  case ENC_UTF8: {
    int64_t written = 0;
    if (!u->two) { // Latin-1: one byte, or two from 0x80 on
      const uint8_t *s = (const uint8_t *)u->units;
      for (uint32_t i = 0; i < n; ++i) {
        uint8_t b = s[i];
        if (b < 0x80) {
          if (written + 1 > capacity) {
            break;
          }
          out[written++] = b;
        } else {
          if (written + 2 > capacity) {
            break;
          }
          out[written++] = (uint8_t)(0xC0 | (b >> 6));
          out[written++] = (uint8_t)(0x80 | (b & 0x3F));
        }
      }
      return written;
    }
    for (uint32_t i = 0; i < n;) {
      uint32_t taken;
      uint32_t cp = code_point(u, i, &taken);
      int size = utf8_size(cp);
      if (written + size > capacity) {
        break;
      }
      put_utf8(out + written, cp);
      written += size;
      i += taken;
    }
    return written;
  }
  case ENC_UTF16LE: {
    int64_t count = (int64_t)n * 2 < capacity ? (int64_t)n : capacity / 2;
    for (int64_t i = 0; i < count; ++i) {
      uint32_t c = unit_at(u, (uint32_t)i);
      out[2 * i] = (uint8_t)c;
      out[2 * i + 1] = (uint8_t)(c >> 8);
    }
    return count * 2;
  }
  case ENC_BASE64:
  case ENC_BASE64URL:
    return (int64_t)base64_decode(out, (size_t)capacity, u);
  case ENC_HEX: {
    int64_t count = (int64_t)(n / 2) < capacity ? (int64_t)(n / 2) : capacity;
    if (!u->two) {
      const uint8_t *s = (const uint8_t *)u->units;
      for (int64_t i = 0; i < count; ++i) {
        int a = kHexDigits[s[2 * i]], b = kHexDigits[s[2 * i + 1]];
        if (!a || !b) {
          return i;
        }
        out[i] = (uint8_t)((a - 1) << 4 | (b - 1));
      }
      return count;
    }
    for (int64_t i = 0; i < count; ++i) {
      int a = unhex(unit_at(u, (uint32_t)(2 * i))), b = unhex(unit_at(u, (uint32_t)(2 * i + 1)));
      if (a < 0 || b < 0) {
        return i;
      }
      out[i] = (uint8_t)((a << 4) | b);
    }
    return count;
  }
  default: { // latin1 and ascii: the low byte of each unit
    int64_t count = (int64_t)n < capacity ? (int64_t)n : capacity;
    if (!u->two) {
      memcpy(out, u->units, (size_t)count);
    } else {
      for (int64_t i = 0; i < count; ++i) {
        out[i] = (uint8_t)unit_at(u, (uint32_t)i);
      }
    }
    return count;
  }
  }
}

static TzBytes *buffer_of(int64_t length) {
  if (length > MAX_LENGTH) {
    tz_panicf("a Buffer of %lld bytes exceeds the maximum of %lld", (long long)length,
              (long long)MAX_LENGTH);
  }

  return tz_bytes_new_unset(TZ_BYTES_BUFFER, length);
}

TzBytes *tz_buffer_from_string(void *obj, uint64_t meta, int32_t encoding) {
  Units u;
  units_of(&u, obj, meta);
  int64_t size = byte_length(&u, encoding);
  TzBytes *b = buffer_of(size);
  int64_t written = encode_into(b->data, size, &u, encoding);
  // byteLength may overestimate for base64 and hex, as in node.
  b->length = (int32_t)written;

  return b;
}

TzBytes *tz_buffer_copy_of(const uint8_t *data, int64_t length) {
  TzBytes *b = buffer_of(length);
  if (length) {
    memcpy(b->data, data, (size_t)length);
  }

  return b;
}

//===----------------------------------------------------------------------===//
// To text
//===----------------------------------------------------------------------===//

// The WHATWG UTF-8 decoder, which V8 and TextDecoder follow: each byte that
// does not belong to a character becomes U+FFFD, as does a sequence cut
// short. Returns 0 at the first such byte when `fatal`.
static int decode_utf8(TzBuf *out, const uint8_t *s, int64_t n, int fatal) {
  int64_t i = 0;
  while (i < n) {
    uint32_t b = s[i];
    if (b < 0x80) {
      tz_buf_push(out, (uint16_t)b);
      ++i;
      continue;
    }
    uint32_t cp, need;
    uint32_t lower = 0x80, upper = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) {
      need = 1;
      cp = b & 0x1F;
    } else if (b >= 0xE0 && b <= 0xEF) {
      if (b == 0xE0) {
        lower = 0xA0;
      }
      if (b == 0xED) {
        upper = 0x9F;
      }
      need = 2;
      cp = b & 0xF;
    } else if (b >= 0xF0 && b <= 0xF4) {
      if (b == 0xF0) {
        lower = 0x90;
      }
      if (b == 0xF4) {
        upper = 0x8F;
      }
      need = 3;
      cp = b & 0x7;
    } else {
      if (fatal) {
        return 0;
      }
      tz_buf_push(out, 0xFFFD);
      ++i;
      continue;
    }
    ++i;
    uint32_t seen = 0;
    while (seen < need) {
      if (i >= n || s[i] < lower || s[i] > upper) {
        break;
      }
      cp = (cp << 6) | (s[i] & 0x3F);
      lower = 0x80;
      upper = 0xBF;
      ++i;
      ++seen;
    }
    if (seen < need) {
      // The byte that broke the sequence starts over.
      if (fatal) {
        return 0;
      }
      tz_buf_push(out, 0xFFFD);
      continue;
    }
    if (cp >= 0x10000) {
      cp -= 0x10000;
      tz_buf_push(out, (uint16_t)(0xD800 + (cp >> 10)));
      tz_buf_push(out, (uint16_t)(0xDC00 + (cp & 0x3FF)));
    } else {
      tz_buf_push(out, (uint16_t)cp);
    }
  }

  return 1;
}

static TzStr utf8_string(const uint8_t *s, int64_t n) {
  int64_t ascii = 0;
  while (ascii < n && s[ascii] < 0x80) {
    ++ascii;
  }
  if (ascii == n) {
    return tz_str_from_ascii((const char *)s, (size_t)n);
  }
  TzBuf out;
  tz_buf_init(&out);
  decode_utf8(&out, s, n, 0);

  return tz_buf_take(&out);
}

static TzStr encode_text(const uint8_t *s, int64_t n, int32_t encoding) {
  switch (encoding) {
  case ENC_UTF8:
    return utf8_string(s, n);
  case ENC_LATIN1:
    return tz_str_from_ascii((const char *)s, (size_t)n);
  case ENC_ASCII: {
    char *text = (char *)tz_alloc((size_t)n + 1);
    for (int64_t i = 0; i < n; ++i) {
      text[i] = (char)(s[i] & 0x7F);
    }
    TzStr str = tz_str_from_ascii(text, (size_t)n);
    tz_free(text);
    return str;
  }
  case ENC_UTF16LE: {
    int64_t count = n / 2;
    uint16_t *units = (uint16_t *)tz_alloc((size_t)count * 2 + 2);
    for (int64_t i = 0; i < count; ++i) {
      units[i] = (uint16_t)(s[2 * i] | (s[2 * i + 1] << 8));
    }
    TzStr str = tz_str_from_utf16(units, (size_t)count);
    tz_free(units);
    return str;
  }
  case ENC_HEX: {
    static const char digits[] = "0123456789abcdef";
    TzStr str;
    char small[16];
    char *text = tz_str_ascii_units((size_t)n * 2, &str);
    if (!text) {
      text = small;
    }
    for (int64_t i = 0; i < n; ++i) {
      text[2 * i] = digits[s[i] >> 4];
      text[2 * i + 1] = digits[s[i] & 15];
    }
    if (text == small) {
      str = tz_str_from_ascii(small, (size_t)n * 2);
    }
    return str;
  }
  default: { // base64, base64url
    int url = encoding == ENC_BASE64URL;
    const char *digits = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                             : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t length = (size_t)n / 3 * 4;
    if (n % 3) {
      length += url ? (size_t)(n % 3) + 1 : 4;
    }
    TzStr str;
    char small[16];
    char *text = tz_str_ascii_units(length, &str);
    if (!text) {
      text = small;
    }
    size_t k = 0;
    int64_t i = 0;
    for (; i + 2 < n; i += 3) {
      uint32_t v = (uint32_t)s[i] << 16 | (uint32_t)s[i + 1] << 8 | s[i + 2];
      text[k++] = digits[v >> 18];
      text[k++] = digits[(v >> 12) & 63];
      text[k++] = digits[(v >> 6) & 63];
      text[k++] = digits[v & 63];
    }
    if (i < n) {
      uint32_t v = (uint32_t)s[i] << 16 | (i + 1 < n ? (uint32_t)s[i + 1] << 8 : 0);
      text[k++] = digits[v >> 18];
      text[k++] = digits[(v >> 12) & 63];
      if (i + 1 < n) {
        text[k++] = digits[(v >> 6) & 63];
      } else if (!url) {
        text[k++] = '=';
      }
      if (!url) {
        text[k++] = '=';
      }
    }
    if (text == small) {
      str = tz_str_from_ascii(small, k);
    }
    return str;
  }
  }
}

TzStr tz_buffer_decode(const uint8_t *data, int64_t length, int32_t encoding) {
  if (length == 0) {
    return tz_str_from_ascii("", 0);
  }

  return encode_text(data, length, encoding);
}

// buf.toString(encoding, start?, end?): positions clamped to the bytes, as
// node does (`has`: 1 start, 2 end).
void tz_buffer_to_string(TzStr *out, const TzBytes *b, int32_t encoding, int64_t start, int64_t end,
                         int32_t has) {
  int64_t n = b->length;
  int64_t from = (has & 1) ? (start < 0 ? 0 : start) : 0;
  int64_t to = (has & 2) ? (end > n ? n : end) : n;
  if (from >= to) {
    *out = tz_str_from_ascii("", 0);
    return;
  }
  *out = encode_text(b->data + from, to - from, encoding);
}

//===----------------------------------------------------------------------===//
// Methods of node
//===----------------------------------------------------------------------===//

TzBytes *tz_buffer_alloc(int64_t size, int32_t zero) {
  if (size < 0 || size > MAX_LENGTH) {
    tz_throw(TZ_RANGE_ERROR,
             "The value of \"size\" is out of range. It must be >= 0 && <= %lld. Received %lld",
             (long long)MAX_LENGTH, (long long)size);
    return NULL;
  }

  return zero ? tz_bytes_new(TZ_BYTES_BUFFER, size) : buffer_of(size);
}

// Buffer.from(arrayBuffer, offset?, length?): the same memory (`has`: 1
// offset, 2 length); the buffer is borrowed.
TzBytes *tz_buffer_view(TzArrayBuffer *buffer, int64_t offset, int64_t length, int32_t has) {
  if (!(has & 1)) {
    offset = 0;
  }
  if (offset < 0) {
    tz_throw(TZ_RANGE_ERROR, "Start offset %lld is outside the bounds of the buffer",
             (long long)offset);
    return NULL;
  }
  if (offset > buffer->length) {
    tz_throw(TZ_RANGE_ERROR, "\"offset\" is outside of buffer bounds");
    return NULL;
  }
  int64_t max = buffer->length - offset;
  if (!(has & 2)) {
    length = max;
  } else if (length > max) {
    tz_throw(TZ_RANGE_ERROR, "\"length\" is outside of buffer bounds");
    return NULL;
  } else if (length < 0) {
    length = 0;
  }
  TzBytes *b = (TzBytes *)tz_obj_alloc(sizeof(TzBytes));
  b->rc = 1;
  b->layout = &tz_bytes_layouts[TZ_BYTES_BUFFER];
  b->data = buffer->data + offset;
  b->length = (int32_t)length;
  b->offset = (int32_t)offset;
  b->buffer = buffer;
  tz_rc_inc(&buffer->rc);

  return b;
}

// Buffer.concat(list, totalLength?): the bytes of the Uint8Arrays one after
// another, cut or padded with zeros to the total length.
TzBytes *tz_buffer_concat(const TzArray *list, int64_t total, int32_t has_total) {
  TzBytes *const *items = (TzBytes *const *)list->data;
  if (!has_total) {
    total = 0;
    for (uint32_t i = 0; i < list->length; ++i) {
      total += items[i]->length;
    }
  }
  TzBytes *b = tz_buffer_alloc(total, 0);
  if (!b) {
    return NULL;
  }
  int64_t at = 0;
  for (uint32_t i = 0; i < list->length && at < total; ++i) {
    int64_t n = items[i]->length;
    if (n > total - at) {
      n = total - at;
    }
    memcpy(b->data + at, items[i]->data, (size_t)n);
    at += n;
  }
  if (at < total) {
    memset(b->data + at, 0, (size_t)(total - at));
  }

  return b;
}

static int compare_memory(const uint8_t *a, int64_t an, const uint8_t *b, int64_t bn) {
  int64_t n = an < bn ? an : bn;
  int c = n ? memcmp(a, b, (size_t)n) : 0;
  if (c != 0) {
    return c < 0 ? -1 : 1;
  }

  return an < bn ? -1 : an > bn ? 1 : 0;
}

int32_t tz_bytes_compare(const TzBytes *a, const TzBytes *b) {
  return compare_memory(a->data, a->length, b->data, b->length);
}

int32_t tz_bytes_equal(const TzBytes *a, const TzBytes *b) {
  return a->length == b->length &&
         (a->length == 0 || memcmp(a->data, b->data, (size_t)a->length) == 0);
}

TZ_COLD static void out_of_range(const char *name, int64_t value, int64_t max) {
  if (max < 0) {
    tz_throw(TZ_RANGE_ERROR, "The value of \"%s\" is out of range. It must be >= 0. Received %lld",
             name, (long long)value);
  } else {
    tz_throw(TZ_RANGE_ERROR,
             "The value of \"%s\" is out of range. It must be >= 0 && <= %lld. Received %lld", name,
             (long long)max, (long long)value);
  }
}

// buf.compare(target, targetStart?, targetEnd?, sourceStart?, sourceEnd?);
// `has` holds a bit for each, from 1 on.
int32_t tz_buffer_compare_range(const TzBytes *source, const TzBytes *target, int64_t tstart,
                                int64_t tend, int64_t sstart, int64_t send, int32_t has) {
  if (!(has & 1)) {
    tstart = 0;
  }
  if (!(has & 2)) {
    tend = target->length;
  }
  if (!(has & 4)) {
    sstart = 0;
  }
  if (!(has & 8)) {
    send = source->length;
  }
  if (tstart < 0) {
    out_of_range("targetStart", tstart, -1);
    return 0;
  }
  if (tend > target->length || tend < 0) {
    out_of_range("targetEnd", tend, target->length);
    return 0;
  }
  if (sstart < 0) {
    out_of_range("sourceStart", sstart, -1);
    return 0;
  }
  if (send > source->length || send < 0) {
    out_of_range("sourceEnd", send, source->length);
    return 0;
  }
  if (sstart >= send) {
    return tstart >= tend ? 0 : -1;
  }
  if (tstart >= tend) {
    return 1;
  }

  return compare_memory(source->data + sstart, send - sstart, target->data + tstart, tend - tstart);
}

// buf.copy(target, targetStart?, sourceStart?, sourceEnd?): returns how many
// bytes it copied (`has`: 1, 2 and 4).
int32_t tz_buffer_copy(const TzBytes *source, TzBytes *target, int64_t tstart, int64_t sstart,
                       int64_t send, int32_t has) {
  if (!(has & 1)) {
    tstart = 0;
  }
  if (!(has & 2)) {
    sstart = 0;
  }
  if (!(has & 4)) {
    send = source->length;
  }
  if (tstart < 0) {
    out_of_range("targetStart", tstart, -1);
    return 0;
  }
  if (sstart < 0 || sstart > source->length) {
    out_of_range("sourceStart", sstart, source->length);
    return 0;
  }
  if (send < 0) {
    out_of_range("sourceEnd", send, -1);
    return 0;
  }
  if (send > source->length) {
    send = source->length;
  }
  if (tstart >= target->length || sstart >= send) {
    return 0;
  }
  int64_t n = send - sstart;
  if (n > target->length - tstart) {
    n = target->length - tstart;
  }
  memmove(target->data + tstart, source->data + sstart, (size_t)n);

  return (int32_t)n;
}

// buf.write(string, offset?, length?, encoding): returns the bytes written.
int32_t tz_buffer_write_string(TzBytes *b, void *obj, uint64_t meta, int64_t offset, int64_t length,
                               int32_t has, int32_t encoding) {
  if (!(has & 1)) {
    offset = 0;
  }
  if (offset < 0 || offset > b->length) {
    out_of_range("offset", offset, b->length);
    return 0;
  }
  int64_t room = b->length - offset;
  if (has & 2) {
    if (length < 0 || length > room) {
      out_of_range("length", length, room);
      return 0;
    }
    room = length;
  }
  Units u;
  units_of(&u, obj, meta);

  return (int32_t)encode_into(b->data + offset, room, &u, encoding);
}

// The bytes a value to look for or to fill with stands for: a string in an
// encoding (in a block to free), or the bytes of a Uint8Array.
static uint8_t *bytes_of_string(void *obj, uint64_t meta, int32_t encoding, int64_t *n) {
  Units u;
  units_of(&u, obj, meta);
  int64_t size = byte_length(&u, encoding);
  uint8_t *bytes = (uint8_t *)tz_alloc((size_t)size + 1);
  *n = encode_into(bytes, size, &u, encoding);

  return bytes;
}

// node's indexOf: from byteOffset on (from the end if negative); with mode
// 1, lastIndexOf backward from it; with 2, includes.
static int32_t find_bytes(const TzBytes *b, const uint8_t *needle, int64_t n, int64_t from,
                          int32_t has_from, int32_t mode) {
  int64_t length = b->length;
  if (mode == 1) {
    int64_t start = has_from ? (from < 0 ? from + length : from) : length;
    if (start > length - n) {
      start = length - n;
    }
    for (int64_t i = start; i >= 0; --i) {
      if (n == 0 || memcmp(b->data + i, needle, (size_t)n) == 0) {
        return (int32_t)i;
      }
    }
    return -1;
  }
  int64_t start = has_from ? (from < 0 ? (from + length < 0 ? 0 : from + length) : from) : 0;
  if (n == 0) {
    return mode == 2 ? 1 : (int32_t)(start < length ? start : length);
  }
  for (int64_t i = start; i + n <= length; ++i) {
    if (b->data[i] == needle[0] && memcmp(b->data + i, needle, (size_t)n) == 0) {
      return mode == 2 ? 1 : (int32_t)i;
    }
  }

  return mode == 2 ? 0 : -1;
}

int32_t tz_buffer_index_of_string(const TzBytes *b, void *obj, uint64_t meta, int64_t from,
                                  int32_t has_from, int32_t encoding, int32_t mode) {
  int64_t n;
  uint8_t *needle = bytes_of_string(obj, meta, encoding, &n);
  int32_t found = find_bytes(b, needle, n, from, has_from, mode);
  tz_free(needle);

  return found;
}

int32_t tz_buffer_index_of_bytes(const TzBytes *b, const TzBytes *needle, int64_t from,
                                 int32_t has_from, int32_t mode) {
  return find_bytes(b, needle->data, needle->length, from, has_from, mode);
}

int32_t tz_buffer_index_of_byte(const TzBytes *b, int32_t value, int64_t from, int32_t has_from,
                                int32_t mode) {
  uint8_t byte = (uint8_t)value;

  return find_bytes(b, &byte, 1, from, has_from, mode);
}

// buf.fill(value, offset?, end?, encoding?) with a string or a Uint8Array:
// its bytes over and over. An empty value fills with zeros; a string that
// gives no bytes in its encoding is a TypeError, as in node.
static void fill_with(TzBytes *b, const uint8_t *bytes, int64_t n, int64_t offset, int64_t end,
                      int32_t has) {
  int64_t from = (has & 1) ? offset : 0, to = (has & 2) ? end : b->length;
  if (from < 0 || from > b->length) {
    out_of_range("offset", from, b->length);
    return;
  }
  if (to < 0 || to > b->length) {
    out_of_range("end", to, b->length);
    return;
  }
  for (int64_t i = from, k = 0; i < to; ++i, k = k + 1 == n ? 0 : k + 1) {
    b->data[i] = n ? bytes[k] : 0;
  }
}

void tz_buffer_fill_string(TzBytes *b, void *obj, uint64_t meta, int64_t offset, int64_t end,
                           int32_t has, int32_t encoding) {
  TzStr s = {obj, meta};
  int64_t n;
  uint8_t *bytes = bytes_of_string(obj, meta, encoding, &n);
  uint32_t length;
  int two;
  tz_str_units(&s, &length, &two);
  if (n == 0 && length > 0) {
    tz_free(bytes);
    TzBuf text;
    tz_buf_init(&text);
    tz_buf_ascii(&text, "The argument 'value' is invalid. Received '");
    tz_buf_str(&text, s);
    tz_buf_push(&text, '\'');
    TzStr message = tz_buf_take(&text);
    tz_throw_str(TZ_TYPE_ERROR, message.obj, message.meta);
    tz_str_release_value(message);
    return;
  }
  fill_with(b, bytes, n, offset, end, has);
  tz_free(bytes);
}

void tz_buffer_fill_bytes(TzBytes *b, const TzBytes *value, int64_t offset, int64_t end,
                          int32_t has) {
  // A copy first: the value may lie in the bytes being filled.
  uint8_t *bytes = (uint8_t *)tz_alloc((size_t)value->length + 1);
  if (value->length) {
    memcpy(bytes, value->data, (size_t)value->length);
  }
  fill_with(b, bytes, value->length, offset, end, has);
  tz_free(bytes);
}

void tz_buffer_fill_byte(TzBytes *b, int32_t value, int64_t offset, int64_t end, int32_t has) {
  uint8_t byte = (uint8_t)value;
  fill_with(b, &byte, 1, offset, end, has);
}

// The error of read…() and write…() at `offset` for `size` bytes, as node
// words it (its boundsError).
TZ_COLD void tz_buffer_bounds_error(int64_t offset, int64_t size, int64_t length) {
  if (length - size < 0) {
    tz_throw(TZ_RANGE_ERROR, "Attempt to access memory outside buffer bounds");
  } else {
    tz_throw(TZ_RANGE_ERROR,
             "The value of \"offset\" is out of range. It must be >= 0 and <= %lld. Received %lld",
             (long long)(length - size), (long long)offset);
  }
}

static int check_var(const TzBytes *b, int64_t offset, int64_t size) {
  if (size < 1 || size > 6) {
    tz_throw(TZ_RANGE_ERROR,
             "The value of \"byteLength\" is out of range. It must be >= 1 and <= 6. Received %lld",
             (long long)size);
    return 0;
  }
  if (offset < 0 || offset + size > b->length) {
    tz_buffer_bounds_error(offset, size, b->length);
    return 0;
  }

  return 1;
}

// readUIntLE(offset, byteLength) and the like: `flags` 1 signed, 2 big-endian.
int64_t tz_buffer_read_var(const TzBytes *b, int64_t offset, int64_t size, int32_t flags) {
  if (!check_var(b, offset, size)) {
    return 0;
  }
  uint64_t v = 0;
  for (int64_t i = 0; i < size; ++i) {
    int64_t at = (flags & 2) ? offset + i : offset + size - 1 - i;
    v = (v << 8) | b->data[at];
  }
  if ((flags & 1) && (v >> (size * 8 - 1)) & 1) {
    v |= ~(uint64_t)0 << (size * 8);
  }

  return (int64_t)v;
}

int64_t tz_buffer_write_var(TzBytes *b, int64_t value, int64_t offset, int64_t size,
                            int32_t flags) {
  if (!check_var(b, offset, size)) {
    return 0;
  }
  uint64_t v = (uint64_t)value;
  for (int64_t i = 0; i < size; ++i) {
    int64_t at = (flags & 2) ? offset + size - 1 - i : offset + i;
    b->data[at] = (uint8_t)v;
    v >>= 8;
  }

  return offset + size;
}

// swap16(), swap32() and swap64(): the bytes of each element reversed.
void tz_buffer_swap(TzBytes *b, int32_t size) {
  if (b->length % size != 0) {
    tz_throw(TZ_RANGE_ERROR, "Buffer size must be a multiple of %d-bits", size * 8);
    return;
  }
  for (int64_t i = 0; i < b->length; i += size) {
    for (int32_t j = 0; j < size / 2; ++j) {
      uint8_t t = b->data[i + j];
      b->data[i + j] = b->data[i + size - 1 - j];
      b->data[i + size - 1 - j] = t;
    }
  }
}

//===----------------------------------------------------------------------===//
// TextEncoder and TextDecoder
//===----------------------------------------------------------------------===//

void *tz_text_encoder_new(void) {
  TzInstance *e = (TzInstance *)tz_obj_alloc(sizeof(TzInstance));
  e->rc = 1;
  e->layout = &tz_bytes_layouts[TZ_BYTES_TEXT_ENCODER];

  return e;
}

// encode(s): the UTF-8 of the string, a lone surrogate as U+FFFD.
TzBytes *tz_text_encode(void *obj, uint64_t meta) {
  Units u;
  units_of(&u, obj, meta);
  int64_t size = utf8_length(&u);
  TzBytes *b = tz_bytes_new_unset(TZ_BYTES_U8, size);
  encode_into(b->data, size, &u, ENC_UTF8);

  return b;
}

// encodeInto(s, dst): as much of the string as fits, whole characters; the
// code units read and the bytes written go to out[0] and out[1].
void tz_text_encode_into(void *obj, uint64_t meta, TzBytes *dst, int32_t *out) {
  Units u;
  units_of(&u, obj, meta);
  int64_t written = 0;
  uint32_t i = 0;
  while (i < u.length) {
    uint32_t taken;
    uint32_t cp = code_point(&u, i, &taken);
    int size = utf8_size(cp);
    if (written + size > dst->length) {
      break;
    }
    put_utf8(dst->data + written, cp);
    written += size;
    i += taken;
  }
  out[0] = (int32_t)i;
  out[1] = (int32_t)written;
}

// The encodings of a TextDecoder; a single-byte one has its table (see
// encodings.h), a multi-byte one its TzCjkEncoding (cjk.c), which the
// program holds.
enum { TEXT_UTF8, TEXT_UTF16LE, TEXT_UTF16BE, TEXT_SINGLE_BYTE, TEXT_CJK };

typedef struct {
  int64_t rc;
  const TzLayout *layout;
  int32_t encoding;
  int32_t fatal, ignore_bom;
  // The last decode had `stream: true`: the stream goes on from `state`,
  // and `bom_seen` says whether its first character, which may be a BOM, is
  // behind.
  int32_t streaming;
  const void *table;
  uint32_t state[4];
  int32_t bom_seen;
} TzTextDecoder;

const char *tz_text_decoder_name(const void *decoder) {
  const TzTextDecoder *d = (const TzTextDecoder *)decoder;
  switch (d->encoding) {
  case TEXT_UTF8:
    return "utf-8";
  case TEXT_UTF16LE:
    return "utf-16le";
  case TEXT_UTF16BE:
    return "utf-16be";
  case TEXT_CJK:
    return ((const TzCjkEncoding *)d->table)->name;
  default:
    return ((const TzEncodingTable *)d->table)->name;
  }
}

int32_t tz_text_decoder_flag(const void *decoder, int32_t which) {
  const TzTextDecoder *d = (const TzTextDecoder *)decoder;

  return which == 0 ? d->fatal : d->ignore_bom;
}

void tz_text_decoder_encoding(TzStr *out, const void *decoder) {
  const char *name = tz_text_decoder_name(decoder);
  *out = tz_str_from_ascii(name, strlen(name));
}

// A decoder of an encoding Sema found by its label: TEXT_* and, for an old
// encoding, its table.
void *tz_text_decoder_make(int32_t encoding, const void *table, int32_t fatal, int32_t ignore_bom) {
  TzTextDecoder *d = (TzTextDecoder *)tz_obj_alloc(sizeof(TzTextDecoder));
  memset(d, 0, sizeof *d);
  d->rc = 1;
  d->layout = &tz_bytes_layouts[TZ_BYTES_TEXT_DECODER];
  d->encoding = encoding;
  d->fatal = fatal;
  d->ignore_bom = ignore_bom;
  d->table = table;

  return d;
}

static int is_space(uint32_t c) {
  return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

// `new TextDecoder(label)` of a label known only when the program runs:
// found among the labels of the Encoding standard, with the tables of the
// old encodings the program holds (all there are: `count` of one byte and
// `cjk_count` of several). An unknown label is a RangeError, as in node.
void *tz_text_decoder_new(void *obj, uint64_t meta, const TzEncodingTable *const *tables,
                          int32_t count, const TzCjkEncoding *const *cjk, int32_t cjk_count,
                          int32_t fatal, int32_t ignore_bom) {
  Units u;
  units_of(&u, obj, meta);
  // ASCII whitespace around the label does not count.
  uint32_t start = 0, end = u.length;
  while (start < end && is_space(unit_at(&u, start))) {
    ++start;
  }
  while (end > start && is_space(unit_at(&u, end - 1))) {
    --end;
  }
  char label[64];
  size_t n = 0;
  int ok = end - start < sizeof label;
  for (uint32_t i = start; i < end && ok; ++i) {
    uint32_t c = unit_at(&u, i);
    if (c >= 0x80) {
      ok = 0;
      break;
    }
    label[n++] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
  }
  label[n] = 0;
  const char *name = ok ? tz_encoding_name(label) : NULL;
  if (name) {
    if (strcmp(name, "utf-8") == 0) {
      return tz_text_decoder_make(TEXT_UTF8, NULL, fatal, ignore_bom);
    }
    if (strcmp(name, "utf-16le") == 0) {
      return tz_text_decoder_make(TEXT_UTF16LE, NULL, fatal, ignore_bom);
    }
    if (strcmp(name, "utf-16be") == 0) {
      return tz_text_decoder_make(TEXT_UTF16BE, NULL, fatal, ignore_bom);
    }
    for (int32_t i = 0; i < count; ++i) {
      if (strcmp(tables[i]->name, name) == 0) {
        return tz_text_decoder_make(TEXT_SINGLE_BYTE, tables[i], fatal, ignore_bom);
      }
    }
    for (int32_t i = 0; i < cjk_count; ++i) {
      if (strcmp(cjk[i]->name, name) == 0) {
        return tz_text_decoder_make(TEXT_CJK, cjk[i], fatal, ignore_bom);
      }
    }
  }
  TzStr s = {obj, meta};
  TzBuf text;
  tz_buf_init(&text);
  tz_buf_ascii(&text, "The \"");
  tz_buf_str(&text, s);
  tz_buf_ascii(&text, "\" encoding is not supported");
  TzStr message = tz_buf_take(&text);
  tz_throw_str(TZ_RANGE_ERROR, message.obj, message.meta);
  tz_str_release_value(message);

  return NULL;
}

TZ_COLD static void invalid_data(const char *encoding) {
  tz_throw(TZ_TYPE_ERROR, "The encoded data was not valid for encoding %s", encoding);
}

// A character of UTF-8 or UTF-16: the first of a stream is dropped if it is
// a BOM, unless the decoder keeps it.
static void put_text(TzTextDecoder *d, TzBuf *out, uint32_t cp) {
  if (!d->bom_seen) {
    d->bom_seen = 1;
    if (cp == 0xFEFF && !d->ignore_bom) {
      return;
    }
  }
  if (cp >= 0x10000) {
    cp -= 0x10000;
    tz_buf_push(out, (uint16_t)(0xD800 + (cp >> 10)));
    tz_buf_push(out, (uint16_t)(0xDC00 + (cp & 0x3FF)));
  } else {
    tz_buf_push(out, (uint16_t)cp);
  }
}

// The WHATWG UTF-8 decoder (see decode_utf8), byte by byte from the state of
// the stream: the code point so far, the bytes it needs and has, and the
// bounds of the next one. Returns 0 at the first error when fatal.
static int utf8_stream(TzTextDecoder *d, TzBuf *out, const uint8_t *s, int64_t n, int stream) {
  uint32_t cp = d->state[0], needed = d->state[1], seen = d->state[2];
  uint32_t lower = needed ? d->state[3] & 0xFF : 0x80, upper = needed ? d->state[3] >> 8 : 0xBF;
  int64_t i = 0;
  for (;;) {
    if (i == n) {
      if (stream) {
        break;
      }
      // The end of the stream, inside a character.
      if (needed) {
        cp = needed = seen = 0;
        if (d->fatal) {
          return 0;
        }
        put_text(d, out, 0xFFFD);
      }
      break;
    }
    uint32_t b = s[i];
    if (!needed) {
      ++i;
      if (b < 0x80) {
        put_text(d, out, b);
      } else if (b >= 0xC2 && b <= 0xDF) {
        needed = 1;
        cp = b & 0x1F;
      } else if (b >= 0xE0 && b <= 0xEF) {
        if (b == 0xE0) {
          lower = 0xA0;
        }
        if (b == 0xED) {
          upper = 0x9F;
        }
        needed = 2;
        cp = b & 0xF;
      } else if (b >= 0xF0 && b <= 0xF4) {
        if (b == 0xF0) {
          lower = 0x90;
        }
        if (b == 0xF4) {
          upper = 0x8F;
        }
        needed = 3;
        cp = b & 0x7;
      } else {
        if (d->fatal) {
          return 0;
        }
        put_text(d, out, 0xFFFD);
      }
      continue;
    }
    if (b < lower || b > upper) {
      // The byte that broke the character starts over.
      cp = needed = seen = 0;
      lower = 0x80;
      upper = 0xBF;
      if (d->fatal) {
        return 0;
      }
      put_text(d, out, 0xFFFD);
      continue;
    }
    ++i;
    lower = 0x80;
    upper = 0xBF;
    cp = (cp << 6) | (b & 0x3F);
    if (++seen != needed) {
      continue;
    }
    put_text(d, out, cp);
    cp = needed = seen = 0;
  }
  d->state[0] = cp;
  d->state[1] = needed;
  d->state[2] = seen;
  d->state[3] = lower | upper << 8;

  return 1;
}

// The WHATWG UTF-16 decoder, from the state of the stream: a byte of the
// next code unit (plus one, 0 for none) and a leading surrogate (0 for none).
static int utf16_stream(TzTextDecoder *d, TzBuf *out, const uint8_t *s, int64_t n, int stream,
                        int big) {
  int lead_byte = (int)d->state[0] - 1;
  uint32_t lead_surrogate = d->state[1];
  int back[2], restored = 0; // the bytes of a unit after a lone surrogate, the next one last
  int64_t i = 0;
  for (;;) {
    int b;
    if (restored) {
      b = back[--restored];
    } else if (i < n) {
      b = s[i++];
    } else if (stream) {
      break;
    } else {
      b = -1;
    }
    if (b < 0) {
      // The end of the stream, inside a character.
      if (lead_byte >= 0 || lead_surrogate) {
        lead_byte = -1;
        lead_surrogate = 0;
        if (d->fatal) {
          return 0;
        }
        put_text(d, out, 0xFFFD);
      }
      break;
    }
    if (lead_byte < 0) {
      lead_byte = b;
      continue;
    }
    uint32_t unit = big ? (uint32_t)(lead_byte << 8 | b) : (uint32_t)(b << 8 | lead_byte);
    lead_byte = -1;
    if (lead_surrogate) {
      uint32_t lead = lead_surrogate;
      lead_surrogate = 0;
      if (unit >= 0xDC00 && unit <= 0xDFFF) {
        put_text(d, out, 0x10000 + ((lead - 0xD800) << 10) + (unit - 0xDC00));
        continue;
      }
      // The unit starts over.
      int high = (int)(unit >> 8), low = (int)(unit & 0xFF);
      back[restored++] = big ? low : high;
      back[restored++] = big ? high : low;
      if (d->fatal) {
        return 0;
      }
      put_text(d, out, 0xFFFD);
      continue;
    }
    if (unit >= 0xD800 && unit <= 0xDBFF) {
      lead_surrogate = unit;
      continue;
    }
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
      if (d->fatal) {
        return 0;
      }
      put_text(d, out, 0xFFFD);
      continue;
    }
    put_text(d, out, unit);
  }
  d->state[0] = (uint32_t)(lead_byte + 1);
  d->state[1] = lead_surrogate;

  return 1;
}

// decode(bytes, { stream }): the text, from bytes the caller holds. A call
// without `stream` ends the stream, and the next one starts a new one.
void tz_text_decode(TzStr *out, void *decoder, const uint8_t *data, int64_t n, int32_t stream) {
  TzTextDecoder *d = (TzTextDecoder *)decoder;
  // On an error, the empty string, which holds no memory.
  *out = tz_str_from_ascii("", 0);
  if (!d->streaming) {
    memset(d->state, 0, sizeof d->state);
    d->bom_seen = 0;
  }
  // The whole text of a stream of UTF-8 at once.
  if (d->encoding == TEXT_UTF8 && !d->streaming && !stream && !d->fatal) {
    if (!d->ignore_bom && n >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
      data += 3;
      n -= 3;
    }
    *out = utf8_string(data, n);
    return;
  }
  d->streaming = stream;
  TzBuf text;
  tz_buf_init(&text);
  int ok = 1;
  switch (d->encoding) {
  case TEXT_UTF8:
    ok = utf8_stream(d, &text, data, n, stream);
    break;
  case TEXT_UTF16LE:
  case TEXT_UTF16BE:
    ok = utf16_stream(d, &text, data, n, stream, d->encoding == TEXT_UTF16BE);
    break;
  case TEXT_CJK: {
    const TzCjkEncoding *encoding = (const TzCjkEncoding *)d->table;
    ok = encoding->decode(encoding, d->state, &text, data, n, stream, d->fatal);
    break;
  }
  default: {
    const TzEncodingTable *table = (const TzEncodingTable *)d->table;
    for (int64_t i = 0; i < n && ok; ++i) {
      uint16_t c = data[i] < 0x80 ? data[i] : table->high[data[i] - 0x80];
      if (c == 0xFFFD && d->fatal) {
        ok = 0;
      } else {
        tz_buf_push(&text, c);
      }
    }
    break;
  }
  }
  if (!ok) {
    // The next decode starts a new stream.
    d->streaming = 0;
    tz_buf_free(&text);
    invalid_data(tz_text_decoder_name(d));
    return;
  }
  *out = tz_buf_take(&text);
}
