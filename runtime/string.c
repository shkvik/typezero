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
static inline TzStr flat(TzStr s) {
  if (!is_rope(s.meta))
    return s;
  Rope *r = (Rope *)s.obj;
  if (!r->flat.obj)
    tz_str_flatten(r, s.meta);
  return r->flat;
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
  size_t width = two ? 2 : 1;
  for (int64_t i = 0; i < n; ++i) {
    if (i > 0 && sep_len) {
      copy_units(dst + (size_t)pos * width, two, &separator);
      pos += sep_len;
    }
    TzStr f = flat(items[i]);
    copy_units(dst + (size_t)pos * width, two, &f);
    pos += str_len(f.meta);
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

// Orders strings by UTF-16 code units, as JavaScript's `<` does.
int32_t tz_str_compare(void *a_obj, uint64_t a_meta, void *b_obj, uint64_t b_meta) {
  TzStr a = flat(make_str(a_obj, a_meta)), b = flat(make_str(b_obj, b_meta));
  uint32_t la = str_len(a.meta), lb = str_len(b.meta);
  uint32_t n = la < lb ? la : lb;
  for (uint32_t i = 0; i < n; ++i) {
    uint16_t x = unit_at(&a, i), y = unit_at(&b, i);
    if (x != y)
      return x < y ? -1 : 1;
  }
  return la == lb ? 0 : la < lb ? -1 : 1;
}

static uint32_t clamp(int64_t value, uint32_t len) {
  return value < 0 ? 0 : value > (int64_t)len ? len : (uint32_t)value;
}

// Like JavaScript's indexOf: the first match at or after `from`, or -1.
static int64_t index_of(const TzStr *s, const TzStr *t, uint32_t from) {
  uint32_t len = str_len(s->meta), tlen = str_len(t->meta);
  if (tlen == 0)
    return from;
  if (tlen > len)
    return -1;
  uint16_t first = unit_at(t, 0);
  for (uint32_t i = from; i + tlen <= len; ++i)
    if (unit_at(s, i) == first && tz_str_units_equal(s, i + 1, t, 1, tlen - 1))
      return i;
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

TZ_HOT int32_t tz_str_char_code_at(void *obj, uint64_t meta, int64_t index) {
  check_index(index, str_len(meta));
  TzStr f = flat(make_str(obj, meta));
  return unit_at(&f, (uint32_t)index);
}

// s[from..to): short pieces are copied, longer ones share the buffer.
static TzStr substring_of(TzStr s, uint32_t from, uint32_t to) {
  uint32_t n = to - from;
  if (n == 0)
    return kEmpty;
  if (n == str_len(s.meta)) {
    retain(s);
    return s;
  }
  TzStr f = flat(s);
  if (n < SHORT_LENGTH)
    return string_from_units((const char *)units(&f) + (size_t)from * (is_two(f.meta) ? 2 : 1), n,
                             is_two(f.meta));
  retain(f);
  return make_str(f.obj, make_meta(n, str_off(f.meta) + from, is_two(f.meta), is_ascii(f.meta),
                                   f.meta & F_HEAP));
}

void tz_str_char_at(TzStr *out, void *obj, uint64_t meta, int64_t index) {
  check_index(index, str_len(meta));
  *out = substring_of(make_str(obj, meta), (uint32_t)index, (uint32_t)index + 1);
}

// The code point at `index` for `for...of`: one code unit, or two for a
// surrogate pair. Returns the number of code units taken.
int32_t tz_str_code_point_at(TzStr *out, void *obj, uint64_t meta, int64_t index) {
  uint32_t len = str_len(meta);
  check_index(index, len);
  TzStr s = make_str(obj, meta);
  TzStr f = flat(s);
  uint32_t i = (uint32_t)index;
  uint16_t unit = unit_at(&f, i);
  if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < len) {
    uint16_t next = unit_at(&f, i + 1);
    if (next >= 0xDC00 && next <= 0xDFFF) {
      *out = substring_of(s, i, i + 2);
      return 2;
    }
  }
  *out = substring_of(s, i, i + 1);
  return 1;
}

// JavaScript's slice: negative positions count from the end.
void tz_str_slice(TzStr *out, void *obj, uint64_t meta, int64_t start, int64_t end,
                  int32_t has_end) {
  uint32_t len = str_len(meta);
  uint32_t from = start < 0 ? clamp(len + start, len) : clamp(start, len);
  uint32_t to = !has_end ? len : end < 0 ? clamp(len + end, len) : clamp(end, len);
  *out = from < to ? substring_of(make_str(obj, meta), from, to) : kEmpty;
}

// JavaScript's substring: negative positions become 0, and the ends swap if
// needed.
void tz_str_substring(TzStr *out, void *obj, uint64_t meta, int64_t start, int64_t end,
                      int32_t has_end) {
  uint32_t len = str_len(meta);
  uint32_t a = clamp(start, len);
  uint32_t b = has_end ? clamp(end, len) : len;
  *out = substring_of(make_str(obj, meta), a < b ? a : b, a < b ? b : a);
}

void tz_str_repeat(TzStr *out, void *obj, uint64_t meta, int64_t count) {
  if (count < 0)
    tz_panicf("invalid repeat count %lld", (long long)count);
  uint32_t len = str_len(meta);
  if (count == 0 || len == 0) {
    *out = kEmpty;
    return;
  }
  uint64_t total = (uint64_t)len * (uint64_t)count;
  if (count > (int64_t)MAX_LENGTH || total > MAX_LENGTH)
    tz_panicf("repeated string would exceed the maximum length of %llu",
              (unsigned long long)MAX_LENGTH);
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
  *out = string_from_ascii(text, (size_t)(buf + sizeof buf - text));
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
  *out = substring_of(make_str(obj, meta), (uint32_t)index, (uint32_t)index + 1);
}

// codePointAt(), for a position the compiler checked.
TZ_HOT int32_t tz_str_code_point(void *obj, uint64_t meta, int64_t index) {
  uint32_t len = str_len(meta);
  check_index(index, len);
  TzStr f = flat(make_str(obj, meta));
  uint32_t i = (uint32_t)index;
  return (int32_t)next_code_point(&f, &i, len);
}

// lastIndexOf(): the last match that starts at or before `position`.
int32_t tz_str_last_index_of(void *s_obj, uint64_t s_meta, void *t_obj, uint64_t t_meta,
                             int64_t position, int32_t has_position) {
  TzStr s = flat(make_str(s_obj, s_meta)), t = flat(make_str(t_obj, t_meta));
  uint32_t len = str_len(s.meta), tlen = str_len(t.meta);
  if (tlen > len)
    return -1;
  uint32_t start = has_position ? clamp(position, len) : len;
  for (int64_t k = start < len - tlen ? start : len - tlen; k >= 0; --k)
    if (tz_str_units_equal(&s, (uint32_t)k, &t, 0, tlen))
      return (int32_t)k;
  return -1;
}

// split(separator, limit): the pieces between the separators, at most
// `limit` of them; an empty separator splits into code units.
TzArray *tz_str_split(const TzType *string_type, void *obj, uint64_t meta, void *sep_obj,
                      uint64_t sep_meta, int32_t has_sep, int64_t limit, int32_t has_limit) {
  uint32_t max = has_limit ? (uint32_t)limit : UINT32_MAX; // ToUint32, as in JavaScript
  TzArray *a = tz_arr_new(string_type, 0, 0);
  if (max == 0)
    return a;
  TzStr s = make_str(obj, meta), piece;
  if (!has_sep) {
    retain(s);
    tz_arr_push_value(a, &s);
    return a;
  }
  TzStr f = flat(s), sep = flat(make_str(sep_obj, sep_meta));
  uint32_t len = str_len(f.meta), sep_len = str_len(sep.meta);
  if (sep_len == 0) {
    for (uint32_t i = 0; i < len && a->length < max; ++i) {
      piece = substring_of(s, i, i + 1);
      tz_arr_push_value(a, &piece);
    }
    return a;
  }
  uint32_t p = 0;
  for (int64_t q; (q = index_of(&f, &sep, p)) >= 0; p = (uint32_t)q + sep_len) {
    piece = substring_of(s, p, (uint32_t)q);
    tz_arr_push_value(a, &piece);
    if (a->length == max)
      return a;
  }
  piece = substring_of(s, p, len);
  tz_arr_push_value(a, &piece);
  return a;
}

// trim() (ends 3), trimStart() (1) and trimEnd() (2).
void tz_str_trim(TzStr *out, void *obj, uint64_t meta, int32_t ends) {
  TzStr s = make_str(obj, meta);
  TzStr f = flat(s);
  uint32_t start = 0, end = str_len(f.meta);
  if (ends & 1)
    while (start < end && tz_is_js_space(unit_at(&f, start)))
      ++start;
  if (ends & 2)
    while (end > start && tz_is_js_space(unit_at(&f, end - 1)))
      --end;
  *out = substring_of(s, start, end);
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
  if (length > (int64_t)MAX_LENGTH)
    tz_panicf("RangeError: Invalid string length %lld", (long long)length);
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

// toUpperCase() and toLowerCase(), with the full mappings of Unicode, as V8
// has them: "ß" becomes "SS", and a final capital sigma a final small one.
static void convert_case(TzStr *out, void *obj, uint64_t meta, int upper) {
  TzStr s = make_str(obj, meta);
  TzStr f = flat(s);
  uint32_t len = str_len(f.meta);
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
      tz_panicf("RangeError: Invalid code point %s", text);
    }
    buf_code_point(&b, (uint32_t)cp);
  }
  *out = tz_buf_take(&b);
}

//===----------------------------------------------------------------------===//
// Numbers from strings (number.c does the parsing)
//===----------------------------------------------------------------------===//

// The ASCII characters of flat `f` from `from` to `to`, up to the first other
// one, as a C string in `local` or in a new block; `*n` gets their count.
static char *ascii_text(const TzStr *f, uint32_t from, uint32_t to, char *local, size_t size,
                        size_t *n) {
  char *text = (size_t)(to - from) < size ? local : (char *)tz_alloc((size_t)(to - from) + 1);
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
  while (start < end && tz_is_js_space(unit_at(&f, start)))
    ++start;
  while (end > start && tz_is_js_space(unit_at(&f, end - 1)))
    --end;
  char local[64];
  size_t n;
  char *text = ascii_text(&f, start, end, local, sizeof local, &n);
  double value = tz_number_from_ascii(text, n, n == end - start);
  if (text != local)
    tz_free(text);
  return value;
}

// The ASCII text after the leading whitespace, which parseInt and parseFloat
// read a number from the start of.
static char *leading_text(void *obj, uint64_t meta, char *local, size_t size, size_t *n) {
  TzStr f = flat(make_str(obj, meta));
  uint32_t start = 0, end = str_len(f.meta);
  while (start < end && tz_is_js_space(unit_at(&f, start)))
    ++start;
  return ascii_text(&f, start, end, local, size, n);
}

// parseInt(s, radix); a radix of 0 means none, as in JavaScript.
double tz_parse_int(void *obj, uint64_t meta, int32_t radix) {
  char local[64];
  size_t n;
  char *text = leading_text(obj, meta, local, sizeof local, &n);
  double value = tz_parse_int_ascii(text, n, radix);
  if (text != local)
    tz_free(text);
  return value;
}

double tz_parse_float(void *obj, uint64_t meta) {
  char local[64];
  size_t n;
  char *text = leading_text(obj, meta, local, sizeof local, &n);
  double value = tz_parse_float_ascii(text, n);
  if (text != local)
    tz_free(text);
  return value;
}
