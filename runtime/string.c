// Strings: JavaScript semantics (length and indices in UTF-16 code units)
// with V8-style representations underneath.
//
// A string value is two machine words, `obj` and `meta`. The C functions below
// take them as two separate arguments and return strings through an `out`
// pointer, which keeps the calling convention trivial on every platform.
//
//   obj   the object holding the characters: a buffer, a rope node, a static
//         literal emitted by the compiler, NULL for an empty string, or the
//         characters themselves for an inline string
//   meta  bits  0-29  length in code units
//         bits 30-59  offset of the first code unit in the buffer (slices)
//         bit  60     ASCII: one-byte and all code units < 0x80
//         bit  61     TWO_BYTE: code units are 16-bit, otherwise 8-bit (Latin-1)
//         bit  62     ROPE: obj is a rope node (with HEAP) or holds the
//                     characters (without HEAP, see below)
//         bit  63     HEAP: obj is reference counted (literals are not)
//
// The layout must match CodeGen (kStr* constants in src/codegen/codegen.cpp).
//
// Representations:
//   inline        a one-byte string of up to 8 code units lives in `obj`, the
//                 first unit in the lowest byte and zeros after the last:
//                 no allocation and no reference count. Every one-byte string
//                 that short is inline, so two inline strings are equal
//                 exactly when their words are.
//   flat buffer   code units in one array, 1 or 2 bytes each. A buffer can
//                 have spare capacity: `a + b` appends `b` in place when `a`
//                 ends exactly where the buffer's used part ends, so building
//                 a string with `+=` is linear even when it is read in between.
//   slice         a value pointing into another buffer: substrings of 13 code
//                 units or more share the characters instead of copying them.
//   rope          `a + b` of 13 code units or more is a node pointing at both
//                 halves; the characters are copied once, when something
//                 reads them. The flat result is kept in the node and gets
//                 spare capacity for further appends.
//
// Every function takes its string arguments borrowed and returns an owned
// (+1) result.

#include "tzrt.h"

#include <stdlib.h>
#include <string.h>

// Searching uses SSE2, which every x86-64 has, where the compiler offers it.
#if defined(__SSE2__) && (defined(__clang__) || defined(__GNUC__))
#define TZ_SSE2 1
#include <emmintrin.h>
#endif

#define LEN_MASK ((1ull << 30) - 1)
#define OFF_SHIFT 30
#define F_ASCII (1ull << 60)
#define F_TWO_BYTE (1ull << 61)
#define F_ROPE (1ull << 62)
#define F_HEAP (1ull << 63)
#define MAX_LENGTH LEN_MASK

// Below this length concatenation copies and slices copy: a node or a shared
// buffer costs more than the characters themselves. V8 uses the same limit.
#define SHORT_LENGTH 13

// The longest inline string: as many one-byte code units as fit in `obj`.
#define INLINE_MAX 8

enum { KIND_BUF1 = 1, KIND_BUF2 = 2, KIND_ROPE = 3 };

typedef struct {
  int64_t rc;
  uint32_t kind;
  uint32_t reserved;
} Header;

typedef struct {
  Header h;
  uint32_t capacity; // in code units
  uint32_t used;     // code units written so far; appends continue here
} Buffer;            // the code units follow the header

typedef struct {
  Header h;
  TzStr left, right; // released once the rope is flattened
  TzStr flat;        // the flattened characters, or {NULL, 0}
} Rope;

static const TzStr kEmpty = {NULL, F_ASCII};

static inline uint32_t str_len(uint64_t meta) { return (uint32_t)(meta & LEN_MASK); }
static inline uint32_t str_off(uint64_t meta) { return (uint32_t)((meta >> OFF_SHIFT) & LEN_MASK); }
static inline int is_two(uint64_t meta) { return (meta & F_TWO_BYTE) != 0; }
static inline int is_ascii(uint64_t meta) { return (meta & F_ASCII) != 0; }
static inline int is_rope(uint64_t meta) { return (meta & (F_ROPE | F_HEAP)) == (F_ROPE | F_HEAP); }
static inline int is_inline(uint64_t meta) {
  return (meta & (F_ROPE | F_HEAP | F_TWO_BYTE)) == F_ROPE;
}

static inline uint64_t make_meta(uint32_t len, uint32_t off, int two, int ascii, uint64_t extra) {
  return (uint64_t)len | ((uint64_t)off << OFF_SHIFT) | (two ? F_TWO_BYTE : 0) |
         (ascii && !two ? F_ASCII : 0) | extra;
}

static inline TzStr make_str(void *obj, uint64_t meta) {
  TzStr s = {obj, meta};
  return s;
}

// Inline strings keep code unit i in bits 8*i..8*i+7 of `obj`, which puts
// them in memory order on a little-endian machine: units() hands out &obj.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#error "inline strings assume a little-endian target"
#endif

static inline uint64_t inline_bits(const TzStr *s) { return (uint64_t)(uintptr_t)s->obj; }

// An inline string of `len` (1 to INLINE_MAX) code units held in `bits`, with
// zeros above them. The ASCII flag is computed here, not taken from the
// source, so that equal inline strings have equal words.
static inline TzStr inline_from_bits(uint64_t bits, uint32_t len) {
  TzStr s;
  s.obj = (void *)(uintptr_t)bits;
  s.meta = (uint64_t)len | F_ROPE | ((bits & 0x8080808080808080ull) == 0 ? F_ASCII : 0);
  return s;
}

static inline TzStr inline_str(const uint8_t *data, uint32_t len) {
  uint64_t bits = 0;
  for (uint32_t i = 0; i < len; ++i)
    bits |= (uint64_t)data[i] << (8 * i);
  return inline_from_bits(bits, len);
}

// The inline string data[from..from+n) of one-byte units, 0 < n <= INLINE_MAX,
// where `end` units are readable from `data` on: eight bytes are loaded at
// once when there is room for them.
static inline TzStr inline_piece(const uint8_t *data, uint32_t end, uint32_t from, uint32_t n) {
  uint64_t bits;
  if (from + 8 <= end) {
    memcpy(&bits, data + from, 8);
    bits &= ~0ull >> (64 - 8 * n);
  } else {
    bits = 0;
    for (uint32_t i = 0; i < n; ++i)
      bits |= (uint64_t)data[from + i] << (8 * i);
  }
  return inline_from_bits(bits, n);
}

//===----------------------------------------------------------------------===//
// Objects and reference counts
//===----------------------------------------------------------------------===//

int64_t tz_live_strings; // not static: see the rules in tzrt.h

int64_t tz_str_live_objects(void) { return tz_live_strings; }

static void *object_new(size_t size, uint32_t kind) {
  Header *h = (Header *)tz_alloc(size);
  h->rc = 1;
  h->kind = kind;
  h->reserved = 0;
  ++tz_live_strings;
  return h;
}

static inline void retain(TzStr s) {
  if (s.meta & F_HEAP)
    ++((Header *)s.obj)->rc;
}

void tz_str_free(void *obj);

static inline void release(TzStr s) {
  if ((s.meta & F_HEAP) && --((Header *)s.obj)->rc == 0)
    tz_str_free(s.obj);
}

// A growable stack of pointers for the iterative walks below: ropes built by a
// loop can be a million levels deep, too deep for recursion.
typedef struct {
  void **items;
  size_t count, capacity;
  void *inline_items[32];
} PtrStack;

static void stack_init(PtrStack *s) {
  s->items = s->inline_items;
  s->count = 0;
  s->capacity = 32;
}

static void stack_push(PtrStack *s, void *item) {
  if (s->count == s->capacity) {
    size_t capacity = s->capacity * 2;
    void **items = (void **)tz_alloc(capacity * sizeof(void *));
    memcpy(items, s->items, s->count * sizeof(void *));
    if (s->items != s->inline_items)
      tz_free(s->items);
    s->items = items;
    s->capacity = capacity;
  }
  s->items[s->count++] = item;
}

static void stack_free(PtrStack *s) {
  if (s->items != s->inline_items)
    tz_free(s->items);
}

// Frees an object whose count reached zero, and everything that dies with it.
// Called by the inline release code that the compiler emits.
// tz_str_free of a rope: its halves may die with it.
TZ_NOINLINE void tz_str_free_rope(void *obj) {
  PtrStack pending;
  stack_init(&pending);
  stack_push(&pending, obj);
  while (pending.count > 0) {
    Header *h = (Header *)pending.items[--pending.count];
    if (h->kind == KIND_ROPE) {
      Rope *r = (Rope *)h;
      TzStr children[3] = {r->left, r->right, r->flat};
      for (int i = 0; i < 3; ++i)
        if ((children[i].meta & F_HEAP) && --((Header *)children[i].obj)->rc == 0)
          stack_push(&pending, children[i].obj);
    }
    --tz_live_strings;
    tz_free(h);
  }
  stack_free(&pending);
}

TZ_HOT void tz_str_free(void *obj) {
  if (((Header *)obj)->kind == KIND_ROPE) {
    tz_str_free_rope(obj);
    return;
  }
  --tz_live_strings; // a buffer: nothing else dies with it
  tz_free(obj);
}

//===----------------------------------------------------------------------===//
// Buffers and code units
//===----------------------------------------------------------------------===//

static Buffer *buffer_new(uint32_t capacity, int two) {
  Buffer *b = (Buffer *)object_new(sizeof(Buffer) + (size_t)capacity * (two ? 2 : 1),
                                   two ? KIND_BUF2 : KIND_BUF1);
  b->capacity = capacity;
  b->used = 0;
  return b;
}

static inline char *buffer_units(Buffer *b) { return (char *)b + sizeof(Buffer); }

TZ_NOINLINE void tz_str_flatten(Rope *r, uint64_t meta);

// The flat string behind `s`, borrowed from `s`. Ropes are flattened once.
// The words are read one by one, which keeps the result in registers: a copy
// of the whole struct would pick it from memory, `s` or the rope's.
static inline TzStr flat(TzStr s) {
  if (is_rope(s.meta)) {
    Rope *r = (Rope *)s.obj;
    if (!r->flat.obj)
      tz_str_flatten(r, s.meta);
    s.obj = r->flat.obj;
    s.meta = r->flat.meta;
  }
  return s;
}

// Code units of a flat string. They are inside `*f` itself when it is inline,
// so `f` must outlive the result.
static inline const void *units(const TzStr *f) {
  if (is_inline(f->meta))
    return &f->obj;
  if (!f->obj)
    return NULL;
  return (const char *)f->obj + sizeof(Buffer) +
         (size_t)str_off(f->meta) * (is_two(f->meta) ? 2 : 1);
}

static inline uint16_t unit_at(const TzStr *f, uint32_t i) {
  const void *u = units(f);
  return is_two(f->meta) ? ((const uint16_t *)u)[i] : ((const uint8_t *)u)[i];
}

// The code unit at `i` of a one-byte string that is no rope node: a buffer or
// a literal, tested first (the usual long text), or an inline string; -1 for
// any other string. It is read from the two words alone, so that a loop over
// the string keeps everything in registers.
static inline int32_t direct_byte(void *obj, uint64_t meta, uint32_t i) {
  if ((meta & (F_ROPE | F_TWO_BYTE)) == 0)
    return ((const uint8_t *)obj)[sizeof(Buffer) + str_off(meta) + i];
  if (is_inline(meta))
    return (uint8_t)((uint64_t)(uintptr_t)obj >> (8 * i));
  return -1;
}

// Whether a string is one-byte and no rope node: what direct_byte reads.
static inline int direct_bytes(uint64_t meta) { return !is_rope(meta) && !is_two(meta); }

// Copies the code units of flat `src` to `dst`, widening one-byte units when
// `dst_two` is set. Narrowing is the caller's job.
static void copy_units(void *dst, int dst_two, const TzStr *src) {
  uint32_t len = str_len(src->meta);
  if (len == 0)
    return;
  const void *from = units(src);
  if (dst_two == is_two(src->meta)) {
    memcpy(dst, from, (size_t)len * (dst_two ? 2 : 1));
  } else {
    uint16_t *to = (uint16_t *)dst;
    const uint8_t *bytes = (const uint8_t *)from;
    for (uint32_t i = 0; i < len; ++i)
      to[i] = bytes[i];
  }
}

// Adds the code units of one-byte `s` at position `pos` of an inline string
// being built in `bits`; pos + length of s must not exceed INLINE_MAX.
static inline uint64_t add_inline_units(uint64_t bits, uint32_t pos, TzStr s) {
  uint32_t len = str_len(s.meta);
  if (len == 0)
    return bits;
  if (is_inline(s.meta))
    return bits | inline_bits(&s) << (8 * pos);
  TzStr f = flat(s);
  const uint8_t *u = (const uint8_t *)units(&f);
  for (uint32_t i = 0; i < len; ++i)
    bits |= (uint64_t)u[i] << (8 * (pos + i));
  return bits;
}

TZ_NOINLINE void tz_str_flatten(Rope *r, uint64_t meta) {
  uint32_t len = str_len(meta);
  int two = is_two(meta);
  // Spare room: a flattened string is often appended to next.
  Buffer *b = buffer_new(len + len / 2, two);
  char *dst = buffer_units(b);
  size_t width = two ? 2 : 1;
  uint32_t pos = 0;

  // Copy the leaves left to right without recursion.
  PtrStack pending;
  stack_init(&pending);
  stack_push(&pending, &r->right);
  stack_push(&pending, &r->left);
  while (pending.count > 0) {
    const TzStr *s = (const TzStr *)pending.items[--pending.count];
    if (is_rope(s->meta)) {
      Rope *child = (Rope *)s->obj;
      if (!child->flat.obj) {
        stack_push(&pending, &child->right);
        stack_push(&pending, &child->left);
        continue;
      }
      copy_units(dst + pos * width, two, &child->flat);
    } else {
      copy_units(dst + pos * width, two, s);
    }
    pos += str_len(s->meta);
  }
  stack_free(&pending);

  b->used = len;
  r->flat = make_str(b, make_meta(len, 0, two, is_ascii(meta), F_HEAP));
  release(r->left);
  release(r->right);
  r->left = kEmpty;
  r->right = kEmpty;
}

static TzStr single_char(uint8_t unit) { return inline_str(&unit, 1); }

// A new flat string holding `units` (1 or 2 bytes each), narrowed to one byte
// per unit when possible.
static TzStr string_from_units(const void *data, uint32_t len, int two) {
  if (len == 0)
    return kEmpty;
  int narrow = 1;
  if (two) {
    for (uint32_t i = 0; i < len; ++i) {
      if (((const uint16_t *)data)[i] > 0xFF) {
        narrow = 0;
        break;
      }
    }
  }
  if (narrow && len <= INLINE_MAX) {
    uint8_t bytes[INLINE_MAX];
    for (uint32_t i = 0; i < len; ++i)
      bytes[i] = two ? (uint8_t)((const uint16_t *)data)[i] : ((const uint8_t *)data)[i];
    return inline_str(bytes, len);
  }
  int ascii = 1;
  for (uint32_t i = 0; i < len && ascii; ++i)
    ascii = (two ? ((const uint16_t *)data)[i] : ((const uint8_t *)data)[i]) < 0x80;

  Buffer *b = buffer_new(len, !narrow);
  if (narrow && two) {
    uint8_t *to = (uint8_t *)buffer_units(b);
    for (uint32_t i = 0; i < len; ++i)
      to[i] = (uint8_t)((const uint16_t *)data)[i];
  } else {
    memcpy(buffer_units(b), data, (size_t)len * (two ? 2 : 1));
  }
  b->used = len;
  return make_str(b, make_meta(len, 0, !narrow, ascii, F_HEAP));
}

static TzStr string_from_ascii(const char *text, size_t len) {
  return string_from_units(text, (uint32_t)len, 0);
}

TzStr tz_str_from_ascii(const char *text, size_t length) { return string_from_ascii(text, length); }

// A new string of `length` ASCII characters, which the caller writes at the
// pointer returned before using the string: the text of base64 and hex
// (buffer.c), made without a copy. NULL for a length that fits in the value
// itself; then the caller makes the string with tz_str_from_ascii.
char *tz_str_ascii_units(size_t length, TzStr *out) {
  if (length <= INLINE_MAX)
    return NULL;
  if (length > MAX_LENGTH)
    tz_panicf("string length %llu exceeds the maximum of %llu", (unsigned long long)length,
              (unsigned long long)MAX_LENGTH);
  Buffer *b = buffer_new((uint32_t)length, 0);
  b->used = (uint32_t)length;
  *out = make_str(b, make_meta((uint32_t)length, 0, 0, 1, F_HEAP));
  return buffer_units(b);
}

TzStr tz_str_from_utf16(const uint16_t *units, size_t length) {
  if (length > MAX_LENGTH)
    tz_panicf("string length %llu exceeds the maximum of %llu", (unsigned long long)length,
              (unsigned long long)MAX_LENGTH);
  return string_from_units(units, (uint32_t)length, 1);
}

void tz_str_retain_value(TzStr s) { retain(s); }

void tz_str_release_value(TzStr s) { release(s); }

//===----------------------------------------------------------------------===//
// Concatenation
//===----------------------------------------------------------------------===//

static TzStr rope_new(TzStr left, TzStr right) {
  uint32_t len = str_len(left.meta) + str_len(right.meta);
  int two = is_two(left.meta) || is_two(right.meta);
  int ascii = is_ascii(left.meta) && is_ascii(right.meta);
  Rope *r = (Rope *)object_new(sizeof(Rope), KIND_ROPE);
  retain(left);
  retain(right);
  r->left = left;
  r->right = right;
  r->flat.obj = NULL;
  r->flat.meta = 0;
  return make_str(r, make_meta(len, 0, two, ascii, F_HEAP | F_ROPE));
}

// Copies n bytes; a short copy is two overlapping words, not a call.
static inline void copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t n) {
  if (n >= 8 && n <= 16) {
    uint64_t head, tail;
    memcpy(&head, src, 8);
    memcpy(&tail, src + n - 8, 8);
    memcpy(dst, &head, 8);
    memcpy(dst + n - 8, &tail, 8);
  } else {
    memcpy(dst, src, n);
  }
}

// The one-byte case of join_sep below: the one-byte `items` joined by flat
// `sep` into `out`, which has room for exactly the `total` units they make.
static void join_bytes(uint8_t *out, uint32_t total, const TzStr *items, int64_t n,
                       const TzStr *sep) {
  uint32_t sep_len = str_len(sep->meta), pos = 0;
  const uint8_t *sep_units = (const uint8_t *)units(sep);
  for (int64_t i = 0; i < n; ++i) {
    if (i > 0 && sep_len) {
      if (sep_len == 1)
        out[pos] = sep_units[0];
      else
        copy_bytes(out + pos, sep_units, sep_len);
      pos += sep_len;
    }
    TzStr s = items[i];
    uint32_t len = str_len(s.meta);
    if (is_inline(s.meta) && pos + 8 <= total) {
      // All eight bytes at once: the zeros after the string are written over
      // by what comes next.
      memcpy(out + pos, &s.obj, 8);
    } else if (len) {
      TzStr f = flat(s);
      copy_bytes(out + pos, (const uint8_t *)units(&f), len);
    }
    pos += len;
  }
}

// `items` joined by `sep` (NULL for none) into a new flat string of exactly
// the needed size, inline when it is short enough.
static TzStr join_sep(const TzStr *items, int64_t n, const TzStr *sep) {
  uint64_t total = 0;
  int two = 0, ascii = 1;
  for (int64_t i = 0; i < n; ++i) {
    total += str_len(items[i].meta);
    two |= is_two(items[i].meta);
    ascii &= is_ascii(items[i].meta);
  }
  TzStr separator = kEmpty;
  if (sep && n > 1) {
    separator = flat(*sep);
    total += (uint64_t)str_len(separator.meta) * (uint64_t)(n - 1);
    two |= is_two(separator.meta);
    ascii &= is_ascii(separator.meta);
  }
  if (total > MAX_LENGTH)
    tz_panicf("string length %llu exceeds the maximum of %llu", (unsigned long long)total,
              (unsigned long long)MAX_LENGTH);
  if (total == 0)
    return kEmpty;

  uint32_t sep_len = str_len(separator.meta);
  uint32_t pos = 0;
  if (!two && total <= INLINE_MAX) {
    uint64_t bits = 0;
    for (int64_t i = 0; i < n; ++i) {
      if (i > 0) {
        bits = add_inline_units(bits, pos, separator);
        pos += sep_len;
      }
      bits = add_inline_units(bits, pos, items[i]);
      pos += str_len(items[i].meta);
    }
    return inline_from_bits(bits, pos);
  }

  Buffer *b = buffer_new((uint32_t)total, two);
  char *dst = buffer_units(b);
  if (!two) {
    join_bytes((uint8_t *)dst, (uint32_t)total, items, n, &separator);
    pos = (uint32_t)total;
  } else {
    for (int64_t i = 0; i < n; ++i) {
      if (i > 0 && sep_len) {
        copy_units(dst + (size_t)pos * 2, 1, &separator);
        pos += sep_len;
      }
      TzStr f = flat(items[i]);
      copy_units(dst + (size_t)pos * 2, 1, &f);
      pos += str_len(f.meta);
    }
  }
  b->used = pos;
  return make_str(b, make_meta(pos, 0, two, ascii, F_HEAP));
}

static TzStr join(const TzStr *parts, int64_t n) { return join_sep(parts, n, NULL); }

// Appends parts[1..n) to parts[0] without copying it, if parts[0] ends where
// its buffer's used part ends and the buffer has room.
static int append_in_place(TzStr *out, const TzStr *parts, int64_t n, uint32_t total, int two,
                           int ascii) {
  TzStr base = parts[0];
  if (is_rope(base.meta)) {
    Rope *r = (Rope *)base.obj;
    if (!r->flat.obj)
      return 0;
    base = r->flat;
  }
  if (!(base.meta & F_HEAP))
    return 0;
  int base_two = is_two(base.meta);
  if (two && !base_two)
    return 0;
  Buffer *b = (Buffer *)base.obj;
  uint32_t off = str_off(base.meta), len = str_len(base.meta);
  uint32_t extra = total - len;
  // This check and the update of `used` become a compare-and-swap once tasks
  // run in parallel.
  if (off + len != b->used || (uint64_t)b->used + extra > b->capacity)
    return 0;

  char *dst = buffer_units(b);
  size_t width = base_two ? 2 : 1;
  uint32_t pos = b->used;
  for (int64_t i = 1; i < n; ++i) {
    TzStr f = flat(parts[i]);
    copy_units(dst + pos * width, base_two, &f);
    pos += str_len(f.meta);
  }
  b->used = pos;
  ++b->h.rc;
  *out = make_str(b, make_meta(total, off, base_two, ascii && is_ascii(base.meta), F_HEAP));
  return 1;
}

// A number the compiler passes to tz_str_concat_n as it is, so that `"x" + i`
// needs no temporary string: `meta` has the ROPE and TWO_BYTE flags without
// HEAP, which no real string has, and NUMBER_INT or NUMBER_DOUBLE in its low
// bits; `obj` holds the bits of the value. Must match kNumberPart in CodeGen.
#define NUMBER_INT 1
#define NUMBER_DOUBLE 2

static inline int is_number_part(uint64_t meta) {
  return (meta & (F_ROPE | F_HEAP | F_TWO_BYTE)) == (F_ROPE | F_TWO_BYTE);
}

// The text of a number part too long to be inline: a flat one-byte string on
// the stack. It is never counted (no HEAP flag), so it must not outlive the
// concatenation.
typedef struct {
  Buffer b;
  char units[32];
} NumberText;

// Writes the decimal digits of `value` so that they end at `end` (at most 20
// characters); returns where they start.
static char *format_i64(char *end, int64_t value) {
  char *p = end;
  uint64_t magnitude = value < 0 ? 0 - (uint64_t)value : (uint64_t)value;
  do {
    *--p = (char)('0' + magnitude % 10);
    magnitude /= 10;
  } while (magnitude != 0);
  if (value < 0)
    *--p = '-';
  return p;
}

// The text of a number part: an inline string when it is short enough,
// otherwise a flat string in `t`.
static TzStr number_text(NumberText *t, TzStr part) {
  uint64_t bits = (uint64_t)(uintptr_t)part.obj;
  const char *text = t->units;
  size_t len;
  if ((part.meta & LEN_MASK) == NUMBER_INT) {
    char *end = t->units + sizeof t->units;
    text = format_i64(end, (int64_t)bits);
    len = (size_t)(end - text);
  } else {
    double value;
    memcpy(&value, &bits, sizeof value);
    len = tz_format_f64(value, t->units);
  }
  if (len <= INLINE_MAX)
    return inline_str((const uint8_t *)text, (uint32_t)len);
  memmove(t->units, text, len);
  t->b.h.rc = 1;
  t->b.h.kind = KIND_BUF1;
  t->b.h.reserved = 0;
  t->b.capacity = t->b.used = (uint32_t)len;
  return make_str(&t->b, make_meta((uint32_t)len, 0, 0, 1, 0));
}

// The usual `"x" + i`: when every part is inline or a short number and the
// result fits, it is built in a register. Returns 0 if it does not fit.
static int concat_inline(TzStr *out, const TzStr *parts, int64_t n) {
  uint64_t bits = 0;
  uint32_t pos = 0;
  for (int64_t i = 0; i < n; ++i) {
    uint64_t meta = parts[i].meta;
    if (is_inline(meta)) {
      uint32_t len = str_len(meta);
      if (len > INLINE_MAX - pos)
        return 0;
      bits |= inline_bits(&parts[i]) << (8 * pos);
      pos += len;
    } else if (is_number_part(meta) && (meta & LEN_MASK) == NUMBER_INT) {
      char buf[24];
      char *end = buf + sizeof buf;
      const char *text = format_i64(end, (int64_t)(uintptr_t)parts[i].obj);
      uint32_t len = (uint32_t)(end - text);
      if (len > INLINE_MAX - pos)
        return 0;
      for (uint32_t j = 0; j < len; ++j)
        bits |= (uint64_t)(uint8_t)text[j] << (8 * (pos + j));
      pos += len;
    } else if (str_len(meta) != 0 || is_number_part(meta)) {
      return 0;
    }
  }
  *out = pos == 0 ? kEmpty : inline_from_bits(bits, pos);
  return 1;
}

// tz_str_concat_n when the result is not a short string.
TZ_NOINLINE void tz_str_concat_slow(TzStr *out, const TzStr *parts, int64_t n) {
  TzStr local[16];
  NumberText local_numbers[4];
  TzStr *kept = n <= 16 ? local : (TzStr *)tz_alloc((size_t)n * sizeof(TzStr));
  int64_t numbers_needed = 0;
  for (int64_t i = 0; i < n; ++i)
    numbers_needed += is_number_part(parts[i].meta);
  NumberText *numbers = numbers_needed <= 4 ? local_numbers
                                            : (NumberText *)tz_alloc((size_t)numbers_needed * sizeof(NumberText));
  int64_t numbers_used = 0;

  uint64_t total = 0;
  int two = 0, ascii = 1;
  int64_t count = 0;
  for (int64_t i = 0; i < n; ++i) {
    TzStr part = parts[i];
    if (is_number_part(part.meta))
      part = number_text(&numbers[numbers_used++], part);
    uint32_t len = str_len(part.meta);
    if (len == 0)
      continue;
    kept[count++] = part;
    total += len;
    two |= is_two(part.meta);
    ascii &= is_ascii(part.meta);
  }
  if (total > MAX_LENGTH)
    tz_panicf("string length %llu exceeds the maximum of %llu", (unsigned long long)total,
              (unsigned long long)MAX_LENGTH);

  // A number's text on the stack must become a real string before a result
  // keeps a reference to it. Such text is flat and not counted; inline
  // strings and literals never point into `numbers`.
#define IS_TEMP(s)                                                                                 \
  (numbers_used && !((s).meta & (F_ROPE | F_HEAP)) && (s).obj >= (void *)numbers &&                \
   (s).obj < (void *)(numbers + numbers_used))
  int temp0 = count > 0 && IS_TEMP(kept[0]);
  int temp1 = count > 1 && IS_TEMP(kept[1]);
#undef IS_TEMP

  if (count == 0) {
    *out = kEmpty;
  } else if (!two && total <= INLINE_MAX) {
    // The usual `"x" + i`: built in a register, nothing to allocate.
    uint64_t bits = 0;
    uint32_t pos = 0;
    for (int64_t i = 0; i < count; ++i) {
      bits = add_inline_units(bits, pos, kept[i]);
      pos += str_len(kept[i].meta);
    }
    *out = inline_from_bits(bits, pos);
  } else if (count == 1) {
    if (temp0) {
      *out = string_from_units(units(&kept[0]), str_len(kept[0].meta), 0);
    } else {
      retain(kept[0]);
      *out = kept[0];
    }
  } else if (append_in_place(out, kept, count, (uint32_t)total, two, ascii)) {
    // done
  } else if (total >= SHORT_LENGTH && count == 2) {
    TzStr left = temp0 ? string_from_units(units(&kept[0]), str_len(kept[0].meta), 0) : kept[0];
    TzStr right = temp1 ? string_from_units(units(&kept[1]), str_len(kept[1].meta), 0) : kept[1];
    *out = rope_new(left, right);
    if (temp0)
      release(left);
    if (temp1)
      release(right);
  } else if (total >= SHORT_LENGTH && str_len(kept[0].meta) >= total - str_len(kept[0].meta)) {
    // A long string plus a few short ones, as in `s = s + x + "\n"`: keep the
    // long one and join only the rest.
    TzStr rest = join(kept + 1, count - 1);
    TzStr first = temp0 ? string_from_units(units(&kept[0]), str_len(kept[0].meta), 0) : kept[0];
    *out = rope_new(first, rest);
    release(rest);
    if (temp0)
      release(first);
  } else {
    *out = join(kept, count);
  }

  if (kept != local)
    tz_free(kept);
  if (numbers != local_numbers)
    tz_free(numbers);
}

// `parts[0] + parts[1] + ... + parts[n-1]`: the compiler turns chains of `+`
// and template literals into one call. Parts may be number parts.
void tz_str_concat_n(TzStr *out, const TzStr *parts, int64_t n) {
  if (!concat_inline(out, parts, n))
    tz_str_concat_slow(out, parts, n);
}

// `items` joined by `sep` (NULL for ","), in one buffer of the exact size:
// join() of a string[].
void tz_str_join(TzStr *out, const TzStr *items, uint32_t n, const TzStr *sep) {
  TzStr comma = single_char(',');
  *out = join_sep(items, n, sep ? sep : &comma);
}

//===----------------------------------------------------------------------===//
// Comparison and search
//===----------------------------------------------------------------------===//

// Whether a[ia..ia+n) and b[ib..ib+n) hold the same code units.
TZ_NOINLINE int tz_str_units_equal(const TzStr *a, uint32_t ia, const TzStr *b, uint32_t ib,
                                   uint32_t n) {
  if (n == 0)
    return 1;
  if (is_two(a->meta) == is_two(b->meta)) {
    size_t width = is_two(a->meta) ? 2 : 1;
    return memcmp((const char *)units(a) + ia * width, (const char *)units(b) + ib * width,
                  n * width) == 0;
  }
  for (uint32_t i = 0; i < n; ++i)
    if (unit_at(a, ia + i) != unit_at(b, ib + i))
      return 0;
  return 1;
}

TZ_HOT int32_t tz_str_equal(void *a_obj, uint64_t a_meta, void *b_obj, uint64_t b_meta) {
  uint32_t len = str_len(a_meta);
  if (len != str_len(b_meta))
    return 0;
  if (a_obj == b_obj && a_meta == b_meta)
    return 1;
  if (is_inline(a_meta) && is_inline(b_meta))
    return 0;
  TzStr a = flat(make_str(a_obj, a_meta)), b = flat(make_str(b_obj, b_meta));
  return tz_str_units_equal(&a, 0, &b, 0, len);
}

static inline int32_t compare_lengths(uint32_t la, uint32_t lb) {
  return la == lb ? 0 : la < lb ? -1 : 1;
}

TZ_NOINLINE int32_t tz_str_compare_slow(void *a_obj, uint64_t a_meta, void *b_obj,
                                        uint64_t b_meta) {
  TzStr a = flat(make_str(a_obj, a_meta)), b = flat(make_str(b_obj, b_meta));
  uint32_t la = str_len(a.meta), lb = str_len(b.meta);
  uint32_t n = la < lb ? la : lb;
  if (n == 0)
    return compare_lengths(la, lb);
  if (!is_two(a.meta) && !is_two(b.meta)) {
    // Bytes compare as the code units they are.
    int c = memcmp(units(&a), units(&b), n);
    return c != 0 ? (c < 0 ? -1 : 1) : compare_lengths(la, lb);
  }
  for (uint32_t i = 0; i < n; ++i) {
    uint16_t x = unit_at(&a, i), y = unit_at(&b, i);
    if (x != y)
      return x < y ? -1 : 1;
  }
  return compare_lengths(la, lb);
}

static inline uint64_t byte_swap(uint64_t x) {
#if defined(__clang__) || defined(__GNUC__)
  return __builtin_bswap64(x);
#else
  uint64_t y = 0;
  for (int i = 0; i < 8; ++i, x >>= 8)
    y = (y << 8) | (x & 0xFF);
  return y;
#endif
}

// The first eight code units of a one-byte string that is no rope, as a word
// with the first unit as the highest byte: an inline string has zeros after
// its last unit, and a buffer has at least nine units.
static inline uint64_t first_word(void *obj, uint64_t meta) {
  uint64_t word = (uint64_t)(uintptr_t)obj;
  if (!is_inline(meta))
    memcpy(&word, (const char *)obj + sizeof(Buffer) + str_off(meta), 8);
  return byte_swap(word);
}

// Orders strings by UTF-16 code units, as JavaScript's `<` does. One-byte
// strings mostly differ in their first eight units, which compare as words:
// the zeros after the end of a shorter inline string order it first, as its
// length does.
TZ_HOT int32_t tz_str_compare(void *a_obj, uint64_t a_meta, void *b_obj, uint64_t b_meta) {
  if (direct_bytes(a_meta) && direct_bytes(b_meta) &&
      (is_inline(a_meta) || str_len(a_meta) >= 8) && (is_inline(b_meta) || str_len(b_meta) >= 8)) {
    uint64_t x = first_word(a_obj, a_meta), y = first_word(b_obj, b_meta);
    if (x != y)
      return x < y ? -1 : 1;
    if (is_inline(a_meta) && is_inline(b_meta))
      return compare_lengths(str_len(a_meta), str_len(b_meta));
  }
  return tz_str_compare_slow(a_obj, a_meta, b_obj, b_meta);
}

static uint32_t clamp(int64_t value, uint32_t len) {
  return value < 0 ? 0 : value > (int64_t)len ? len : (uint32_t)value;
}

// The position of the first byte `c` in data[from..end), or `end`. Sixteen
// bytes at a time, without reading past `end`.
static inline uint32_t find_byte(const uint8_t *data, uint32_t from, uint32_t end, uint8_t c) {
#ifdef TZ_SSE2
  __m128i needle = _mm_set1_epi8((char)c);
  for (; from + 16 <= end; from += 16) {
    __m128i block = _mm_loadu_si128((const __m128i *)(data + from));
    unsigned mask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(block, needle));
    if (mask)
      return from + (uint32_t)__builtin_ctz(mask);
  }
#endif
  for (; from < end; ++from)
    if (data[from] == c)
      return from;
  return end;
}

// The same for a 16-bit code unit, eight at a time.
static inline uint32_t find_unit16(const uint16_t *data, uint32_t from, uint32_t end, uint16_t c) {
#ifdef TZ_SSE2
  __m128i needle = _mm_set1_epi16((short)c);
  for (; from + 8 <= end; from += 8) {
    __m128i block = _mm_loadu_si128((const __m128i *)(data + from));
    unsigned mask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi16(block, needle));
    if (mask)
      return from + (uint32_t)__builtin_ctz(mask) / 2;
  }
#endif
  for (; from < end; ++from)
    if (data[from] == c)
      return from;
  return end;
}

// The first code unit `c` in units[from..end) of a flat string, or `end`.
static inline uint32_t find_unit(const void *units, int two, uint32_t from, uint32_t end,
                                 uint16_t c) {
  if (two)
    return find_unit16((const uint16_t *)units, from, end, c);
  return c > 0xFF ? end : find_byte((const uint8_t *)units, from, end, (uint8_t)c);
}

// How many bytes `c` data[0..end) holds, counting no further than `max`.
static uint32_t count_byte(const uint8_t *data, uint32_t end, uint8_t c, uint32_t max) {
  uint32_t count = 0, i = 0;
#ifdef TZ_SSE2
  // Sixteen counters of a byte each, summed every 255 blocks before they
  // could overflow.
  __m128i needle = _mm_set1_epi8((char)c), zero = _mm_setzero_si128();
  while (i + 16 <= end && count < max) {
    uint32_t stop = end - i >= 255 * 16 ? i + 255 * 16 : end - 15;
    __m128i counters = zero;
    for (; i < stop; i += 16) {
      __m128i block = _mm_loadu_si128((const __m128i *)(data + i));
      counters = _mm_sub_epi8(counters, _mm_cmpeq_epi8(block, needle));
    }
    __m128i sums = _mm_sad_epu8(counters, zero);
    count += (uint32_t)_mm_cvtsi128_si32(sums) + (uint32_t)_mm_extract_epi16(sums, 4);
  }
#endif
  for (; i < end && count < max; ++i)
    count += data[i] == c;
  return count < max ? count : max;
}

static inline int bytes_equal(const uint8_t *a, const uint8_t *b, uint32_t n) {
  if (n > 16)
    return memcmp(a, b, n) == 0;
  for (uint32_t i = 0; i < n; ++i)
    if (a[i] != b[i])
      return 0;
  return 1;
}

// The first start of needle[0..n) (n >= 2) in data from `from` on, before
// `end`, the last start + 1; or `end`. The candidates are the places where
// both the first and the last byte match, sixteen at a time.
static uint32_t find_bytes(const uint8_t *data, uint32_t from, uint32_t end, const uint8_t *needle,
                           uint32_t n) {
#ifdef TZ_SSE2
  __m128i first = _mm_set1_epi8((char)needle[0]), last = _mm_set1_epi8((char)needle[n - 1]);
  for (; from + 16 <= end; from += 16) {
    __m128i a = _mm_loadu_si128((const __m128i *)(data + from));
    __m128i b = _mm_loadu_si128((const __m128i *)(data + from + n - 1));
    unsigned mask = (unsigned)_mm_movemask_epi8(
        _mm_and_si128(_mm_cmpeq_epi8(a, first), _mm_cmpeq_epi8(b, last)));
    for (; mask; mask &= mask - 1) {
      uint32_t i = from + (uint32_t)__builtin_ctz(mask);
      if (bytes_equal(data + i + 1, needle + 1, n - 2))
        return i;
    }
  }
#endif
  for (; from < end; ++from)
    if (data[from] == needle[0] && data[from + n - 1] == needle[n - 1] &&
        bytes_equal(data + from + 1, needle + 1, n - 2))
      return from;
  return end;
}

// Like JavaScript's indexOf: the first match at or after `from`, or -1. Both
// strings are flat; `from` is at most the length of `s`.
static int64_t index_of(const TzStr *s, const TzStr *t, uint32_t from) {
  uint32_t len = str_len(s->meta), tlen = str_len(t->meta);
  if (tlen == 0)
    return from;
  if (tlen > len)
    return -1;
  const void *su = units(s);
  int two = is_two(s->meta);
  // The last start + 1.
  uint32_t end = len - tlen + 1;
  if (!two && !is_two(t->meta)) {
    const uint8_t *tu = (const uint8_t *)units(t);
    uint32_t i = tlen == 1 ? find_byte((const uint8_t *)su, from, end, tu[0])
                           : find_bytes((const uint8_t *)su, from, end, tu, tlen);
    return i == end ? -1 : (int64_t)i;
  }
  uint16_t first = unit_at(t, 0);
  // Candidates start where the first unit matches; the rest is compared.
  for (uint32_t i = from; i < end; ++i) {
    i = find_unit(su, two, i, end, first);
    if (i == end)
      return -1;
    if (tlen == 1 || tz_str_units_equal(s, i + 1, t, 1, tlen - 1))
      return i;
  }
  return -1;
}

int32_t tz_str_index_of(void *s_obj, uint64_t s_meta, void *t_obj, uint64_t t_meta, int64_t from,
                        int32_t has_from) {
  TzStr s = flat(make_str(s_obj, s_meta)), t = flat(make_str(t_obj, t_meta));
  return (int32_t)index_of(&s, &t, has_from ? clamp(from, str_len(s.meta)) : 0);
}

int32_t tz_str_includes(void *s_obj, uint64_t s_meta, void *t_obj, uint64_t t_meta, int64_t from,
                        int32_t has_from) {
  return tz_str_index_of(s_obj, s_meta, t_obj, t_meta, from, has_from) >= 0;
}

// Whether inline `s` holds inline `t` at `start`; start + length of t must
// not exceed the length of s.
static inline int inline_has_at(void *s_obj, uint32_t start, void *t_obj, uint32_t tlen) {
  if (tlen == 0)
    return 1;
  uint64_t mask = tlen == 8 ? ~0ull : (1ull << (8 * tlen)) - 1;
  return (((uint64_t)(uintptr_t)s_obj >> (8 * start)) & mask) == (uint64_t)(uintptr_t)t_obj;
}

TZ_HOT int32_t tz_str_starts_with(void *s_obj, uint64_t s_meta, void *t_obj, uint64_t t_meta,
                                  int64_t position, int32_t has_position) {
  uint32_t len = str_len(s_meta), tlen = str_len(t_meta);
  uint32_t start = has_position ? clamp(position, len) : 0;
  if ((uint64_t)start + tlen > len)
    return 0;
  if (is_inline(s_meta) && is_inline(t_meta))
    return inline_has_at(s_obj, start, t_obj, tlen);
  TzStr s = flat(make_str(s_obj, s_meta)), t = flat(make_str(t_obj, t_meta));
  return tz_str_units_equal(&s, start, &t, 0, tlen);
}

TZ_HOT int32_t tz_str_ends_with(void *s_obj, uint64_t s_meta, void *t_obj, uint64_t t_meta,
                                int64_t end_position, int32_t has_end) {
  uint32_t len = str_len(s_meta), tlen = str_len(t_meta);
  uint32_t end = has_end ? clamp(end_position, len) : len;
  if (tlen > end)
    return 0;
  if (is_inline(s_meta) && is_inline(t_meta))
    return inline_has_at(s_obj, end - tlen, t_obj, tlen);
  TzStr s = flat(make_str(s_obj, s_meta)), t = flat(make_str(t_obj, t_meta));
  return tz_str_units_equal(&s, end - tlen, &t, 0, tlen);
}

//===----------------------------------------------------------------------===//
// Characters and substrings
//===----------------------------------------------------------------------===//

// The compiler checks or proves every index before calling these, so this
// fires only on a bug in the compiler.
static void check_index(int64_t index, uint32_t len) {
  if (index < 0 || index >= (int64_t)len)
    tz_panicf("internal error: string index %lld out of range for length %u", (long long)index,
              len);
}

TZ_NOINLINE int32_t tz_str_char_code_at_slow(void *obj, uint64_t meta, uint32_t index) {
  TzStr f = flat(make_str(obj, meta));
  return unit_at(&f, index);
}

TZ_HOT int32_t tz_str_char_code_at(void *obj, uint64_t meta, int64_t index) {
  check_index(index, str_len(meta));
  int32_t unit = direct_byte(obj, meta, (uint32_t)index);
  return unit >= 0 ? unit : tz_str_char_code_at_slow(obj, meta, (uint32_t)index);
}

// A copy of data[from..from+n) of flat one-byte `f`, whose units are `data`,
// for INLINE_MAX < n < SHORT_LENGTH, as substring_of makes it.
TZ_NOINLINE void tz_str_short_piece(TzStr *out, const TzStr *f, const uint8_t *data,
                                    uint32_t from, uint32_t n) {
  if (n == str_len(f->meta)) {
    retain(*f);
    *out = *f;
    return;
  }
  Buffer *b = buffer_new(n, 0);
  copy_bytes((uint8_t *)buffer_units(b), data + from, n);
  b->used = n;
  int ascii = 1;
  if (!is_ascii(f->meta))
    for (uint32_t i = 0; i < n && ascii; ++i)
      ascii = data[from + i] < 0x80;
  *out = make_str(b, make_meta(n, 0, 0, ascii, F_HEAP));
}

// s[from..to): short pieces are copied, longer ones share the buffer. The
// string comes as its two words and the result goes out through `out`: a
// TzStr passed by value would go through memory.
TZ_NOINLINE void tz_str_substring_into(TzStr *out, void *obj, uint64_t meta, uint32_t from,
                                       uint32_t to) {
  TzStr s = make_str(obj, meta);
  uint32_t n = to - from;
  if (n == 0) {
    *out = kEmpty;
    return;
  }
  if (n == str_len(s.meta)) {
    retain(s);
    *out = s;
    return;
  }
  TzStr f = flat(s);
  if (n <= INLINE_MAX && !is_two(f.meta)) {
    *out = inline_piece((const uint8_t *)units(&f), str_len(f.meta), from, n);
  } else if (n < SHORT_LENGTH && !is_two(f.meta)) {
    tz_str_short_piece(out, &f, (const uint8_t *)units(&f), from, n);
  } else if (n < SHORT_LENGTH) {
    *out = string_from_units((const char *)units(&f) + (size_t)from * (is_two(f.meta) ? 2 : 1), n,
                             is_two(f.meta));
  } else {
    retain(f);
    *out = make_str(f.obj, make_meta(n, str_off(f.meta) + from, is_two(f.meta), is_ascii(f.meta),
                                     f.meta & F_HEAP));
  }
}

static inline TzStr substring_of(TzStr s, uint32_t from, uint32_t to) {
  TzStr piece;
  tz_str_substring_into(&piece, s.obj, s.meta, from, to);
  return piece;
}

// substring_of without a call when the string is one-byte and no rope and the
// piece is inline or shares the buffer: what the hot methods below inline. A
// short piece is cut from a word of eight units, the last eight when it ends
// there (a buffer has at least nine).
static inline void substring_fast(TzStr *out, void *obj, uint64_t meta, uint32_t from,
                                  uint32_t to) {
  uint32_t n = to - from, len = str_len(meta);
  if (direct_bytes(meta) && n != 0 && n != len) {
    uint64_t mask = ~0ull >> (64 - 8 * (n <= INLINE_MAX ? n : INLINE_MAX));
    if (is_inline(meta)) {
      *out = inline_from_bits(((uint64_t)(uintptr_t)obj >> (8 * from)) & mask, n);
      return;
    }
    const char *data = (const char *)obj + sizeof(Buffer) + str_off(meta);
    if (n <= INLINE_MAX && len >= 8) {
      uint32_t at = from + 8 <= len ? from : len - 8;
      uint64_t bits;
      memcpy(&bits, data + at, 8);
      *out = inline_from_bits((bits >> (8 * (from - at))) & mask, n);
      return;
    }
    if (n >= SHORT_LENGTH) {
      if (meta & F_HEAP)
        ++((Header *)obj)->rc;
      *out = make_str(obj, make_meta(n, str_off(meta) + from, 0, is_ascii(meta), meta & F_HEAP));
      return;
    }
  }
  tz_str_substring_into(out, obj, meta, from, to);
}

TzStr tz_str_sub(TzStr s, uint32_t from, uint32_t to) { return substring_of(s, from, to); }

const void *tz_str_units(TzStr *s, uint32_t *length, int *two) {
  *s = flat(*s);
  *length = str_len(s->meta);
  *two = is_two(s->meta);
  return units(s);
}

TZ_NOINLINE void tz_str_char_at_slow(TzStr *out, void *obj, uint64_t meta, uint32_t index) {
  tz_str_substring_into(out, obj, meta, index, index + 1);
}

// s[i]: the code unit at i as a string. One byte is an inline string.
TZ_HOT void tz_str_char_at(TzStr *out, void *obj, uint64_t meta, int64_t index) {
  check_index(index, str_len(meta));
  int32_t unit = direct_byte(obj, meta, (uint32_t)index);
  if (unit >= 0)
    *out = inline_from_bits((uint64_t)unit, 1);
  else
    tz_str_char_at_slow(out, obj, meta, (uint32_t)index);
}

TZ_NOINLINE int32_t tz_str_code_point_at_slow(TzStr *out, void *obj, uint64_t meta,
                                              int64_t index) {
  uint32_t len = str_len(meta);
  TzStr s = make_str(obj, meta);
  TzStr f = flat(s);
  uint32_t i = (uint32_t)index;
  uint16_t unit = unit_at(&f, i);
  if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < len) {
    uint16_t next = unit_at(&f, i + 1);
    if (next >= 0xDC00 && next <= 0xDFFF) {
      tz_str_substring_into(out, obj, meta, i, i + 2);
      return 2;
    }
  }
  tz_str_substring_into(out, obj, meta, i, i + 1);
  return 1;
}

// The code point at `index` for `for...of`: one code unit, or two for a
// surrogate pair. Returns the number of code units taken. A one-byte string
// has no surrogates.
TZ_HOT int32_t tz_str_code_point_at(TzStr *out, void *obj, uint64_t meta, int64_t index) {
  check_index(index, str_len(meta));
  int32_t unit = direct_byte(obj, meta, (uint32_t)index);
  if (unit < 0)
    return tz_str_code_point_at_slow(out, obj, meta, index);
  *out = inline_from_bits((uint64_t)unit, 1);
  return 1;
}

// JavaScript's slice: negative positions count from the end.
TZ_HOT void tz_str_slice(TzStr *out, void *obj, uint64_t meta, int64_t start, int64_t end,
                         int32_t has_end) {
  uint32_t len = str_len(meta);
  uint32_t from = start < 0 ? clamp(len + start, len) : clamp(start, len);
  uint32_t to = !has_end ? len : end < 0 ? clamp(len + end, len) : clamp(end, len);
  if (from < to)
    substring_fast(out, obj, meta, from, to);
  else
    *out = kEmpty;
}

// JavaScript's substring: negative positions become 0, and the ends swap if
// needed.
TZ_HOT void tz_str_substring(TzStr *out, void *obj, uint64_t meta, int64_t start, int64_t end,
                             int32_t has_end) {
  uint32_t len = str_len(meta);
  uint32_t a = clamp(start, len);
  uint32_t b = has_end ? clamp(end, len) : len;
  substring_fast(out, obj, meta, a < b ? a : b, a < b ? b : a);
}

void tz_str_repeat(TzStr *out, void *obj, uint64_t meta, int64_t count) {
  // RangeErrors, as in JavaScript; the program checks for them after the
  // call.
  if (count < 0) {
    tz_throw(TZ_RANGE_ERROR, "Invalid count value: %lld", (long long)count);
    *out = kEmpty;
    return;
  }
  uint32_t len = str_len(meta);
  if (count == 0 || len == 0) {
    *out = kEmpty;
    return;
  }
  uint64_t total = (uint64_t)len * (uint64_t)count;
  if (count > (int64_t)MAX_LENGTH || total > MAX_LENGTH) {
    tz_throw(TZ_RANGE_ERROR, "Invalid string length");
    *out = kEmpty;
    return;
  }
  if (count == 1) {
    TzStr s = make_str(obj, meta);
    retain(s);
    *out = s;
    return;
  }
  TzStr f = flat(make_str(obj, meta));
  int two = is_two(f.meta);
  if (!two && total <= INLINE_MAX) {
    uint64_t bits = 0;
    for (int64_t i = 0; i < count; ++i)
      bits = add_inline_units(bits, (uint32_t)i * len, f);
    *out = inline_from_bits(bits, (uint32_t)total);
    return;
  }
  size_t width = two ? 2 : 1;
  Buffer *b = buffer_new((uint32_t)total, two);
  char *dst = buffer_units(b);
  copy_units(dst, two, &f);
  // Double the filled part until it is long enough.
  size_t filled = (size_t)len * width, need = (size_t)total * width;
  while (filled < need) {
    size_t chunk = filled < need - filled ? filled : need - filled;
    memcpy(dst + filled, dst, chunk);
    filled += chunk;
  }
  b->used = (uint32_t)total;
  *out = make_str(b, make_meta((uint32_t)total, 0, two, is_ascii(f.meta), F_HEAP));
}

//===----------------------------------------------------------------------===//
// Conversions and printing
//===----------------------------------------------------------------------===//

void tz_str_from_i64(TzStr *out, int64_t value) {
  char buf[24];
  const char *text = format_i64(buf + sizeof buf, value);
  size_t len = (size_t)(buf + sizeof buf - text);
  if (len <= INLINE_MAX)
    *out = inline_str((const uint8_t *)text, (uint32_t)len);
  else
    *out = string_from_ascii(text, len);
}

void tz_str_from_f64(TzStr *out, double value) {
  char buf[64];
  *out = string_from_ascii(buf, tz_format_f64(value, buf));
}

// Writes code units to stdout as UTF-8. A lone surrogate, which UTF-8 cannot
// express, becomes U+FFFD, as in Node.js.
TZ_NOINLINE void tz_out_write_units(const void *data, uint32_t len, int two) {
  char buf[256];
  size_t n = 0;
  for (uint32_t i = 0; i < len; ++i) {
    if (n > sizeof buf - 4) {
      tz_out_write(buf, n);
      n = 0;
    }
    uint32_t cp = two ? ((const uint16_t *)data)[i] : ((const uint8_t *)data)[i];
    if (two && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len) {
      uint16_t low = ((const uint16_t *)data)[i + 1];
      if (low >= 0xDC00 && low <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        ++i;
      }
    }
    if (cp >= 0xD800 && cp <= 0xDFFF)
      cp = 0xFFFD;
    if (cp < 0x80) {
      buf[n++] = (char)cp;
    } else if (cp < 0x800) {
      buf[n++] = (char)(0xC0 | (cp >> 6));
      buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      buf[n++] = (char)(0xE0 | (cp >> 12));
      buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
      buf[n++] = (char)(0xF0 | (cp >> 18));
      buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
      buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      buf[n++] = (char)(0x80 | (cp & 0x3F));
    }
  }
  tz_out_write(buf, n);
}

void tz_out_write_utf16(const uint16_t *units, size_t length) {
  tz_out_write_units(units, (uint32_t)length, 1);
}

void tz_print_string(void *obj, uint64_t meta) {
  TzStr f = flat(make_str(obj, meta));
  uint32_t len = str_len(f.meta);
  if (len == 0)
    return;
  if (is_ascii(f.meta))
    tz_out_write((const char *)units(&f), len);
  else
    tz_out_write_units(units(&f), len, is_two(f.meta));
}

//===----------------------------------------------------------------------===//
// Building strings in C
//===----------------------------------------------------------------------===//

void tz_buf_init(TzBuf *b) {
  b->data = NULL;
  b->length = 0;
  b->capacity = 0;
}

void tz_buf_free(TzBuf *b) {
  tz_free(b->data);
  tz_buf_init(b);
}

static void buf_reserve(TzBuf *b, size_t extra) {
  if (b->length + extra <= b->capacity)
    return;
  size_t capacity = b->capacity + b->capacity / 2 + 16;
  if (capacity < b->length + extra)
    capacity = b->length + extra;
  uint16_t *data = (uint16_t *)tz_alloc(capacity * sizeof(uint16_t));
  if (b->length)
    memcpy(data, b->data, b->length * sizeof(uint16_t));
  tz_free(b->data);
  b->data = data;
  b->capacity = capacity;
}

void tz_buf_push(TzBuf *b, uint16_t unit) {
  buf_reserve(b, 1);
  b->data[b->length++] = unit;
}

void tz_buf_ascii_n(TzBuf *b, const char *text, size_t length) {
  buf_reserve(b, length);
  for (size_t i = 0; i < length; ++i)
    b->data[b->length++] = (uint8_t)text[i];
}

void tz_buf_ascii(TzBuf *b, const char *text) { tz_buf_ascii_n(b, text, strlen(text)); }

void tz_buf_bytes(TzBuf *b, const uint8_t *units8, size_t length) {
  buf_reserve(b, length);
  for (size_t i = 0; i < length; ++i)
    b->data[b->length + i] = units8[i];
  b->length += length;
}

void tz_buf_units(TzBuf *b, const uint16_t *units16, size_t length) {
  buf_reserve(b, length);
  if (length)
    memcpy(b->data + b->length, units16, length * sizeof(uint16_t));
  b->length += length;
}

void tz_buf_str(TzBuf *b, TzStr s) {
  TzStr f = flat(s);
  uint32_t len = str_len(f.meta);
  buf_reserve(b, len);
  copy_units(b->data + b->length, 1, &f);
  b->length += len;
}

TzStr tz_buf_take(TzBuf *b) {
  TzStr s = tz_str_from_utf16(b->data, b->length);
  tz_buf_free(b);
  return s;
}

//===----------------------------------------------------------------------===//
// Building a string of a known length
//===----------------------------------------------------------------------===//

// A flat string built from pieces whose total length is known up front:
// inline when it is one-byte and short enough, otherwise one buffer.
typedef struct {
  Buffer *b; // NULL while inline
  uint64_t bits;
  uint32_t pos;
  int two;
} Builder;

static void builder_init(Builder *w, uint32_t total, int two) {
  w->b = !two && total <= INLINE_MAX ? NULL : buffer_new(total, two);
  w->bits = 0;
  w->pos = 0;
  w->two = two;
}

// Appends `n` code units of flat `s` from `from` on. A two-byte `s` needs a
// two-byte builder.
static void builder_put(Builder *w, const TzStr *s, uint32_t from, uint32_t n) {
  if (n == 0)
    return;
  if (!w->b) {
    for (uint32_t i = 0; i < n; ++i)
      w->bits |= (uint64_t)(uint8_t)unit_at(s, from + i) << (8 * (w->pos + i));
  } else {
    char *dst = buffer_units(w->b) + (size_t)w->pos * (w->two ? 2 : 1);
    const char *src = (const char *)units(s);
    if (is_two(s->meta) == w->two) {
      size_t width = w->two ? 2 : 1;
      memcpy(dst, src + (size_t)from * width, (size_t)n * width);
    } else {
      for (uint32_t i = 0; i < n; ++i)
        ((uint16_t *)dst)[i] = ((const uint8_t *)src)[from + i];
    }
  }
  w->pos += n;
}

static TzStr builder_finish(Builder *w, int ascii) {
  if (!w->b)
    return w->pos == 0 ? kEmpty : inline_from_bits(w->bits, w->pos);
  w->b->used = w->pos;
  return make_str(w->b, make_meta(w->pos, 0, w->two, ascii, F_HEAP));
}

// Appends code units of flat `s` to a growing buffer.
static void buf_put(TzBuf *b, const TzStr *s, uint32_t from, uint32_t n) {
  if (n == 0)
    return;
  buf_reserve(b, n);
  const void *u = units(s);
  for (uint32_t i = 0; i < n; ++i)
    b->data[b->length + i] =
        is_two(s->meta) ? ((const uint16_t *)u)[from + i] : ((const uint8_t *)u)[from + i];
  b->length += n;
}

static void buf_code_point(TzBuf *b, uint32_t cp) {
  if (cp < 0x10000) {
    tz_buf_push(b, (uint16_t)cp);
  } else {
    cp -= 0x10000;
    tz_buf_push(b, (uint16_t)(0xD800 + (cp >> 10)));
    tz_buf_push(b, (uint16_t)(0xDC00 + (cp & 0x3FF)));
  }
}

// The code point at `*i` of flat `s` (a surrogate pair, or one code unit),
// advancing `*i` past it.
static uint32_t next_code_point(const TzStr *s, uint32_t *i, uint32_t len) {
  uint32_t unit = unit_at(s, *i);
  ++*i;
  if (unit >= 0xD800 && unit <= 0xDBFF && *i < len) {
    uint32_t low = unit_at(s, *i);
    if (low >= 0xDC00 && low <= 0xDFFF) {
      ++*i;
      return 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
    }
  }
  return unit;
}

// The code point that ends before `*i`, moving `*i` back to its start.
static uint32_t previous_code_point(const TzStr *s, uint32_t *i) {
  uint32_t unit = unit_at(s, --*i);
  if (unit >= 0xDC00 && unit <= 0xDFFF && *i > 0) {
    uint32_t high = unit_at(s, *i - 1);
    if (high >= 0xD800 && high <= 0xDBFF) {
      --*i;
      return 0x10000 + ((high - 0xD800) << 10) + (unit - 0xDC00);
    }
  }
  return unit;
}

//===----------------------------------------------------------------------===//
// More methods
//===----------------------------------------------------------------------===//

// charAt(): "" out of range.
void tz_str_char_at_or_empty(TzStr *out, void *obj, uint64_t meta, int64_t index) {
  if (index < 0 || index >= (int64_t)str_len(meta)) {
    *out = kEmpty;
    return;
  }
  tz_str_char_at(out, obj, meta, index);
}

TZ_NOINLINE int32_t tz_str_code_point_slow(void *obj, uint64_t meta, uint32_t index) {
  TzStr f = flat(make_str(obj, meta));
  return (int32_t)next_code_point(&f, &index, str_len(meta));
}

// codePointAt(), for a position the compiler checked.
TZ_HOT int32_t tz_str_code_point(void *obj, uint64_t meta, int64_t index) {
  check_index(index, str_len(meta));
  int32_t unit = direct_byte(obj, meta, (uint32_t)index);
  return unit >= 0 ? unit : tz_str_code_point_slow(obj, meta, (uint32_t)index);
}

// lastIndexOf(): the last match that starts at or before `position`.
int32_t tz_str_last_index_of(void *s_obj, uint64_t s_meta, void *t_obj, uint64_t t_meta,
                             int64_t position, int32_t has_position) {
  TzStr s = flat(make_str(s_obj, s_meta)), t = flat(make_str(t_obj, t_meta));
  uint32_t len = str_len(s.meta), tlen = str_len(t.meta);
  if (tlen > len)
    return -1;
  uint32_t start = has_position ? clamp(position, len) : len;
  int64_t k = start < len - tlen ? start : len - tlen;
  if (tlen == 0)
    return (int32_t)k;
  if (!is_two(s.meta) && !is_two(t.meta)) {
    const uint8_t *su = (const uint8_t *)units(&s), *tu = (const uint8_t *)units(&t);
    for (; k >= 0; --k)
      if (su[k] == tu[0] && bytes_equal(su + k + 1, tu + 1, tlen - 1))
        return (int32_t)k;
    return -1;
  }
  uint16_t first = unit_at(&t, 0);
  for (; k >= 0; --k)
    if (unit_at(&s, (uint32_t)k) == first && tz_str_units_equal(&s, (uint32_t)k, &t, 0, tlen))
      return (int32_t)k;
  return -1;
}

// The pieces of split_bytes: data[p..q) of flat one-byte `f`. A slice is
// not retained here; `*slices` counts them, for the caller to retain `f`
// once for all of them.
static inline void split_piece(TzStr *out, const TzStr *f, const uint8_t *data, uint32_t len,
                               uint32_t p, uint32_t q, uint32_t *slices) {
  uint32_t n = q - p;
  if (n - 1 < INLINE_MAX) { // 1 to INLINE_MAX
    *out = inline_piece(data, len, p, n);
  } else if (n >= SHORT_LENGTH) {
    ++*slices;
    *out = make_str(f->obj, make_meta(n, str_off(f->meta) + p, 0, is_ascii(f->meta),
                                      f->meta & F_HEAP));
  } else if (n == 0) {
    *out = kEmpty;
  } else {
    tz_str_short_piece(out, f, data, p, n);
  }
}

// split() of a one-byte string by a one-byte code unit: the usual case. The
// separators are counted first, so the array is made once at its final size
// and the pieces are written straight into it; then the bits of sixteen
// bytes at a time give the separators.
static TzArray *split_bytes(const TzType *string_type, const TzStr *f, uint8_t c, uint32_t max) {
  const uint8_t *data = (const uint8_t *)units(f);
  uint32_t len = str_len(f->meta);
  // n - 1 separators end pieces; the last piece ends at the end, or at the
  // separator after it when the limit cuts the rest off.
  uint32_t n = count_byte(data, len, c, max - 1) + 1;
  TzArray *a = tz_arr_new(string_type, 0, n);
  TzStr *out = (TzStr *)a->data;
  uint32_t k = 0, p = 0, slices = 0;
  if (n == max) { // perhaps fewer pieces than separators
    for (; k + 1 < n; ++k) {
      uint32_t q = find_byte(data, p, len, c);
      split_piece(&out[k], f, data, len, p, q, &slices);
      p = q + 1;
    }
  } else {
    uint32_t b = 0;
#ifdef TZ_SSE2
    // Every separator ends a piece. Each piece is first written as if it
    // were short, its first eight bytes loaded at once (they are there while
    // eight bytes follow the block); those that are not short are noted and
    // redone in batches. That keeps the branches that the processor could
    // not predict out of the loop.
    struct {
      uint32_t k, p, n;
    } redo[64];
    uint32_t redos = 0;
    __m128i needle = _mm_set1_epi8((char)c);
    for (; b + 24 <= len; b += 16) {
      __m128i block = _mm_loadu_si128((const __m128i *)(data + b));
      unsigned mask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(block, needle));
      for (; mask; mask &= mask - 1) {
        uint32_t q = b + (uint32_t)__builtin_ctz(mask), piece = q - p;
        uint64_t bits;
        memcpy(&bits, data + p, 8);
        bits &= piece >= 8 ? ~0ull : (1ull << (8 * piece)) - 1;
        out[k].obj = (void *)(uintptr_t)bits;
        out[k].meta =
            (uint64_t)piece | F_ROPE | ((bits & 0x8080808080808080ull) == 0 ? F_ASCII : 0);
        redo[redos].k = k;
        redo[redos].p = p;
        redo[redos].n = piece;
        redos += piece - 1 >= INLINE_MAX; // empty, or too long to be inline
        if (redos == 64) {
          for (uint32_t i = 0; i < redos; ++i)
            split_piece(&out[redo[i].k], f, data, len, redo[i].p, redo[i].p + redo[i].n,
                        &slices);
          redos = 0;
        }
        ++k;
        p = q + 1;
      }
    }
    for (uint32_t i = 0; i < redos; ++i)
      split_piece(&out[redo[i].k], f, data, len, redo[i].p, redo[i].p + redo[i].n, &slices);
#endif
    for (; b < len; ++b) {
      if (data[b] == c) {
        split_piece(&out[k++], f, data, len, p, b, &slices);
        p = b + 1;
      }
    }
  }
  split_piece(&out[k], f, data, len, p, find_byte(data, p, len, c), &slices);
  if (f->meta & F_HEAP)
    ((Header *)f->obj)->rc += slices;
  a->length = n;
  return a;
}

// split(separator, limit): the pieces between the separators, at most
// `limit` of them; an empty separator splits into code units.
TzArray *tz_str_split(const TzType *string_type, void *obj, uint64_t meta, void *sep_obj,
                      uint64_t sep_meta, int32_t has_sep, int64_t limit, int32_t has_limit) {
  uint32_t max = has_limit ? (uint32_t)limit : UINT32_MAX; // ToUint32, as in JavaScript
  if (max == 0)
    return tz_arr_new(string_type, 0, 0);
  TzStr s = make_str(obj, meta);
  if (!has_sep) {
    TzArray *a = tz_arr_new(string_type, 1, 1);
    retain(s);
    *(TzStr *)a->data = s;
    return a;
  }
  TzStr f = flat(s), sep = flat(make_str(sep_obj, sep_meta));
  uint32_t len = str_len(f.meta), sep_len = str_len(sep.meta);
  if (sep_len == 0) {
    // Each code unit, a short string unless it is above 0xFF.
    uint32_t n = len < max ? len : max;
    TzArray *a = tz_arr_new(string_type, 0, n);
    TzStr *out = (TzStr *)a->data;
    if (is_two(f.meta)) {
      for (uint32_t i = 0; i < n; ++i)
        tz_str_substring_into(&out[i], f.obj, f.meta, i, i + 1);
    } else {
      const uint8_t *data = (const uint8_t *)units(&f);
      for (uint32_t i = 0; i < n; ++i)
        out[i] = inline_from_bits(data[i], 1);
    }
    a->length = n;
    return a;
  }
  if (sep_len == 1 && !is_two(f.meta) && unit_at(&sep, 0) <= 0xFF)
    return split_bytes(string_type, &f, (uint8_t)unit_at(&sep, 0), max);

  // Any other separator: the matches are found twice, once to count them.
  uint32_t n = 1;
  for (int64_t q = 0; n < max && (q = index_of(&f, &sep, (uint32_t)q)) >= 0; q += sep_len)
    ++n;
  TzArray *a = tz_arr_new(string_type, 0, n);
  TzStr *out = (TzStr *)a->data;
  uint32_t p = 0;
  for (uint32_t k = 0; k < n; ++k) {
    int64_t q = index_of(&f, &sep, p);
    uint32_t end = q < 0 ? len : (uint32_t)q;
    tz_str_substring_into(&out[k], f.obj, f.meta, p, end);
    p = end + sep_len;
  }
  a->length = n;
  return a;
}

// `[...s]`: appends the code points of s to `a`, each as a string, as the
// string's iterator gives them.
void tz_arr_append_code_points(TzArray *a, void *obj, uint64_t meta) {
  TzStr s = make_str(obj, meta), f = flat(s);
  uint32_t len = str_len(meta);
  for (uint32_t i = 0; i < len;) {
    uint32_t start = i;
    next_code_point(&f, &i, len);
    TzStr piece = substring_of(s, start, i);
    tz_arr_push_value(a, &piece);
  }
}

// Whitespace as JavaScript's trim() and Number() see it, ASCII first.
static inline int is_space(uint32_t unit) {
  return unit == ' ' || (unit >= 9 && unit <= 13) || (unit >= 0x80 && tz_is_js_space(unit));
}

TZ_NOINLINE void tz_str_trim_slow(TzStr *out, void *obj, uint64_t meta, int32_t ends) {
  TzStr f = flat(make_str(obj, meta));
  uint32_t start = 0, end = str_len(f.meta);
  if (ends & 1)
    while (start < end && is_space(unit_at(&f, start)))
      ++start;
  if (ends & 2)
    while (end > start && is_space(unit_at(&f, end - 1)))
      --end;
  tz_str_substring_into(out, obj, meta, start, end);
}

// trim() (ends 3), trimStart() (1) and trimEnd() (2).
TZ_HOT void tz_str_trim(TzStr *out, void *obj, uint64_t meta, int32_t ends) {
  if (!direct_bytes(meta)) {
    tz_str_trim_slow(out, obj, meta, ends);
    return;
  }
  uint32_t start = 0, end = str_len(meta);
  if (ends & 1)
    while (start < end && is_space((uint32_t)direct_byte(obj, meta, start)))
      ++start;
  if (ends & 2)
    while (end > start && is_space((uint32_t)direct_byte(obj, meta, end - 1)))
      --end;
  substring_fast(out, obj, meta, start, end);
}

// padStart() and padEnd(): `fill` (by default a space) repeated, and cut, up
// to `length` code units in all.
void tz_str_pad(TzStr *out, void *obj, uint64_t meta, int64_t length, void *fill_obj,
                uint64_t fill_meta, int32_t has_fill, int32_t at_start) {
  TzStr s = make_str(obj, meta);
  TzStr fill = has_fill ? flat(make_str(fill_obj, fill_meta)) : single_char(' ');
  uint32_t len = str_len(meta), fill_len = str_len(fill.meta);
  if (length <= (int64_t)len || fill_len == 0) {
    retain(s);
    *out = s;
    return;
  }
  if (length > (int64_t)MAX_LENGTH) {
    tz_throw(TZ_RANGE_ERROR, "Invalid string length");
    *out = kEmpty;
    return;
  }
  TzStr f = flat(s);
  uint32_t total = (uint32_t)length, gap = total - len;
  Builder w;
  builder_init(&w, total, is_two(f.meta) || is_two(fill.meta));
  if (!at_start)
    builder_put(&w, &f, 0, len);
  for (uint32_t done = 0; done < gap; done += fill_len)
    builder_put(&w, &fill, 0, gap - done < fill_len ? gap - done : fill_len);
  if (at_start)
    builder_put(&w, &f, 0, len);
  *out = builder_finish(&w, is_ascii(f.meta) && is_ascii(fill.meta));
}

// Unicode's Final_Sigma: the sigma at `i` ends a word if a cased letter comes
// before it and none after it, not counting case-ignorable ones.
static int final_sigma(const TzStr *f, uint32_t i, uint32_t len) {
  int preceded = 0;
  for (uint32_t j = i; j > 0;) {
    uint32_t cp = previous_code_point(f, &j);
    if (!tz_is_case_ignorable(cp)) {
      preceded = tz_is_cased(cp);
      break;
    }
  }
  if (!preceded)
    return 0;
  for (uint32_t j = i + 1; j < len;) {
    uint32_t cp = next_code_point(f, &j, len);
    if (!tz_is_case_ignorable(cp))
      return !tz_is_cased(cp);
  }
  return 1;
}

// Whether toUpperCase() (`upper`) or toLowerCase() maps a Latin-1 unit to
// the one 0x20 away: the ASCII letters, and the Latin-1 ones but × and ÷.
static inline int flips_case(uint8_t c, int upper) {
  if (upper)
    return (c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7);
  return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7);
}

// toUpperCase() and toLowerCase() of a one-byte string whose result is
// one-byte too, which is every toLowerCase(); toUpperCase() leaves Latin-1
// only for µ, ß and ÿ, and then this returns 0.
static int latin1_case(TzStr *out, const TzStr *s, const TzStr *f, int upper) {
  uint32_t len = str_len(f->meta);
  const uint8_t *u = (const uint8_t *)units(f);
  uint32_t first = len; // the first unit that changes
  for (uint32_t i = 0; i < len; ++i) {
    if (upper && (u[i] == 0xB5 || u[i] == 0xDF || u[i] == 0xFF))
      return 0;
    if (first == len && flips_case(u[i], upper))
      first = i;
  }
  if (first == len) {
    retain(*s);
    *out = *s;
    return 1;
  }
  Builder w;
  builder_init(&w, len, 0);
  builder_put(&w, f, 0, len);
  uint8_t *dst = w.b ? (uint8_t *)buffer_units(w.b) : (uint8_t *)&w.bits;
  for (uint32_t i = first; i < len; ++i)
    if (flips_case(dst[i], upper))
      dst[i] ^= 0x20;
  *out = builder_finish(&w, 0);
  return 1;
}

// toUpperCase() and toLowerCase(), with the full mappings of Unicode, as V8
// has them: "ß" becomes "SS", and a final capital sigma a final small one.
static void convert_case(TzStr *out, void *obj, uint64_t meta, int upper) {
  TzStr s = make_str(obj, meta);
  TzStr f = flat(s);
  uint32_t len = str_len(f.meta);
  if (is_ascii(f.meta) && is_inline(f.meta)) {
    // The letters of all eight bytes at once: adding a constant to a byte
    // sets its top bit from the first letter on, another constant from just
    // past the last one, and a letter flips 0x20.
    uint64_t bits = inline_bits(&f);
    uint64_t from = upper ? 0x1F1F1F1F1F1F1F1Full : 0x3F3F3F3F3F3F3F3Full;
    uint64_t past = upper ? 0x0505050505050505ull : 0x2525252525252525ull;
    uint64_t letters = (bits + from) & ~(bits + past) & 0x8080808080808080ull;
    *out = inline_from_bits(bits ^ (letters >> 2), len);
    return;
  }
  if (!is_two(f.meta) && !is_ascii(f.meta) && latin1_case(out, &s, &f, upper))
    return;
  if (is_ascii(f.meta)) {
    const uint8_t *u = (const uint8_t *)units(&f);
    uint8_t from = upper ? 'a' : 'A';
    uint32_t i = 0;
    while (i < len && (uint8_t)(u[i] - from) > 25)
      ++i;
    if (i == len) {
      retain(s);
      *out = s;
      return;
    }
    Builder w;
    builder_init(&w, len, 0);
    builder_put(&w, &f, 0, len);
    uint8_t *dst = w.b ? (uint8_t *)buffer_units(w.b) : (uint8_t *)&w.bits;
    for (; i < len; ++i)
      if ((uint8_t)(dst[i] - from) <= 25)
        dst[i] ^= 0x20;
    *out = builder_finish(&w, 1);
    return;
  }
  TzBuf b;
  tz_buf_init(&b);
  int changed = 0;
  for (uint32_t i = 0; i < len;) {
    uint32_t at = i;
    uint32_t cp = next_code_point(&f, &i, len);
    uint32_t mapped[3];
    int count;
    if (!upper && cp == 0x3A3) {
      mapped[0] = final_sigma(&f, at, len) ? 0x3C2 : 0x3C3;
      count = 1;
    } else {
      count = tz_case_map(cp, upper, mapped);
    }
    changed |= count != 1 || mapped[0] != cp;
    for (int k = 0; k < count; ++k)
      buf_code_point(&b, mapped[k]);
  }
  if (!changed) {
    tz_buf_free(&b);
    retain(s);
    *out = s;
    return;
  }
  *out = tz_buf_take(&b);
}

void tz_str_to_upper(TzStr *out, void *obj, uint64_t meta) { convert_case(out, obj, meta, 1); }

void tz_str_to_lower(TzStr *out, void *obj, uint64_t meta) { convert_case(out, obj, meta, 0); }

// The text that replaces a match, with the patterns of JavaScript: "$$" is
// "$", "$&" the match, "$`" what comes before it and "$'" what follows it.
static void put_replacement(TzBuf *b, const TzStr *replacement, const TzStr *s, uint32_t at,
                            uint32_t match_len) {
  uint32_t len = str_len(replacement->meta), s_len = str_len(s->meta);
  uint32_t plain = 0; // where the text not copied yet starts
  for (uint32_t i = 0; i + 1 < len; ++i) {
    if (unit_at(replacement, i) != '$')
      continue;
    uint16_t next = unit_at(replacement, i + 1);
    if (next != '$' && next != '&' && next != '`' && next != '\'')
      continue;
    buf_put(b, replacement, plain, i - plain);
    if (next == '$')
      tz_buf_push(b, '$');
    else if (next == '&')
      buf_put(b, s, at, match_len);
    else if (next == '`')
      buf_put(b, s, 0, at);
    else
      buf_put(b, s, at + match_len, s_len - at - match_len);
    plain = i + 2;
    ++i;
  }
  buf_put(b, replacement, plain, len - plain);
}

// tz_str_replace of one-byte `f` and `replacement` when the replacement has
// no patterns ($): the matches are counted first, and the result is made at
// its final size, one byte per unit. `at` is the first match. Returns 0 if
// it does not apply.
static int replace_plain(TzStr *out, const TzStr *f, const TzStr *pattern,
                         const TzStr *replacement, int64_t at, int all) {
  uint32_t len = str_len(f->meta), p_len = str_len(pattern->meta);
  uint32_t r_len = str_len(replacement->meta);
  const uint8_t *r = (const uint8_t *)units(replacement);
  if (r_len && find_byte(r, 0, r_len, '$') != r_len)
    return 0;
  // An empty pattern matches between every two code units, and at both ends.
  uint64_t count = 1;
  if (all) {
    if (p_len == 0)
      count = (uint64_t)len + 1;
    else
      for (int64_t q = at + p_len; (q = index_of(f, pattern, (uint32_t)q)) >= 0; q += p_len)
        ++count;
  }
  uint64_t total = len + count * r_len - count * p_len;
  if (total > MAX_LENGTH)
    tz_panicf("string length %llu exceeds the maximum of %llu", (unsigned long long)total,
              (unsigned long long)MAX_LENGTH);
  Builder w;
  builder_init(&w, (uint32_t)total, 0);
  uint32_t copied = 0;
  for (uint64_t k = 0; k < count; ++k) {
    builder_put(&w, f, copied, (uint32_t)at - copied);
    builder_put(&w, replacement, 0, r_len);
    copied = (uint32_t)at + p_len;
    if (k + 1 < count)
      at = p_len ? index_of(f, pattern, copied) : at + 1;
  }
  builder_put(&w, f, copied, len - copied);
  *out = builder_finish(&w, is_ascii(f->meta) && is_ascii(replacement->meta));
  return 1;
}

// replace() (the first match) and replaceAll() of a string pattern.
void tz_str_replace(TzStr *out, void *s_obj, uint64_t s_meta, void *p_obj, uint64_t p_meta,
                    void *r_obj, uint64_t r_meta, int32_t all) {
  TzStr s = make_str(s_obj, s_meta);
  TzStr f = flat(s), pattern = flat(make_str(p_obj, p_meta)),
        replacement = flat(make_str(r_obj, r_meta));
  uint32_t len = str_len(f.meta), p_len = str_len(pattern.meta);
  int64_t at = index_of(&f, &pattern, 0);
  if (at < 0) {
    retain(s);
    *out = s;
    return;
  }
  if (!is_two(f.meta) && !is_two(replacement.meta) &&
      replace_plain(out, &f, &pattern, &replacement, at, all))
    return;
  TzBuf b;
  tz_buf_init(&b);
  uint32_t copied = 0;
  while (at >= 0) {
    buf_put(&b, &f, copied, (uint32_t)at - copied);
    put_replacement(&b, &replacement, &f, (uint32_t)at, p_len);
    copied = (uint32_t)at + p_len;
    if (!all)
      break;
    // An empty pattern matches between every two code units.
    uint32_t from = (uint32_t)at + (p_len ? p_len : 1);
    at = from <= len ? index_of(&f, &pattern, from) : -1;
  }
  buf_put(&b, &f, copied, len - copied);
  *out = tz_buf_take(&b);
}

// String.fromCharCode(): each code is taken modulo 2^16.
void tz_str_from_char_codes(TzStr *out, const int32_t *codes, int64_t n) {
  TzBuf b;
  tz_buf_init(&b);
  for (int64_t i = 0; i < n; ++i)
    tz_buf_push(&b, (uint16_t)codes[i]);
  *out = tz_buf_take(&b);
}

// String.fromCodePoint(): a code point that is not an integer from 0 to
// 0x10FFFF is a RangeError.
void tz_str_from_code_points(TzStr *out, const double *code_points, int64_t n) {
  TzBuf b;
  tz_buf_init(&b);
  for (int64_t i = 0; i < n; ++i) {
    double cp = code_points[i];
    if (!(cp >= 0 && cp <= 0x10FFFF && cp == (double)(uint32_t)cp)) {
      char text[32];
      text[tz_format_f64(cp, text)] = 0;
      tz_throw(TZ_RANGE_ERROR, "Invalid code point %s", text);
      tz_buf_free(&b);
      *out = tz_str_from_ascii("", 0);
      return;
    }
    buf_code_point(&b, (uint32_t)cp);
  }
  *out = tz_buf_take(&b);
}

//===----------------------------------------------------------------------===//
// Numbers from strings (number.c does the parsing)
//===----------------------------------------------------------------------===//

// The ASCII characters of flat `*f` from `from` to `to`, up to the first
// other one; `*n` gets their count. A one-byte string lends its own units,
// read from `*f`, which must stay where it is meanwhile. A two-byte one is
// copied to `local`, or to a new block that `*owned` gets for the caller to
// free.
static const char *ascii_text(const TzStr *f, uint32_t from, uint32_t to, char *local, size_t size,
                              size_t *n, char **owned) {
  *owned = NULL;
  if (!is_two(f->meta)) {
    const char *text = from < to ? (const char *)units(f) + from : "";
    size_t k = 0;
    while (k < to - from && (uint8_t)text[k] < 0x80)
      ++k;
    *n = k;
    return text;
  }
  char *text = local;
  if ((size_t)(to - from) >= size)
    text = *owned = (char *)tz_alloc((size_t)(to - from) + 1);
  size_t k = 0;
  for (uint32_t i = from; i < to; ++i) {
    uint16_t unit = unit_at(f, i);
    if (unit >= 0x80)
      break;
    text[k++] = (char)unit;
  }
  text[k] = 0;
  *n = k;
  return text;
}

// Number(s) and `+s`: the whole string, without the whitespace around it,
// must be a number; the empty string is 0.
double tz_str_to_number(void *obj, uint64_t meta) {
  TzStr f = flat(make_str(obj, meta));
  uint32_t start = 0, end = str_len(f.meta);
  while (start < end && is_space(unit_at(&f, start)))
    ++start;
  while (end > start && is_space(unit_at(&f, end - 1)))
    --end;
  char local[64], *owned;
  size_t n;
  const char *text = ascii_text(&f, start, end, local, sizeof local, &n, &owned);
  double value = tz_number_from_ascii(text, n, n == end - start);
  tz_free(owned);
  return value;
}

// The ASCII text after the leading whitespace of flat `*f`, which parseInt
// and parseFloat read a number from the start of.
static const char *leading_text(const TzStr *f, char *local, size_t size, size_t *n,
                                char **owned) {
  uint32_t start = 0, end = str_len(f->meta);
  while (start < end && is_space(unit_at(f, start)))
    ++start;
  return ascii_text(f, start, end, local, size, n, owned);
}

// parseInt(s, radix); a radix of 0 means none, as in JavaScript.
double tz_parse_int(void *obj, uint64_t meta, int32_t radix) {
  TzStr f = flat(make_str(obj, meta));
  char local[64], *owned;
  size_t n;
  const char *text = leading_text(&f, local, sizeof local, &n, &owned);
  double value = tz_parse_int_ascii(text, n, radix);
  tz_free(owned);
  return value;
}

double tz_parse_float(void *obj, uint64_t meta) {
  TzStr f = flat(make_str(obj, meta));
  char local[64], *owned;
  size_t n;
  const char *text = leading_text(&f, local, sizeof local, &n, &owned);
  double value = tz_parse_float_ascii(text, n);
  tz_free(owned);
  return value;
}
