#include "runtime/core/string/string.h"
#include "runtime/core/fail/fail.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/error/error.h"
#include "runtime/core/group/group.h"
#include "runtime/core/number/number.h"
#include "runtime/core/output/output.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/tzrt/tzrt.h"
#include <math.h>
#include "runtime/core/unicode/unicode.h"
#include <stdlib.h>
#include <string.h>

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

#define SHORT_LENGTH 13

#define INLINE_MAX 8

enum { KIND_BUF1 = 1, KIND_BUF2 = 2, KIND_ROPE = 3 };

typedef struct {
  int64_t rc;
  uint32_t kind;
  uint32_t reserved;
} Header;

typedef struct {
  Header h;
  uint32_t capacity;
  uint32_t used;
} Buffer;

typedef struct {
  Header h;
  TzStr left, right;
  TzStr flat;
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

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#error "inline strings assume a little-endian target"
#endif

static inline uint64_t inline_bits(const TzStr *s) { return (uint64_t)(uintptr_t)s->obj; }

static inline TzStr inline_from_bits(uint64_t bits, uint32_t len) {
  TzStr s;
  s.obj = (void *)(uintptr_t)bits;
  s.meta = (uint64_t)len | F_ROPE | ((bits & 0x8080808080808080ull) == 0 ? F_ASCII : 0);

  return s;
}

static inline TzStr inline_str(const uint8_t *data, uint32_t len) {
  uint64_t bits = 0;
  for (uint32_t i = 0; i < len; ++i) {
    bits |= (uint64_t)data[i] << (8 * i);
  }

  return inline_from_bits(bits, len);
}

static inline TzStr inline_piece(const uint8_t *data, uint32_t end, uint32_t from, uint32_t n) {
  uint64_t bits;
  if (from + 8 <= end) {
    memcpy(&bits, data + from, 8);
    bits &= ~0ull >> (64 - 8 * n);
  } else {
    bits = 0;
    for (uint32_t i = 0; i < n; ++i) {
      bits |= (uint64_t)data[from + i] << (8 * i);
    }
  }

  return inline_from_bits(bits, n);
}

static void *object_new(size_t size, uint32_t kind) {
  Header *h = (Header *)tz_alloc(size);
  h->rc = 1;
  h->kind = kind;
  h->reserved = 0;
  ++tz_live_strings;

  return h;
}

static inline void retain(TzStr s) {
  if (s.meta & F_HEAP) {
    tz_rc_inc(&((Header *)s.obj)->rc);
  }
}

void tz_str_free(void *obj);

static inline void release(TzStr s) {
  if ((s.meta & F_HEAP) && tz_rc_dec(&((Header *)s.obj)->rc)) {
    tz_str_free(s.obj);
  }
}

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
    if (s->items != s->inline_items) {
      tz_free(s->items);
    }
    s->items = items;
    s->capacity = capacity;
  }
  s->items[s->count++] = item;
}

static void stack_free(PtrStack *s) {
  if (s->items != s->inline_items) {
    tz_free(s->items);
  }
}

TZ_NOINLINE void tz_str_free_rope(void *obj) {
  PtrStack pending;
  stack_init(&pending);
  stack_push(&pending, obj);
  while (pending.count > 0) {
    Header *h = (Header *)pending.items[--pending.count];
    if (h->kind == KIND_ROPE) {
      Rope *r = (Rope *)h;
      TzStr children[3] = {r->left, r->right, r->flat};
      for (int i = 0; i < 3; ++i) {
        if ((children[i].meta & F_HEAP) && tz_rc_dec(&((Header *)children[i].obj)->rc)) {
          stack_push(&pending, children[i].obj);
        }
      }
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
  --tz_live_strings;
  tz_free(obj);
}

static Buffer *buffer_new(uint32_t capacity, int two) {
  Buffer *b = (Buffer *)object_new(sizeof(Buffer) + (size_t)capacity * (two ? 2 : 1),
                                   two ? KIND_BUF2 : KIND_BUF1);
  b->capacity = capacity;
  b->used = 0;

  return b;
}

static inline char *buffer_units(Buffer *b) { return (char *)b + sizeof(Buffer); }

TZ_NOINLINE void tz_str_flatten(Rope *r, uint64_t meta);

static inline void *rope_flat_obj(const Rope *r) {
#if TZ_PARALLEL
  return __atomic_load_n(&r->flat.obj, __ATOMIC_ACQUIRE);
#else
  return r->flat.obj;
#endif
}

static inline TzStr flat(TzStr s) {
  if (is_rope(s.meta)) {
    Rope *r = (Rope *)s.obj;
    void *obj = rope_flat_obj(r);
    if (!obj) {
      tz_str_flatten(r, s.meta);
      obj = r->flat.obj;
    }
    s.obj = obj;
    s.meta = r->flat.meta;
  }

  return s;
}

static inline const void *units(const TzStr *f) {
  if (is_inline(f->meta)) {
    return &f->obj;
  }
  if (!f->obj) {
    return NULL;
  }

  return (const char *)f->obj + sizeof(Buffer) +
         (size_t)str_off(f->meta) * (is_two(f->meta) ? 2 : 1);
}

static inline uint16_t unit_at(const TzStr *f, uint32_t i) {
  const void *u = units(f);

  return is_two(f->meta) ? ((const uint16_t *)u)[i] : ((const uint8_t *)u)[i];
}

static inline int32_t direct_byte(void *obj, uint64_t meta, uint32_t i) {
  if ((meta & (F_ROPE | F_TWO_BYTE)) == 0) {
    return ((const uint8_t *)obj)[sizeof(Buffer) + str_off(meta) + i];
  }
  if (is_inline(meta)) {
    return (uint8_t)((uint64_t)(uintptr_t)obj >> (8 * i));
  }

  return -1;
}

static void copy_units(void *dst, int dst_two, const TzStr *src) {
  uint32_t len = str_len(src->meta);
  if (len == 0) {
    return;
  }
  const void *from = units(src);
  if (dst_two == is_two(src->meta)) {
    memcpy(dst, from, (size_t)len * (dst_two ? 2 : 1));
  } else {
    uint16_t *to = (uint16_t *)dst;
    const uint8_t *bytes = (const uint8_t *)from;
    for (uint32_t i = 0; i < len; ++i) {
      to[i] = bytes[i];
    }
  }
}

static inline uint64_t add_inline_units(uint64_t bits, uint32_t pos, TzStr s) {
  uint32_t len = str_len(s.meta);
  if (len == 0) {
    return bits;
  }
  if (is_inline(s.meta)) {
    return bits | inline_bits(&s) << (8 * pos);
  }
  TzStr f = flat(s);
  const uint8_t *u = (const uint8_t *)units(&f);
  for (uint32_t i = 0; i < len; ++i) {
    bits |= (uint64_t)u[i] << (8 * (pos + i));
  }

  return bits;
}

#if TZ_PARALLEL
int32_t tz_str_flatten_lock;

static void flatten_lock(void) {
  for (unsigned spin = 0;; ++spin) {
    int32_t free_ = 0;
    if (__atomic_compare_exchange_n(&tz_str_flatten_lock, &free_, 1, 1, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
      return;
    }
    if (spin > 64) {
      tz_thread_yield();
    } else {
      tz_cpu_relax();
    }
  }
}

static void flatten_unlock(void) { __atomic_store_n(&tz_str_flatten_lock, 0, __ATOMIC_RELEASE); }

static void flatten_rope(Rope *r, uint64_t meta);

TZ_NOINLINE void tz_str_flatten(Rope *r, uint64_t meta) {
  if (!tz_rc_is_shared(r->h.rc)) {
    PtrStack pending;
    stack_init(&pending);
    stack_push(&pending, &r->right);
    stack_push(&pending, &r->left);
    while (pending.count > 0) {
      const TzStr *s = (const TzStr *)pending.items[--pending.count];
      if (!is_rope(s->meta)) {
        continue;
      }
      Rope *child = (Rope *)s->obj;
      if (rope_flat_obj(child)) {
        continue;
      }
      if (tz_rc_is_shared(child->h.rc)) {
        tz_str_flatten(child, s->meta);
        continue;
      }
      stack_push(&pending, &child->right);
      stack_push(&pending, &child->left);
    }
    stack_free(&pending);
    flatten_rope(r, meta);
    return;
  }
  flatten_lock();
  if (!r->flat.obj) {
    flatten_rope(r, meta);
  }
  flatten_unlock();
}

static void flatten_rope(Rope *r, uint64_t meta) {
#else
TZ_NOINLINE void tz_str_flatten(Rope *r, uint64_t meta) {
#endif
  uint32_t len = str_len(meta);
  int two = is_two(meta);
  Buffer *b = buffer_new(len + len / 2, two);
  char *dst = buffer_units(b);
  size_t width = two ? 2 : 1;
  uint32_t pos = 0;

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
#if TZ_PARALLEL
  if (tz_rc_is_shared(r->h.rc)) {
    b->h.rc = TZ_RC_SHARED + 1;
  }
  r->flat.meta = make_meta(len, 0, two, is_ascii(meta), F_HEAP);
  __atomic_store_n(&r->flat.obj, (void *)b, __ATOMIC_RELEASE);
#else
  r->flat = make_str(b, make_meta(len, 0, two, is_ascii(meta), F_HEAP));
#endif
  release(r->left);
  release(r->right);
  r->left = kEmpty;
  r->right = kEmpty;
}

static TzStr single_char(uint8_t unit) { return inline_str(&unit, 1); }

static TzStr string_from_units(const void *data, uint32_t len, int two) {
  if (len == 0) {
    return kEmpty;
  }
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
    for (uint32_t i = 0; i < len; ++i) {
      bytes[i] = two ? (uint8_t)((const uint16_t *)data)[i] : ((const uint8_t *)data)[i];
    }
    return inline_str(bytes, len);
  }
  int ascii = 1;
  for (uint32_t i = 0; i < len && ascii; ++i) {
    ascii = (two ? ((const uint16_t *)data)[i] : ((const uint8_t *)data)[i]) < 0x80;
  }

  Buffer *b = buffer_new(len, !narrow);
  if (narrow && two) {
    uint8_t *to = (uint8_t *)buffer_units(b);
    for (uint32_t i = 0; i < len; ++i) {
      to[i] = (uint8_t)((const uint16_t *)data)[i];
    }
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

TzStr tz_str_from_latin1(const uint8_t *bytes, size_t length, int ascii) {
  if (length <= INLINE_MAX) {
    return length ? inline_str(bytes, (uint32_t)length) : kEmpty;
  }
  if (length > MAX_LENGTH) {
    tz_panicf("string length %llu exceeds the maximum of %llu", (unsigned long long)length,
              (unsigned long long)MAX_LENGTH);
  }
  Buffer *b = buffer_new((uint32_t)length, 0);
  memcpy(buffer_units(b), bytes, length);
  b->used = (uint32_t)length;

  return make_str(b, make_meta((uint32_t)length, 0, 0, ascii, F_HEAP));
}

char *tz_str_ascii_units(size_t length, TzStr *out) {
  if (length <= INLINE_MAX) {
    return NULL;
  }
  if (length > MAX_LENGTH) {
    tz_panicf("string length %llu exceeds the maximum of %llu", (unsigned long long)length,
              (unsigned long long)MAX_LENGTH);
  }
  Buffer *b = buffer_new((uint32_t)length, 0);
  b->used = (uint32_t)length;
  *out = make_str(b, make_meta((uint32_t)length, 0, 0, 1, F_HEAP));

  return buffer_units(b);
}

TzStr tz_str_from_utf16(const uint16_t *units, size_t length) {
  if (length > MAX_LENGTH) {
    tz_panicf("string length %llu exceeds the maximum of %llu", (unsigned long long)length,
              (unsigned long long)MAX_LENGTH);
  }

  return string_from_units(units, (uint32_t)length, 1);
}

void tz_str_retain_value(TzStr s) { retain(s); }

void tz_str_release_value(TzStr s) { release(s); }

#if TZ_PARALLEL
void tz_str_publish(TzStr s) {
  if (!(s.meta & F_HEAP)) {
    return;
  }
  PtrStack pending;
  stack_init(&pending);
  stack_push(&pending, s.obj);
  while (pending.count > 0) {
    Header *h = (Header *)pending.items[--pending.count];
    if (h->rc <= 0 || h->rc >= TZ_RC_FOREVER) {
      continue;
    }
    h->rc = TZ_RC_SHARED + h->rc;
    if (h->kind == KIND_ROPE) {
      Rope *r = (Rope *)h;
      TzStr children[3] = {r->left, r->right, r->flat};
      for (int i = 0; i < 3; ++i) {
        if (children[i].meta & F_HEAP) {
          stack_push(&pending, children[i].obj);
        }
      }
    }
  }
  stack_free(&pending);
}
#endif

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

static void join_bytes(uint8_t *out, uint32_t total, const TzStr *items, int64_t n,
                       const TzStr *sep) {
  uint32_t sep_len = str_len(sep->meta), pos = 0;
  const uint8_t *sep_units = (const uint8_t *)units(sep);
  for (int64_t i = 0; i < n; ++i) {
    if (i > 0 && sep_len) {
      if (sep_len == 1) {
        out[pos] = sep_units[0];
      } else {
        copy_bytes(out + pos, sep_units, sep_len);
      }
      pos += sep_len;
    }
    TzStr s = items[i];
    uint32_t len = str_len(s.meta);
    if (is_inline(s.meta) && pos + 8 <= total) {
      memcpy(out + pos, &s.obj, 8);
    } else if (len) {
      TzStr f = flat(s);
      copy_bytes(out + pos, (const uint8_t *)units(&f), len);
    }
    pos += len;
  }
}

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
  if (total > MAX_LENGTH) {
    tz_panicf("string length %llu exceeds the maximum of %llu", (unsigned long long)total,
              (unsigned long long)MAX_LENGTH);
  }
  if (total == 0) {
    return kEmpty;
  }

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

#define NUMBER_INT 1
#define NUMBER_DOUBLE 2

typedef struct {
  Buffer b;
  char units[32];
} NumberText;

void tz_str_join(TzStr *out, const TzStr *items, uint32_t n, const TzStr *sep) {
  TzStr comma = single_char(',');
  *out = join_sep(items, n, sep ? sep : &comma);
}

TZ_NOINLINE int tz_str_units_equal(const TzStr *a, uint32_t ia, const TzStr *b, uint32_t ib,
                                   uint32_t n) {
  if (n == 0) {
    return 1;
  }
  if (is_two(a->meta) == is_two(b->meta)) {
    size_t width = is_two(a->meta) ? 2 : 1;
    return memcmp((const char *)units(a) + ia * width, (const char *)units(b) + ib * width,
                  n * width) == 0;
  }
  for (uint32_t i = 0; i < n; ++i) {
    if (unit_at(a, ia + i) != unit_at(b, ib + i)) {
      return 0;
    }
  }

  return 1;
}

TZ_HOT int32_t tz_str_equal(void *a_obj, uint64_t a_meta, void *b_obj, uint64_t b_meta) {
  uint32_t len = str_len(a_meta);
  if (len != str_len(b_meta)) {
    return 0;
  }
  if (a_obj == b_obj && a_meta == b_meta) {
    return 1;
  }
  if (is_inline(a_meta) && is_inline(b_meta)) {
    return 0;
  }
  TzStr a = flat(make_str(a_obj, a_meta)), b = flat(make_str(b_obj, b_meta));

  return tz_str_units_equal(&a, 0, &b, 0, len);
}

static inline uint32_t find_byte(const uint8_t *data, uint32_t from, uint32_t end, uint8_t c) {
#ifdef TZ_SSE2
  __m128i needle = _mm_set1_epi8((char)c);
  for (; from + 16 <= end; from += 16) {
    __m128i block = _mm_loadu_si128((const __m128i *)(data + from));
    unsigned mask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(block, needle));
    if (mask) {
      return from + (uint32_t)__builtin_ctz(mask);
    }
  }
#endif
  for (; from < end; ++from) {
    if (data[from] == c) {
      return from;
    }
  }

  return end;
}

static inline uint32_t find_unit16(const uint16_t *data, uint32_t from, uint32_t end, uint16_t c) {
#ifdef TZ_SSE2
  __m128i needle = _mm_set1_epi16((short)c);
  for (; from + 8 <= end; from += 8) {
    __m128i block = _mm_loadu_si128((const __m128i *)(data + from));
    unsigned mask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi16(block, needle));
    if (mask) {
      return from + (uint32_t)__builtin_ctz(mask) / 2;
    }
  }
#endif
  for (; from < end; ++from) {
    if (data[from] == c) {
      return from;
    }
  }

  return end;
}

static inline uint32_t find_unit(const void *units, int two, uint32_t from, uint32_t end,
                                 uint16_t c) {
  if (two) {
    return find_unit16((const uint16_t *)units, from, end, c);
  }

  return c > 0xFF ? end : find_byte((const uint8_t *)units, from, end, (uint8_t)c);
}

static void check_index(int64_t index, uint32_t len) {
  if (index < 0 || index >= (int64_t)len) {
    tz_panicf("internal error: string index %lld out of range for length %u", (long long)index,
              len);
  }
}

TZ_NOINLINE void tz_str_short_piece(TzStr *out, const TzStr *f, const uint8_t *data, uint32_t from,
                                    uint32_t n) {
  if (n == str_len(f->meta)) {
    retain(*f);
    *out = *f;
    return;
  }
  Buffer *b = buffer_new(n, 0);
  copy_bytes((uint8_t *)buffer_units(b), data + from, n);
  b->used = n;
  int ascii = 1;
  if (!is_ascii(f->meta)) {
    for (uint32_t i = 0; i < n && ascii; ++i) {
      ascii = data[from + i] < 0x80;
    }
  }
  *out = make_str(b, make_meta(n, 0, 0, ascii, F_HEAP));
}

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

TZ_HOT void tz_str_char_at(TzStr *out, void *obj, uint64_t meta, int64_t index) {
  check_index(index, str_len(meta));
  int32_t unit = direct_byte(obj, meta, (uint32_t)index);
  if (unit >= 0) {
    *out = inline_from_bits((uint64_t)unit, 1);
  } else {
    tz_str_char_at_slow(out, obj, meta, (uint32_t)index);
  }
}

void tz_buf_integer(TzBuf *b, uint32_t kind, int64_t value) {
  char text[24];
  int negative = kind != TZ_T_UINT64 && value < 0;
  uint64_t magnitude = negative ? 0 - (uint64_t)value : (uint64_t)value;
  char *p = text + sizeof text;
  do {
    *--p = (char)('0' + magnitude % 10);
    magnitude /= 10;
  } while (magnitude != 0);
  if (negative) {
    *--p = '-';
  }
  tz_buf_ascii_n(b, p, (size_t)(text + sizeof text - p));
}

void tz_str_from_f64(TzStr *out, double value) {
  char buf[64];
  *out = string_from_ascii(buf, tz_format_f64(value, buf));
}

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
    if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xFFFD;
    }
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

void tz_print_string(void *obj, uint64_t meta) {
  TzStr f = flat(make_str(obj, meta));
  uint32_t len = str_len(f.meta);
  if (len == 0) {
    return;
  }
  if (is_ascii(f.meta)) {
    tz_out_write((const char *)units(&f), len);
  } else {
    tz_out_write_units(units(&f), len, is_two(f.meta));
  }
}

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
  if (b->length + extra <= b->capacity) {
    return;
  }
  size_t capacity = b->capacity + b->capacity / 2 + 16;
  if (capacity < b->length + extra) {
    capacity = b->length + extra;
  }
  uint16_t *data = (uint16_t *)tz_alloc(capacity * sizeof(uint16_t));
  if (b->length) {
    memcpy(data, b->data, b->length * sizeof(uint16_t));
  }
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
  for (size_t i = 0; i < length; ++i) {
    b->data[b->length++] = (uint8_t)text[i];
  }
}

void tz_buf_ascii(TzBuf *b, const char *text) { tz_buf_ascii_n(b, text, strlen(text)); }

void tz_buf_bytes(TzBuf *b, const uint8_t *units8, size_t length) {
  buf_reserve(b, length);
  for (size_t i = 0; i < length; ++i) {
    b->data[b->length + i] = units8[i];
  }
  b->length += length;
}

void tz_buf_utf8(TzBuf *out, const char *text, size_t n) {
  const unsigned char *s = (const unsigned char *)text;
  for (size_t i = 0; i < n;) {
    uint32_t c = s[i];
    size_t len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
    if (len == 0 || i + len > n) {
      tz_buf_push(out, 0xFFFD);
      ++i;
      continue;
    }
    if (len > 1) {
      c &= 0x3Fu >> (len - 1);
      for (size_t k = 1; k < len; ++k) {
        c = (c << 6) | (s[i + k] & 0x3Fu);
      }
    }
    i += len;
    if (c >= 0x10000) {
      c -= 0x10000;
      tz_buf_push(out, (uint16_t)(0xD800 + (c >> 10)));
      tz_buf_push(out, (uint16_t)(0xDC00 + (c & 0x3FF)));
    } else {
      tz_buf_push(out, (uint16_t)c);
    }
  }
}

void tz_buf_units(TzBuf *b, const uint16_t *units16, size_t length) {
  buf_reserve(b, length);
  if (length) {
    memcpy(b->data + b->length, units16, length * sizeof(uint16_t));
  }
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

typedef struct {
  Buffer *b;
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

static void builder_put(Builder *w, const TzStr *s, uint32_t from, uint32_t n) {
  if (n == 0) {
    return;
  }
  if (!w->b) {
    for (uint32_t i = 0; i < n; ++i) {
      w->bits |= (uint64_t)(uint8_t)unit_at(s, from + i) << (8 * (w->pos + i));
    }
  } else {
    char *dst = buffer_units(w->b) + (size_t)w->pos * (w->two ? 2 : 1);
    const char *src = (const char *)units(s);
    if (is_two(s->meta) == w->two) {
      size_t width = w->two ? 2 : 1;
      memcpy(dst, src + (size_t)from * width, (size_t)n * width);
    } else {
      for (uint32_t i = 0; i < n; ++i) {
        ((uint16_t *)dst)[i] = ((const uint8_t *)src)[from + i];
      }
    }
  }
  w->pos += n;
}

static TzStr builder_finish(Builder *w, int ascii) {
  if (!w->b) {
    return w->pos == 0 ? kEmpty : inline_from_bits(w->bits, w->pos);
  }
  w->b->used = w->pos;

  return make_str(w->b, make_meta(w->pos, 0, w->two, ascii, F_HEAP));
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

static inline int is_space(uint32_t unit) {
  return unit == ' ' || (unit >= 9 && unit <= 13) || (unit >= 0x80 && tz_is_js_space(unit));
}

static int final_sigma(const TzStr *f, uint32_t i, uint32_t len) {
  int preceded = 0;
  for (uint32_t j = i; j > 0;) {
    uint32_t cp = previous_code_point(f, &j);
    if (!tz_is_case_ignorable(cp)) {
      preceded = tz_is_cased(cp);
      break;
    }
  }
  if (!preceded) {
    return 0;
  }
  for (uint32_t j = i + 1; j < len;) {
    uint32_t cp = next_code_point(f, &j, len);
    if (!tz_is_case_ignorable(cp)) {
      return !tz_is_cased(cp);
    }
  }

  return 1;
}

static inline int flips_case(uint8_t c, int upper) {
  if (upper) {
    return (c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7);
  }

  return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7);
}

static int latin1_case(TzStr *out, const TzStr *s, const TzStr *f, int upper) {
  uint32_t len = str_len(f->meta);
  const uint8_t *u = (const uint8_t *)units(f);
  uint32_t first = len;
  for (uint32_t i = 0; i < len; ++i) {
    if (upper && (u[i] == 0xB5 || u[i] == 0xDF || u[i] == 0xFF)) {
      return 0;
    }
    if (first == len && flips_case(u[i], upper)) {
      first = i;
    }
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
  for (uint32_t i = first; i < len; ++i) {
    if (flips_case(dst[i], upper)) {
      dst[i] ^= 0x20;
    }
  }
  *out = builder_finish(&w, 0);

  return 1;
}

static void convert_case(TzStr *out, void *obj, uint64_t meta, int upper) {
  TzStr s = make_str(obj, meta);
  TzStr f = flat(s);
  uint32_t len = str_len(f.meta);
  if (is_ascii(f.meta) && is_inline(f.meta)) {
    uint64_t bits = inline_bits(&f);
    uint64_t from = upper ? 0x1F1F1F1F1F1F1F1Full : 0x3F3F3F3F3F3F3F3Full;
    uint64_t past = upper ? 0x0505050505050505ull : 0x2525252525252525ull;
    uint64_t letters = (bits + from) & ~(bits + past) & 0x8080808080808080ull;
    *out = inline_from_bits(bits ^ (letters >> 2), len);
    return;
  }
  if (!is_two(f.meta) && !is_ascii(f.meta) && latin1_case(out, &s, &f, upper)) {
    return;
  }
  if (is_ascii(f.meta)) {
    const uint8_t *u = (const uint8_t *)units(&f);
    uint8_t from = upper ? 'a' : 'A';
    uint32_t i = 0;
    while (i < len && (uint8_t)(u[i] - from) > 25) {
      ++i;
    }
    if (i == len) {
      retain(s);
      *out = s;
      return;
    }
    Builder w;
    builder_init(&w, len, 0);
    builder_put(&w, &f, 0, len);
    uint8_t *dst = w.b ? (uint8_t *)buffer_units(w.b) : (uint8_t *)&w.bits;
    for (; i < len; ++i) {
      if ((uint8_t)(dst[i] - from) <= 25) {
        dst[i] ^= 0x20;
      }
    }
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
    for (int k = 0; k < count; ++k) {
      buf_code_point(&b, mapped[k]);
    }
  }
  if (!changed) {
    tz_buf_free(&b);
    retain(s);
    *out = s;
    return;
  }
  *out = tz_buf_take(&b);
}

void tz_str_to_lower(TzStr *out, void *obj, uint64_t meta) { convert_case(out, obj, meta, 0); }

#define URI_BIT(c) (1ull << ((c) & 63))
#define URI_RANGE(a, b) ((~0ull >> (63 - ((b) & 63))) & (~0ull << ((a) & 63)))
#define URI_UNRESERVED_LOW                                                                         \
  (URI_RANGE('0', '9') | URI_BIT('!') | URI_BIT('\'') | URI_BIT('(') | URI_BIT(')') |              \
   URI_BIT('*') | URI_BIT('-') | URI_BIT('.'))
#define URI_UNRESERVED_HIGH                                                                        \
  (URI_RANGE('A', 'Z') | URI_RANGE('a', 'z') | URI_BIT('_') | URI_BIT('~'))
#define URI_RESERVED_LOW                                                                           \
  (URI_BIT(';') | URI_BIT('/') | URI_BIT('?') | URI_BIT(':') | URI_BIT('&') | URI_BIT('=') |       \
   URI_BIT('+') | URI_BIT('$') | URI_BIT(',') | URI_BIT('#'))
#define URI_RESERVED_HIGH URI_BIT('@')

#define URI_EACH8(F, c)                                                                            \
  F(c), F((c) + 1), F((c) + 2), F((c) + 3), F((c) + 4), F((c) + 5), F((c) + 6), F((c) + 7)
#define URI_EACH64(F, c)                                                                           \
  URI_EACH8(F, c), URI_EACH8(F, (c) + 8), URI_EACH8(F, (c) + 16), URI_EACH8(F, (c) + 24),          \
      URI_EACH8(F, (c) + 32), URI_EACH8(F, (c) + 40), URI_EACH8(F, (c) + 48),                      \
      URI_EACH8(F, (c) + 56)
#define URI_EACH256(F) {URI_EACH64(F, 0), URI_EACH64(F, 64), URI_EACH64(F, 128), URI_EACH64(F, 192)}

#define URI_LENGTH(c, low, high)                                                                   \
  ((c) >= 128 ? 6 : (((c) < 64 ? (low) : (high)) >> ((c) & 63)) & 1 ? 1 : 3)
#define URI_COMPONENT_LENGTH(c) URI_LENGTH(c, URI_UNRESERVED_LOW, URI_UNRESERVED_HIGH)
#define URI_FULL_LENGTH(c)                                                                         \
  URI_LENGTH(c, URI_UNRESERVED_LOW | URI_RESERVED_LOW, URI_UNRESERVED_HIGH | URI_RESERVED_HIGH)
#define URI_HEX(c)                                                                                 \
  ((c) >= '0' && (c) <= '9'                     ? (c) - '0'                                        \
   : ((c) | 0x20) >= 'a' && ((c) | 0x20) <= 'f' ? ((c) | 0x20) - 'a' + 10                          \
                                                : -1)

static const uint8_t kUriComponentLengths[256] = URI_EACH256(URI_COMPONENT_LENGTH);
static const uint8_t kUriLengths[256] = URI_EACH256(URI_FULL_LENGTH);
static const int8_t kHexDigits[256] = URI_EACH256(URI_HEX);

TZ_THREAD_LOCAL TzMaker tz_uri_fail;

static void uri_malformed(TzStr *out) {
  tz_fail_ascii(tz_uri_fail, "ERR_INVALID_URI", "URI malformed");
  *out = kEmpty;
}

static void uri_encode(TzStr *out, void *obj, uint64_t meta, const uint8_t *lengths) {
  static const char hex[] = "0123456789ABCDEF";
  TzStr s = make_str(obj, meta);
  TzStr f = flat(s);
  uint32_t len = str_len(f.meta);
  const uint8_t *bytes = (const uint8_t *)units(&f);
  const uint16_t *wide = (const uint16_t *)bytes;
  int two = is_two(f.meta);
  uint64_t total = 0;
  if (!two) {
    for (uint32_t i = 0; i < len; ++i) {
      total += lengths[bytes[i]];
    }
  } else {
    for (uint32_t i = 0; i < len; ++i) {
      uint32_t c = wide[i];
      if (c < 0x100) {
        total += lengths[c];
      } else if (c < 0x800) {
        total += 6;
      } else if (c < 0xD800 || c > 0xDFFF) {
        total += 9;
      } else if (c <= 0xDBFF && i + 1 < len && wide[i + 1] >= 0xDC00 && wide[i + 1] <= 0xDFFF) {
        total += 12;
        ++i;
      } else {
        total += 9;
      }
    }
  }
  if (total == len) {
    retain(s);
    *out = s;
    return;
  }
  char local[INLINE_MAX];
  char *text = tz_str_ascii_units((size_t)total, out);
  char *p = text ? text : local;
  if (!two) {
    for (uint32_t i = 0; i < len; ++i) {
      uint32_t c = bytes[i];
      uint8_t length = lengths[c];
      if (length == 1) {
        *p++ = (char)c;
        continue;
      }
      if (length == 6) {
        p[0] = '%';
        p[1] = 'C';
        p[2] = hex[c >> 6];
        p += 3;
        c = 0x80 | (c & 0x3F);
      }
      p[0] = '%';
      p[1] = hex[c >> 4];
      p[2] = hex[c & 15];
      p += 3;
    }
    if (!text) {
      *out = string_from_ascii(local, (size_t)total);
    }
    return;
  }
  for (uint32_t i = 0; i < len;) {
    while (i < len && wide[i] < 0x80 && lengths[wide[i]] == 1) {
      *p++ = (char)wide[i++];
    }
    if (i == len) {
      break;
    }
    uint32_t c = wide[i++];
    if (c >= 0xD800 && c <= 0xDBFF && i < len && wide[i] >= 0xDC00 && wide[i] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (uint32_t)(wide[i++] - 0xDC00);
    } else if (c >= 0xD800 && c <= 0xDFFF) {
      c = 0xFFFD;
    }
    uint8_t utf8[4];
    int n;
    if (c < 0x80) {
      utf8[0] = (uint8_t)c, n = 1;
    } else if (c < 0x800) {
      utf8[0] = (uint8_t)(0xC0 | c >> 6), utf8[1] = (uint8_t)(0x80 | (c & 0x3F)), n = 2;
    } else if (c < 0x10000) {
      utf8[0] = (uint8_t)(0xE0 | c >> 12), utf8[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
      utf8[2] = (uint8_t)(0x80 | (c & 0x3F)), n = 3;
    } else {
      utf8[0] = (uint8_t)(0xF0 | c >> 18), utf8[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
      utf8[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F)), utf8[3] = (uint8_t)(0x80 | (c & 0x3F));
      n = 4;
    }
    for (int k = 0; k < n; ++k) {
      p[0] = '%';
      p[1] = hex[utf8[k] >> 4];
      p[2] = hex[utf8[k] & 15];
      p += 3;
    }
  }
  if (!text) {
    *out = string_from_ascii(local, (size_t)total);
  }
}

void tz_uri_encode_component(TzStr *out, void *obj, uint64_t meta) {
  uri_encode(out, obj, meta, kUriComponentLengths);
}

void tz_uri_encode(TzStr *out, void *obj, uint64_t meta) {
  uri_encode(out, obj, meta, kUriLengths);
}

static inline int hex_digit(uint32_t c) { return c < 256 ? kHexDigits[c] : -1; }

static inline int uri_escape_at(const void *u, int two, uint32_t len, uint32_t at) {
  if (at + 2 >= len) {
    return -1;
  }
  const uint8_t *bytes = (const uint8_t *)u;
  const uint16_t *wide = (const uint16_t *)u;
  uint32_t c = two ? wide[at] : bytes[at];
  int hi = hex_digit(two ? wide[at + 1] : bytes[at + 1]);
  int lo = hex_digit(two ? wide[at + 2] : bytes[at + 2]);

  return c == '%' && (hi | lo) >= 0 ? hi << 4 | lo : -1;
}

static inline uint32_t uri_unescape(const void *u, int two, uint32_t len, uint32_t at,
                                    uint32_t *c) {
  static const uint32_t smallest[5] = {0, 0, 0x80, 0x800, 0x10000};
  int b = uri_escape_at(u, two, len, at);
  if (b < 0) {
    return 0;
  }
  if (b < 0x80) {
    *c = (uint32_t)b;
    return 3;
  }
  int count = b >= 0xF8 ? 0 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 0;
  if (count == 0) {
    return 0;
  }
  uint32_t v = (uint32_t)b & (0x7Fu >> count);
  for (int k = 1; k < count; ++k) {
    int more = uri_escape_at(u, two, len, at + 3 * (uint32_t)k);
    if (more < 0 || (more & 0xC0) != 0x80) {
      return 0;
    }
    v = v << 6 | (uint32_t)(more & 0x3F);
  }
  if (v < smallest[count] || (v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF) {
    return 0;
  }
  *c = v;

  return 3 * (uint32_t)count;
}

static inline int uri_keeps_escape(uint32_t c, int keep_reserved) {
  return keep_reserved && c < 0x80 && kUriLengths[c] == 1 && kUriComponentLengths[c] != 1;
}

static inline void uri_decode_wide(TzStr *out, const void *u, int two, uint32_t len, uint32_t i,
                                   int keep_reserved, const uint8_t *prefix, uint32_t n,
                                   uint32_t room) {
  Buffer *b = buffer_new(room, 1);
  uint16_t *result = (uint16_t *)buffer_units(b);
  uint32_t all = 0;
  for (uint32_t k = 0; k < n; ++k) {
    result[k] = prefix[k];
  }
  uint32_t next = find_unit(u, two, i, len, '%');
  for (;;) {
    for (; i < next; ++i) {
      uint16_t unit = two ? ((const uint16_t *)u)[i] : ((const uint8_t *)u)[i];
      result[n++] = unit;
      all |= unit;
    }
    if (i == len) {
      break;
    }
    uint32_t c, taken = uri_unescape(u, two, len, i, &c);
    if (taken == 0) {
      tz_str_free(b);
      uri_malformed(out);
      return;
    }
    if (uri_keeps_escape(c, keep_reserved)) {
      for (uint32_t k = 0; k < 3; ++k) {
        result[n++] = two ? ((const uint16_t *)u)[i + k] : ((const uint8_t *)u)[i + k];
      }
    } else if (c >= 0x10000) {
      result[n++] = (uint16_t)(0xD800 + ((c - 0x10000) >> 10));
      result[n++] = (uint16_t)(0xDC00 + (c & 0x3FF));
      all |= 0xD800;
    } else {
      result[n++] = (uint16_t)c;
      all |= c;
    }
    i += taken;
    next = find_unit(u, two, i, len, '%');
  }
  if (all <= 0xFF || n <= INLINE_MAX || n < room / 2) {
    *out = string_from_units(result, n, 1);
    tz_str_free(b);
    return;
  }
  b->used = n;
  *out = make_str(b, make_meta(n, 0, 1, 0, F_HEAP));
}

static void uri_decode_bytes(TzStr *out, const uint8_t *u, uint32_t len, uint32_t next,
                             int keep_reserved, int ascii, uint32_t room) {
  uint32_t c;
  if (uri_unescape(u, 0, len, next, &c) && c > 0xFF) {
    uri_decode_wide(out, u, 0, len, next, keep_reserved, u, next, room);
    return;
  }
  Buffer *b = buffer_new(room, 0);
  uint8_t *dst = (uint8_t *)buffer_units(b);
  memcpy(dst, u, next);
  uint32_t n = next, i = next;
  while (i < len) {
    if (u[i] != '%') {
      dst[n++] = u[i++];
      continue;
    }
    uint32_t taken = uri_unescape(u, 0, len, i, &c);
    if (taken == 0) {
      tz_str_free(b);
      uri_malformed(out);
      return;
    }
    if (c > 0xFF) {
      uri_decode_wide(out, u, 0, len, i, keep_reserved, dst, n, room);
      tz_str_free(b);
      return;
    }
    if (uri_keeps_escape(c, keep_reserved)) {
      dst[n] = u[i], dst[n + 1] = u[i + 1], dst[n + 2] = u[i + 2];
      n += 3;
    } else {
      dst[n++] = (uint8_t)c;
      ascii &= c < 0x80;
    }
    i += taken;
  }
  if (n <= INLINE_MAX || n < room / 2) {
    *out = string_from_units(dst, n, 0);
    tz_str_free(b);
    return;
  }
  b->used = n;
  *out = make_str(b, make_meta(n, 0, 0, ascii, F_HEAP));
}

static void uri_decode(TzStr *out, void *obj, uint64_t meta, int keep_reserved) {
  TzStr s = make_str(obj, meta);
  TzStr f = flat(s);
  uint32_t len = str_len(f.meta);
  const void *u = units(&f);
  int two = is_two(f.meta);
  uint32_t next = find_unit(u, two, 0, len, '%');
  if (next == len) {
    retain(s);
    *out = s;
    return;
  }
  if (two) {
    uri_decode_wide(out, u, 1, len, 0, keep_reserved, NULL, 0, len);
  } else {
    uri_decode_bytes(out, (const uint8_t *)u, len, next, keep_reserved, is_ascii(f.meta), len);
  }
}

void tz_uri_decode_component(TzStr *out, void *obj, uint64_t meta, TzMaker fail) {
  tz_uri_fail = fail;
  uri_decode(out, obj, meta, 0);
  tz_uri_fail = NULL;
}

void tz_uri_decode(TzStr *out, void *obj, uint64_t meta, TzMaker fail) {
  tz_uri_fail = fail;
  uri_decode(out, obj, meta, 1);
  tz_uri_fail = NULL;
}

static const char *ascii_text(const TzStr *f, uint32_t from, uint32_t to, char *local, size_t size,
                              size_t *n, char **owned) {
  *owned = NULL;
  if (!is_two(f->meta)) {
    const char *text = from < to ? (const char *)units(f) + from : "";
    size_t k = 0;
    while (k < to - from && (uint8_t)text[k] < 0x80) {
      ++k;
    }
    *n = k;
    return text;
  }
  char *text = local;
  if ((size_t)(to - from) >= size) {
    text = *owned = (char *)tz_alloc((size_t)(to - from) + 1);
  }
  size_t k = 0;
  for (uint32_t i = from; i < to; ++i) {
    uint16_t unit = unit_at(f, i);
    if (unit >= 0x80) {
      break;
    }
    text[k++] = (char)unit;
  }
  text[k] = 0;
  *n = k;

  return text;
}

double tz_str_to_number(void *obj, uint64_t meta) {
  TzStr f = flat(make_str(obj, meta));
  uint32_t start = 0, end = str_len(f.meta);
  while (start < end && is_space(unit_at(&f, start))) {
    ++start;
  }
  while (end > start && is_space(unit_at(&f, end - 1))) {
    --end;
  }
  char local[64], *owned;
  size_t n;
  const char *text = ascii_text(&f, start, end, local, sizeof local, &n, &owned);
  double value = tz_number_from_ascii(text, n, n == end - start);
  tz_free(owned);

  return value;
}

static const char *leading_text(const TzStr *f, char *local, size_t size, size_t *n, char **owned) {
  uint32_t start = 0, end = str_len(f->meta);
  while (start < end && is_space(unit_at(f, start))) {
    ++start;
  }

  return ascii_text(f, start, end, local, size, n, owned);
}

double tz_parse_int(void *obj, uint64_t meta, int64_t radix) {
  TzStr f = flat(make_str(obj, meta));
  char local[64], *owned;
  size_t n;
  const char *text = leading_text(&f, local, sizeof local, &n, &owned);
  double value = tz_parse_int_ascii(text, n, radix);
  tz_free(owned);

  return value;
}

double tz_parse_float(void *obj, uint64_t meta);

static void number_fail(TzMaker fail, const char *what, void *obj, uint64_t meta) {
  TzStr given = make_str(obj, meta);
  TzBuf text;
  tz_buf_init(&text);
  tz_buf_utf8(&text, what, strlen(what));
  tz_buf_utf8(&text, ": '", 3);
  tz_buf_str(&text, given);
  tz_buf_utf8(&text, "'", 1);
  tz_fail(fail, "ERR_INVALID_NUMBER", tz_buf_take(&text));
}

static double finite_or_fail(double value, TzMaker fail, void *obj, uint64_t meta) {
  if (value != value) {
    number_fail(fail, "not a number", obj, meta);
    return 0;
  }
  if (value == INFINITY || value == -INFINITY) {
    number_fail(fail, "the number is beyond 1.8e308, the range of a double", obj, meta);
    return 0;
  }

  return value;
}

double tz_finite_r(double value, TzMaker fail) {
  const char *what = value != value ? "the bytes hold NaN, which a tz double cannot be"
                     : value == INFINITY || value == -INFINITY
                         ? "the bytes hold an infinity, which a tz double cannot be"
                         : NULL;
  if (what) {
    tz_fail(fail, "ERR_INVALID_NUMBER", tz_str_from_ascii(what, strlen(what)));
    return 0;
  }

  return value;
}

double tz_parse_float_r(void *obj, uint64_t meta, TzMaker fail) {
  return finite_or_fail(tz_parse_float(obj, meta), fail, obj, meta);
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
