#include "runtime/library/decimal/decimal/decimal.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/error/error.h"
#include "runtime/core/group/group.h"
#include "runtime/core/output/output.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/core/unicode/unicode.h"
#include "runtime/library/decimal/decimal_text/decimal_text.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE 1000000000u
#define MAX_EXP 10000000
#define MIN_EXP (-10000000)
#define SCALE_MAX 0x7fffffffu
#define DECIMAL_PLACES 20
#define ARG_MAX 1000000000

enum {
  ROUND_UP,
  ROUND_DOWN,
  ROUND_CEIL,
  ROUND_FLOOR,
  ROUND_HALF_UP,
  ROUND_HALF_DOWN,
  ROUND_HALF_EVEN,
  ROUND_HALF_CEIL,
  ROUND_HALF_FLOOR,
};

static const uint32_t pow10_32[10] = {1,      10,      100,      1000,      10000,
                                      100000, 1000000, 10000000, 100000000, 1000000000};

static const uint64_t pow10_64[20] = {1ull,
                                      10ull,
                                      100ull,
                                      1000ull,
                                      10000ull,
                                      100000ull,
                                      1000000ull,
                                      10000000ull,
                                      100000000ull,
                                      1000000000ull,
                                      10000000000ull,
                                      100000000000ull,
                                      1000000000000ull,
                                      10000000000000ull,
                                      100000000000000ull,
                                      1000000000000000ull,
                                      10000000000000000ull,
                                      100000000000000000ull,
                                      1000000000000000000ull,
                                      10000000000000000000ull};

static inline int is_short(uint64_t meta) { return (meta & TZ_DEC_LONG) == 0; }

static inline uint32_t scale_of(uint64_t meta) { return (uint32_t)meta; }

static inline uint64_t magnitude(int64_t v) { return v < 0 ? 0 - (uint64_t)v : (uint64_t)v; }

TZ_COLD TZ_NORETURN static void overflow(void) { tz_panicf("Maximum decimal size exceeded"); }

TZ_COLD TZ_NORETURN static void division_by_zero(void) { tz_panicf("Division by zero"); }

TZ_COLD TZ_NORETURN static void out_of_range(int64_t value) {
  tz_panicf("internal error: a decimal argument out of range: %lld", (long long)value);
}

void tz_dec_retain_value(TzDec v) {
  if (v.meta & TZ_DEC_HEAP) {
    tz_rc_inc(&((TzDecObj *)(uintptr_t)v.coef)->rc);
  }
}

void tz_dec_release_value(TzDec v) {
  if ((v.meta & TZ_DEC_HEAP) && tz_rc_dec(&((TzDecObj *)(uintptr_t)v.coef)->rc)) {
    tz_dec_free((void *)(uintptr_t)v.coef);
  }
}

#if TZ_PARALLEL
void tz_dec_publish(TzDec v) {
  if (!(v.meta & TZ_DEC_HEAP)) {
    return;
  }
  TzDecObj *o = (TzDecObj *)(uintptr_t)v.coef;
  if (o->rc > 0 && o->rc < TZ_RC_FOREVER) {
    o->rc = TZ_RC_SHARED + o->rc;
  }
}
#endif

typedef struct {
  uint32_t *d;
  uint32_t n, cap;
} Nat;

static void nat_init(Nat *x) {
  x->d = NULL;
  x->n = x->cap = 0;
}

static void nat_free(Nat *x) {
  tz_free(x->d);
  nat_init(x);
}

static void nat_reserve(Nat *x, uint64_t cap) {
  if (cap <= x->cap) {
    return;
  }
  if (cap > (UINT64_C(1) << 28)) {
    tz_panicf("Maximum decimal size exceeded");
  }
  uint64_t grown = (uint64_t)x->cap * 2;
  uint32_t c = (uint32_t)(cap < 4 ? 4 : cap > grown ? cap : grown);
  uint32_t *d = (uint32_t *)tz_alloc((size_t)c * sizeof(uint32_t));
  if (x->n) {
    memcpy(d, x->d, (size_t)x->n * sizeof(uint32_t));
  }
  tz_free(x->d);
  x->d = d;
  x->cap = c;
}

static void nat_trim(Nat *x) {
  while (x->n > 0 && x->d[x->n - 1] == 0) {
    --x->n;
  }
}

static void nat_set_u64(Nat *x, uint64_t v) {
  nat_reserve(x, 3);
  x->n = 0;
  for (; v; v /= BASE) {
    x->d[x->n++] = (uint32_t)(v % BASE);
  }
}

static void nat_copy(Nat *r, const Nat *a) {
  if (r == a) {
    return;
  }
  nat_reserve(r, a->n);
  if (a->n) {
    memcpy(r->d, a->d, (size_t)a->n * sizeof(uint32_t));
  }
  r->n = a->n;
}

static void nat_swap(Nat *a, Nat *b) {
  Nat t = *a;
  *a = *b;
  *b = t;
}

static int nat_cmp(const Nat *a, const Nat *b) {
  if (a->n != b->n) {
    return a->n < b->n ? -1 : 1;
  }
  for (uint32_t i = a->n; i-- > 0;) {
    if (a->d[i] != b->d[i]) {
      return a->d[i] < b->d[i] ? -1 : 1;
    }
  }

  return 0;
}

static void nat_add(Nat *r, const Nat *a, const Nat *b) {
  if (a->n < b->n) {
    const Nat *t = a;
    a = b;
    b = t;
  }
  uint32_t n = a->n, bn = b->n;
  nat_reserve(r, (uint64_t)n + 1);
  uint32_t carry = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t s = a->d[i] + (i < bn ? b->d[i] : 0) + carry;
    carry = s >= BASE;
    r->d[i] = carry ? s - BASE : s;
  }
  r->n = n;
  if (carry) {
    r->d[r->n++] = 1;
  }
}

static void nat_sub(Nat *r, const Nat *a, const Nat *b) {
  uint32_t n = a->n, bn = b->n;
  nat_reserve(r, n);
  int64_t borrow = 0;
  for (uint32_t i = 0; i < n; ++i) {
    int64_t t = (int64_t)a->d[i] - (i < bn ? b->d[i] : 0) - borrow;
    borrow = t < 0;
    r->d[i] = (uint32_t)(borrow ? t + BASE : t);
  }
  r->n = n;
  nat_trim(r);
}

static void nat_mul(Nat *r, const Nat *a, const Nat *b) {
  if (!a->n || !b->n) {
    r->n = 0;
    return;
  }
  uint32_t n = a->n + b->n;
  nat_reserve(r, n);
  memset(r->d, 0, (size_t)n * sizeof(uint32_t));
  for (uint32_t i = 0; i < a->n; ++i) {
    uint64_t ai = a->d[i], carry = 0;
    if (!ai) {
      continue;
    }
    for (uint32_t j = 0; j < b->n; ++j) {
      uint64_t t = ai * b->d[j] + r->d[i + j] + carry;
      carry = t / BASE;
      r->d[i + j] = (uint32_t)(t % BASE);
    }
    r->d[i + b->n] = (uint32_t)carry;
  }
  r->n = n;
  nat_trim(r);
}

static void nat_mul_add(Nat *x, uint32_t m, uint32_t add) {
  uint64_t carry = add;
  for (uint32_t i = 0; i < x->n; ++i) {
    uint64_t t = (uint64_t)x->d[i] * m + carry;
    x->d[i] = (uint32_t)(t % BASE);
    carry = t / BASE;
  }
  for (; carry; carry /= BASE) {
    nat_reserve(x, (uint64_t)x->n + 1);
    x->d[x->n++] = (uint32_t)(carry % BASE);
  }
  nat_trim(x);
}

static uint32_t nat_div_small(Nat *q, const Nat *a, uint32_t m) {
  nat_reserve(q, a->n);
  uint64_t rest = 0;
  for (uint32_t i = a->n; i-- > 0;) {
    uint64_t t = rest * BASE + a->d[i];
    q->d[i] = (uint32_t)(t / m);
    rest = t % m;
  }
  q->n = a->n;
  nat_trim(q);

  return (uint32_t)rest;
}

static int digits_u32(uint32_t v) {
  int k = 1;
  while (k < 10 && v >= pow10_32[k]) {
    ++k;
  }

  return k;
}

static uint64_t nat_digits(const Nat *a) {
  return a->n ? (uint64_t)(a->n - 1) * 9 + (uint64_t)digits_u32(a->d[a->n - 1]) : 0;
}

static uint32_t nat_digit(const Nat *a, uint64_t p) {
  uint64_t i = p / 9;

  return i < a->n ? a->d[i] / pow10_32[p % 9] % 10 : 0;
}

static int nat_any_below(const Nat *a, uint64_t p) {
  uint64_t i = p / 9;
  uint64_t whole = i < a->n ? i : a->n;
  for (uint64_t k = 0; k < whole; ++k) {
    if (a->d[k]) {
      return 1;
    }
  }

  return i < a->n && a->d[i] % pow10_32[p % 9] != 0;
}

static uint64_t nat_trailing_zeros(const Nat *a) {
  uint64_t zeros = 0;
  uint32_t i = 0;
  while (i < a->n && a->d[i] == 0) {
    zeros += 9;
    ++i;
  }
  if (i < a->n) {
    for (uint32_t v = a->d[i]; v % 10 == 0; v /= 10) {
      ++zeros;
    }
  }

  return zeros;
}

static void nat_drop_digits(Nat *r, const Nat *a, uint64_t k) {
  uint64_t limbs = k / 9;
  unsigned rest = (unsigned)(k % 9);
  if (limbs >= a->n) {
    r->n = 0;
    return;
  }
  uint32_t an = a->n, n = an - (uint32_t)limbs;
  nat_reserve(r, an);
  if (rest == 0) {
    memmove(r->d, a->d + limbs, (size_t)n * sizeof(uint32_t));
  } else {
    uint32_t div = pow10_32[rest], mul = pow10_32[9 - rest];
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t lo = a->d[limbs + i] / div;
      uint32_t hi = limbs + i + 1 < an ? a->d[limbs + i + 1] % div : 0;
      r->d[i] = lo + hi * mul;
    }
  }
  r->n = n;
  nat_trim(r);
}

static void nat_keep_digits(Nat *x, uint64_t k) {
  uint64_t limbs = k / 9;
  unsigned rest = (unsigned)(k % 9);
  if (limbs >= x->n) {
    return;
  }
  if (rest) {
    x->d[limbs] %= pow10_32[rest];
    x->n = (uint32_t)limbs + 1;
  } else {
    x->n = (uint32_t)limbs;
  }
  nat_trim(x);
}

static void nat_shift_digits(Nat *r, const Nat *a, uint64_t k) {
  nat_copy(r, a);
  if (!r->n || !k) {
    return;
  }
  unsigned rest = (unsigned)(k % 9);
  uint64_t limbs = k / 9;
  if (rest) {
    nat_mul_add(r, pow10_32[rest], 0);
  }
  if (limbs) {
    nat_reserve(r, r->n + limbs);
    memmove(r->d + limbs, r->d, (size_t)r->n * sizeof(uint32_t));
    memset(r->d, 0, (size_t)limbs * sizeof(uint32_t));
    r->n += (uint32_t)limbs;
  }
}

static void nat_divmod(Nat *q, Nat *r, const Nat *a, const Nat *b) {
  if (nat_cmp(a, b) < 0) {
    if (q) {
      q->n = 0;
    }
    if (r) {
      nat_copy(r, a);
    }
    return;
  }
  if (b->n == 1) {
    Nat scratch;
    nat_init(&scratch);
    uint32_t rest = nat_div_small(q ? q : &scratch, a, b->d[0]);
    nat_free(&scratch);
    if (r) {
      nat_set_u64(r, rest);
    }
    return;
  }
  uint32_t n = b->n, m = a->n - b->n;
  uint32_t f = BASE / (b->d[n - 1] + 1);
  Nat u, v;
  nat_init(&u);
  nat_init(&v);
  nat_copy(&u, a);
  nat_copy(&v, b);
  nat_mul_add(&u, f, 0);
  nat_mul_add(&v, f, 0);
  nat_reserve(&u, (uint64_t)a->n + 1);
  while (u.n < a->n + 1) {
    u.d[u.n++] = 0;
  }
  if (q) {
    nat_reserve(q, (uint64_t)m + 1);
    q->n = m + 1;
  }
  uint64_t top = v.d[n - 1], next = v.d[n - 2];
  for (uint32_t j = m + 1; j-- > 0;) {
    uint64_t num = (uint64_t)u.d[j + n] * BASE + u.d[j + n - 1];
    uint64_t qhat = num / top, rhat = num % top;
    while (qhat >= BASE || qhat * next > rhat * BASE + u.d[j + n - 2]) {
      --qhat;
      rhat += top;
      if (rhat >= BASE) {
        break;
      }
    }
    int64_t borrow = 0;
    uint64_t carry = 0;
    for (uint32_t i = 0; i < n; ++i) {
      uint64_t p = qhat * v.d[i] + carry;
      carry = p / BASE;
      int64_t t = (int64_t)u.d[i + j] - (int64_t)(p % BASE) - borrow;
      borrow = t < 0;
      u.d[i + j] = (uint32_t)(borrow ? t + BASE : t);
    }
    int64_t t = (int64_t)u.d[j + n] - (int64_t)carry - borrow;
    if (t < 0) {
      u.d[j + n] = (uint32_t)(t + BASE);
      --qhat;
      uint32_t c = 0;
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t s = u.d[i + j] + v.d[i] + c;
        c = s >= BASE;
        u.d[i + j] = c ? s - BASE : s;
      }
      u.d[j + n] = (u.d[j + n] + c) % BASE;
    } else {
      u.d[j + n] = (uint32_t)t;
    }
    if (q) {
      q->d[j] = (uint32_t)qhat;
    }
  }
  if (q) {
    nat_trim(q);
  }
  if (r) {
    u.n = n;
    nat_trim(&u);
    nat_div_small(r, &u, f);
  }
  nat_free(&u);
  nat_free(&v);
}

static void nat_isqrt(Nat *r, const Nat *a) {
  if (!a->n) {
    r->n = 0;
    return;
  }
  Nat x, y, t;
  nat_init(&x);
  nat_init(&y);
  nat_init(&t);
  nat_set_u64(&x, 1);
  nat_shift_digits(&x, &x, (nat_digits(a) + 1) / 2);
  for (;;) {
    nat_divmod(&t, NULL, a, &x);
    nat_add(&t, &t, &x);
    nat_div_small(&y, &t, 2);
    if (nat_cmp(&y, &x) >= 0) {
      break;
    }
    nat_swap(&x, &y);
  }
  nat_copy(r, &x);
  nat_free(&x);
  nat_free(&y);
  nat_free(&t);
}

typedef struct {
  Nat c;
  int64_t e;
  int neg;
} Dec;

static void dec_init(Dec *x) {
  nat_init(&x->c);
  x->e = 0;
  x->neg = 0;
}

static void dec_free(Dec *x) { nat_free(&x->c); }

static int dec_is_zero(const Dec *x) { return x->c.n == 0; }

static void dec_set_u64(Dec *x, uint64_t v, int64_t e, int neg) {
  nat_set_u64(&x->c, v);
  x->e = e;
  x->neg = v ? neg : 0;
}

static void dec_copy(Dec *r, const Dec *x) {
  nat_copy(&r->c, &x->c);
  r->e = x->e;
  r->neg = x->neg;
}

static void dec_swap(Dec *a, Dec *b) {
  Dec t = *a;
  *a = *b;
  *b = t;
}

static void dec_normalize(Dec *x) {
  if (!x->c.n) {
    x->e = 0;
    x->neg = 0;
    return;
  }
  uint64_t zeros = nat_trailing_zeros(&x->c);
  if (zeros) {
    nat_drop_digits(&x->c, &x->c, zeros);
    x->e += (int64_t)zeros;
  }
}

static void dec_load(Dec *x, int64_t coef, uint64_t meta) {
  if (is_short(meta)) {
    dec_set_u64(x, magnitude(coef), -(int64_t)scale_of(meta), coef < 0);
  } else {
    const TzDecObj *o = (const TzDecObj *)(uintptr_t)coef;
    nat_reserve(&x->c, o->n);
    memcpy(x->c.d, o->d, (size_t)o->n * sizeof(uint32_t));
    x->c.n = o->n;
    x->e = o->exp;
    x->neg = o->neg;
  }
  dec_normalize(x);
}

static int64_t dec_top(const Dec *x) { return x->c.n ? x->e + (int64_t)nat_digits(&x->c) - 1 : 0; }

static void dec_store(TzDec *out, Dec *x) {
  dec_normalize(x);
  if (!x->c.n) {
    out->coef = 0;
    out->meta = 0;
    return;
  }
  int64_t top = dec_top(x);
  if (top > MAX_EXP) {
    overflow();
  }
  if (top > MAX_EXP || top < MIN_EXP) {
    out->coef = 0;
    out->meta = 0;
    return;
  }
  uint64_t digits = nat_digits(&x->c);
  if (digits <= 19) {
    uint64_t v = 0;
    for (uint32_t i = x->c.n; i-- > 0;) {
      v = v * BASE + x->c.d[i];
    }
    uint64_t limit = x->neg ? UINT64_C(1) << 63 : (UINT64_C(1) << 63) - 1;
    if (x->e > 0 && x->e < 20 && !__builtin_mul_overflow(v, pow10_64[x->e], &v) && v <= limit) {
      out->coef = x->neg ? (int64_t)(0 - v) : (int64_t)v;
      out->meta = 0;
      return;
    }
    if (x->e <= 0 && -x->e <= (int64_t)SCALE_MAX && v <= limit) {
      out->coef = x->neg ? (int64_t)(0 - v) : (int64_t)v;
      out->meta = (uint64_t)(-x->e);
      return;
    }
  }
  TzDecObj *o = (TzDecObj *)tz_alloc(sizeof(TzDecObj) + (size_t)x->c.n * sizeof(uint32_t));
  o->rc = 1;
  o->exp = x->e;
  o->neg = x->neg;
  o->n = x->c.n;
  memcpy(o->d, x->c.d, (size_t)x->c.n * sizeof(uint32_t));
  ++tz_live_decimals;
  out->coef = (int64_t)(uintptr_t)o;
  out->meta = TZ_DEC_HEAP | TZ_DEC_LONG;
}

static void dec_store_free(TzDec *out, Dec *x) {
  dec_store(out, x);
  dec_free(x);
}

static int64_t dec_align(Nat *a, Nat *b, const Dec *x, const Dec *y) {
  int64_t e = x->e < y->e ? x->e : y->e;
  nat_shift_digits(a, &x->c, (uint64_t)(x->e - e));
  nat_shift_digits(b, &y->c, (uint64_t)(y->e - e));

  return e;
}

static int dec_cmp_abs(const Dec *x, const Dec *y) {
  if (!x->c.n || !y->c.n) {
    return x->c.n ? 1 : y->c.n ? -1 : 0;
  }
  int64_t tx = dec_top(x), ty = dec_top(y);
  if (tx != ty) {
    return tx < ty ? -1 : 1;
  }
  Nat a, b;
  nat_init(&a);
  nat_init(&b);
  dec_align(&a, &b, x, y);
  int c = nat_cmp(&a, &b);
  nat_free(&a);
  nat_free(&b);

  return c;
}

static int dec_cmp(const Dec *x, const Dec *y) {
  int sx = dec_is_zero(x) ? 0 : x->neg ? -1 : 1;
  int sy = dec_is_zero(y) ? 0 : y->neg ? -1 : 1;
  if (sx != sy) {
    return sx < sy ? -1 : 1;
  }
  int c = dec_cmp_abs(x, y);

  return sx < 0 ? -c : c;
}

static void dec_add(Dec *r, const Dec *x, const Dec *y, int negate) {
  int yneg = dec_is_zero(y) ? 0 : y->neg ^ negate;
  if (dec_is_zero(x)) {
    dec_copy(r, y);
    r->neg = yneg;
    return;
  }
  if (dec_is_zero(y)) {
    dec_copy(r, x);
    return;
  }
  Nat a, b;
  nat_init(&a);
  nat_init(&b);
  r->e = dec_align(&a, &b, x, y);
  if (x->neg == yneg) {
    nat_add(&r->c, &a, &b);
    r->neg = x->neg;
  } else {
    int c = nat_cmp(&a, &b);
    if (c == 0) {
      r->c.n = 0;
      r->neg = 0;
    } else if (c > 0) {
      nat_sub(&r->c, &a, &b);
      r->neg = x->neg;
    } else {
      nat_sub(&r->c, &b, &a);
      r->neg = yneg;
    }
  }
  nat_free(&a);
  nat_free(&b);
}

static void dec_mul(Dec *r, const Dec *x, const Dec *y) {
  nat_mul(&r->c, &x->c, &y->c);
  r->e = x->e + y->e;
  r->neg = r->c.n ? x->neg ^ y->neg : 0;
}

static int round_away(int rm, int neg, int half, int inexact, int odd) {
  switch (rm) {
  case ROUND_UP:
    return inexact;
  case ROUND_DOWN:
    return 0;
  case ROUND_CEIL:
    return inexact && !neg;
  case ROUND_FLOOR:
    return inexact && neg;
  case ROUND_HALF_UP:
    return inexact && half >= 0;
  case ROUND_HALF_DOWN:
    return half > 0;
  case ROUND_HALF_EVEN:
    return half > 0 || (half == 0 && odd);
  case ROUND_HALF_CEIL:
    return half > 0 || (half == 0 && !neg);
  default:
    return half > 0 || (half == 0 && neg);
  }
}

static void dec_round(Dec *x, int64_t dp, int rm) {
  int64_t target = -dp;
  if (!x->c.n || x->e >= target) {
    return;
  }
  uint64_t k = (uint64_t)(target - x->e);
  uint32_t digit = nat_digit(&x->c, k - 1);
  int sticky = nat_any_below(&x->c, k - 1);
  nat_drop_digits(&x->c, &x->c, k);
  int half = digit > 5 ? 1 : digit < 5 ? -1 : sticky ? 1 : 0;
  int odd = x->c.n && (x->c.d[0] & 1);
  if (round_away(rm, x->neg, half, digit || sticky, odd)) {
    nat_mul_add(&x->c, 1, 1);
  }
  x->e = target;
}

static void dec_div(Dec *r, const Dec *x, const Dec *y, int64_t dp, int rm) {
  if (dec_is_zero(y)) {
    division_by_zero();
  }
  if (dec_is_zero(x) || dec_is_zero(y)) {
    r->c.n = 0;
    r->e = 0;
    r->neg = 0;
    return;
  }
  int64_t shift = x->e - y->e + dp;
  Nat num, den, rest;
  nat_init(&num);
  nat_init(&den);
  nat_init(&rest);
  nat_shift_digits(&num, &x->c, shift > 0 ? (uint64_t)shift : 0);
  nat_shift_digits(&den, &y->c, shift < 0 ? (uint64_t)-shift : 0);
  nat_divmod(&r->c, &rest, &num, &den);
  int neg = x->neg ^ y->neg;
  int half = -1;
  if (rest.n) {
    nat_add(&rest, &rest, &rest);
    half = nat_cmp(&rest, &den);
  }
  int odd = r->c.n && (r->c.d[0] & 1);
  if (round_away(rm, neg, half, rest.n != 0, odd)) {
    nat_mul_add(&r->c, 1, 1);
  }
  r->e = -dp;
  r->neg = r->c.n ? neg : 0;
  nat_free(&num);
  nat_free(&den);
  nat_free(&rest);
}

TZ_HOT int32_t tz_dec_sign(int64_t a, uint64_t am) {
  if (is_short(am)) {
    return (a > 0) - (a < 0);
  }

  return ((const TzDecObj *)(uintptr_t)a)->neg ? -1 : 1;
}

void tz_dec_sqrt(TzDec *out, int64_t a, uint64_t am) {
  Dec x;
  dec_init(&x);
  dec_load(&x, a, am);
  if (x.neg && x.c.n) {
    tz_panicf("Square root of a negative decimal");
  }
  x.neg = 0;
  if (dec_is_zero(&x)) {
    dec_store_free(out, &x);
    return;
  }
  Nat t, s;
  nat_init(&t);
  nat_init(&s);
  nat_copy(&t, &x.c);
  nat_mul_add(&t, 4, 0);
  int64_t shift = x.e + 2 * DECIMAL_PLACES;
  if (shift >= 0) {
    nat_shift_digits(&t, &t, (uint64_t)shift);
  } else {
    nat_drop_digits(&t, &t, (uint64_t)-shift);
  }
  nat_isqrt(&s, &t);
  nat_mul_add(&s, 1, 1);
  nat_div_small(&x.c, &s, 2);
  x.e = -DECIMAL_PLACES;
  nat_free(&t);
  nat_free(&s);
  dec_store_free(out, &x);
}

void tz_dec_shifted_by(TzDec *out, int64_t a, uint64_t am, int64_t k) {
  if (k < -9007199254740991LL || k > 9007199254740991LL) {
    out_of_range(k);
    k = 0;
  }
  Dec x;
  dec_init(&x);
  dec_load(&x, a, am);
  if (!dec_is_zero(&x)) {
    if (k > 2 * (int64_t)MAX_EXP) {
      overflow();
      k = 0;
      x.c.n = 0;
    }
    x.e = k < 2 * (int64_t)MIN_EXP ? 2 * (int64_t)MIN_EXP - 100 : x.e + k;
  }
  dec_store_free(out, &x);
}

enum { FOLD_SUM, FOLD_MAX, FOLD_MIN };

void tz_dec_fold(TzDec *acc, int32_t *has, const TzDec *values, int64_t n, int32_t op) {
  for (int64_t i = 0; i < n; ++i) {
    TzDec v = values[i];
    if (!*has) {
      tz_dec_retain_value(v);
      *acc = v;
      *has = 1;
      continue;
    }
    if (op == FOLD_SUM) {
      TzDec sum;
      tz_dec_add(&sum, acc->coef, acc->meta, v.coef, v.meta);
      tz_dec_release_value(*acc);
      *acc = sum;
      continue;
    }
    int32_t c = tz_dec_cmp(v.coef, v.meta, acc->coef, acc->meta);
    if (op == FOLD_MAX ? c > 0 : c < 0) {
      tz_dec_retain_value(v);
      tz_dec_release_value(*acc);
      *acc = v;
    }
  }
}

void tz_dec_fold_array(TzDec *acc, int32_t *has, const TzArray *array, int32_t op) {
  tz_dec_fold(acc, has, (const TzDec *)array->data, array->length, op);
}

typedef struct {
  char *p;
  size_t n, cap;
} Text;

static void text_init(Text *t) {
  t->p = NULL;
  t->n = t->cap = 0;
}

static void text_reserve(Text *t, size_t more) {
  if (t->n + more <= t->cap) {
    return;
  }
  size_t cap = t->cap * 2 + more + 32;
  char *p = (char *)tz_alloc(cap);
  if (t->n) {
    memcpy(p, t->p, t->n);
  }
  tz_free(t->p);
  t->p = p;
  t->cap = cap;
}

static void text_put(Text *t, const char *s, size_t n) {
  text_reserve(t, n);
  memcpy(t->p + t->n, s, n);
  t->n += n;
}

static void text_char(Text *t, char c) { text_put(t, &c, 1); }

static void text_repeat(Text *t, char c, int64_t count) {
  if (count <= 0) {
    return;
  }
  text_reserve(t, (size_t)count);
  memset(t->p + t->n, c, (size_t)count);
  t->n += (size_t)count;
}

static TzStr text_take(Text *t) {
  TzStr s = tz_str_from_ascii(t->p, t->n);
  tz_free(t->p);
  text_init(t);

  return s;
}

static void put_coefficient(Text *t, const Dec *x) {
  if (!x->c.n) {
    text_char(t, '0');
    return;
  }
  char top[16];
  int n = snprintf(top, sizeof top, "%u", x->c.d[x->c.n - 1]);
  text_put(t, top, (size_t)n);
  for (uint32_t i = x->c.n - 1; i-- > 0;) {
    char limb[16];
    snprintf(limb, sizeof limb, "%09u", x->c.d[i]);
    text_put(t, limb, 9);
  }
}

static void put_fixed_point(Text *t, const char *s, size_t n, int64_t e) {
  if (e < 0) {
    text_put(t, "0.", 2);
    text_repeat(t, '0', -e - 1);
    text_put(t, s, n);
  } else if ((uint64_t)e + 1 >= n) {
    text_put(t, s, n);
    text_repeat(t, '0', e + 1 - (int64_t)n);
  } else {
    text_put(t, s, (size_t)e + 1);
    text_char(t, '.');
    text_put(t, s + e + 1, n - (size_t)e - 1);
  }
}

static void put_exponential(Text *t, const char *s, size_t n, int64_t e) {
  text_char(t, s[0]);
  if (n > 1) {
    text_char(t, '.');
    text_put(t, s + 1, n - 1);
  }
  char exp[24];
  int k = snprintf(exp, sizeof exp, e < 0 ? "e%lld" : "e+%lld", (long long)e);
  text_put(t, exp, (size_t)k);
}

static void format(Text *out, Dec *x, int has_i, int64_t i, int rm, int id) {
  dec_normalize(x);
  int negative = x->neg && x->c.n;
  int64_t ne = dec_top(x);
  Text s;
  text_init(&s);
  if (negative) {
    text_char(out, '-');
  }
  if (!has_i) {
    put_coefficient(&s, x);
    if (id == 1 || (id == 2 && (ne <= -7 || ne >= 21))) {
      put_exponential(out, s.p, s.n, ne);
    } else {
      put_fixed_point(out, s.p, s.n, ne);
    }
    tz_free(s.p);
    return;
  }
  dec_round(x, i - ne - 1, rm);
  dec_normalize(x);
  int64_t e = dec_top(x);
  put_coefficient(&s, x);
  int64_t len = (int64_t)s.n;
  if (id == 1 || (id == 2 && (i <= e || e <= -7))) {
    text_repeat(&s, '0', i - len);
    put_exponential(out, s.p, s.n, e);
  } else {
    i -= ne + (id == 2 && e > ne);
    put_fixed_point(out, s.p, s.n, e);
    if (e + 1 > len) {
      if (--i > 0) {
        text_char(out, '.');
        text_repeat(out, '0', i);
      }
    } else {
      i += e - len;
      if (i > 0) {
        if (e + 1 == len) {
          text_char(out, '.');
        }
        text_repeat(out, '0', i);
      }
    }
  }
  tz_free(s.p);
}

static void format_value(Text *out, int64_t a, uint64_t am, int has_i, int64_t i, int rm, int id) {
  Dec x;
  dec_init(&x);
  dec_load(&x, a, am);
  format(out, &x, has_i, i, rm, id);
  dec_free(&x);
}

static int64_t significant(int64_t a, uint64_t am) {
  Dec x;
  dec_init(&x);
  dec_load(&x, a, am);
  int64_t top = dec_top(&x);
  dec_free(&x);

  return top;
}

static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";

void tz_dec_to_string_base(TzStr *out, int64_t a, uint64_t am, int64_t base) {
  if (base < 2 || base > 36) {
    tz_panicf("internal error: toString() with the radix %lld", (long long)base);
  }
  Dec x;
  dec_init(&x);
  dec_load(&x, a, am);
  Text t;
  text_init(&t);
  if (dec_is_zero(&x)) {
    text_char(&t, '0');
    *out = text_take(&t);
    dec_free(&x);
    return;
  }
  Nat whole, fraction;
  nat_init(&whole);
  nat_init(&fraction);
  uint64_t k = x.e < 0 ? (uint64_t)-x.e : 0;
  if (x.e >= 0) {
    nat_shift_digits(&whole, &x.c, (uint64_t)x.e);
  } else {
    nat_drop_digits(&whole, &x.c, k);
    nat_copy(&fraction, &x.c);
    nat_keep_digits(&fraction, k);
  }
  uint8_t frac[DECIMAL_PLACES + 1];
  int has_fraction = fraction.n != 0;
  if (has_fraction) {
    Nat digit;
    nat_init(&digit);
    for (int j = 0; j <= DECIMAL_PLACES; ++j) {
      nat_mul_add(&fraction, (uint32_t)base, 0);
      nat_drop_digits(&digit, &fraction, k);
      frac[j] = (uint8_t)(digit.n ? digit.d[0] : 0);
      nat_keep_digits(&fraction, k);
    }
    nat_free(&digit);
  }
  int places = has_fraction ? DECIMAL_PLACES : 0;
  int round_up = has_fraction && 2 * (int)frac[DECIMAL_PLACES] >= base;
  for (int j = places; round_up && j-- > 0;) {
    round_up = ++frac[j] == base;
    if (round_up) {
      frac[j] = 0;
    }
  }
  if (round_up) {
    nat_mul_add(&whole, 1, 1);
  }
  while (places > 0 && frac[places - 1] == 0) {
    --places;
  }
  Text ints;
  text_init(&ints);
  if (!whole.n) {
    text_char(&ints, '0');
  }
  while (whole.n) {
    text_char(&ints, alphabet[nat_div_small(&whole, &whole, (uint32_t)base)]);
  }
  if (x.neg) {
    text_char(&t, '-');
  }
  for (size_t j = ints.n; j-- > 0;) {
    text_char(&t, ints.p[j]);
  }
  if (places > 0) {
    text_char(&t, '.');
    for (int j = 0; j < places; ++j) {
      text_char(&t, alphabet[frac[j]]);
    }
  }
  tz_free(ints.p);
  nat_free(&whole);
  nat_free(&fraction);
  dec_free(&x);
  *out = text_take(&t);
}

static void dec_add_product(Dec *r, const Dec *a, const Dec *q, const Dec *b) {
  Dec p;
  dec_init(&p);
  dec_mul(&p, q, b);
  dec_add(r, a, &p, 0);
  dec_free(&p);
}

TzArray *tz_dec_to_fraction(const TzType *type, int64_t a, uint64_t am, int64_t mc, uint64_t mm,
                            int32_t has_max) {
  Dec x, md, d, n0, n1, d0, d1, d2, n, q, t, xn, xd;
  Dec *all[] = {&x, &md, &d, &n0, &n1, &d0, &d1, &d2, &n, &q, &t, &xn, &xd};
  for (size_t i = 0; i < sizeof all / sizeof *all; ++i) {
    dec_init(all[i]);
  }
  dec_load(&x, a, am);
  if (has_max) {
    dec_load(&md, mc, mm);
    if (md.e < 0) {
      dec_round(&md, 0, ROUND_FLOOR);
    }
    if (md.neg || dec_is_zero(&md)) {
      dec_set_u64(&md, 1, 0, 0);
    }
  }
  int neg = x.neg;
  uint64_t digits = x.c.n ? nat_digits(&x.c) : 1;
  int64_t e = (int64_t)digits - dec_top(&x) - 1;
  dec_set_u64(&d, 1, e, 0);
  if (!has_max || dec_cmp(&md, &d) > 0) {
    if (e > 0) {
      dec_copy(&md, &d);
    } else {
      dec_set_u64(&md, 1, 0, 0);
    }
  }
  nat_copy(&n.c, &x.c);
  n.e = 0;
  n.neg = 0;
  dec_copy(&xn, &n);
  dec_copy(&xd, &d);
  dec_set_u64(&n1, 1, 0, 0);
  dec_set_u64(&d0, 1, 0, 0);
  dec_set_u64(&n0, 0, 0, 0);
  dec_set_u64(&d1, 0, 0, 0);
  for (;;) {
    if (dec_is_zero(&d)) {
      break;
    }
    dec_div(&q, &n, &d, 0, ROUND_DOWN);
    dec_add_product(&d2, &d0, &q, &d1);
    if (dec_cmp(&d2, &md) > 0) {
      break;
    }
    dec_swap(&d0, &d1);
    dec_swap(&d1, &d2);
    dec_add_product(&t, &n0, &q, &n1);
    dec_swap(&n0, &n1);
    dec_swap(&n1, &t);
    dec_mul(&t, &q, &d);
    dec_add(&d2, &n, &t, 1);
    dec_swap(&n, &d);
    dec_swap(&d, &d2);
  }
  dec_add(&t, &md, &d0, 1);
  dec_div(&d2, &t, &d1, 0, ROUND_DOWN);
  dec_add_product(&t, &n0, &d2, &n1);
  dec_swap(&n0, &t);
  dec_add_product(&t, &d0, &d2, &d1);
  dec_swap(&d0, &t);
  Dec u, v, w;
  dec_init(&u);
  dec_init(&v);
  dec_init(&w);
  dec_mul(&u, &n1, &xd);
  dec_mul(&v, &xn, &d1);
  dec_add(&w, &u, &v, 1);
  w.neg = 0;
  dec_mul(&u, &w, &d0);
  dec_mul(&v, &n0, &xd);
  dec_mul(&w, &xn, &d0);
  dec_add(&t, &v, &w, 1);
  t.neg = 0;
  dec_mul(&v, &t, &d1);
  int first = dec_cmp(&u, &v) < 1;
  Dec *num = first ? &n1 : &n0, *den = first ? &d1 : &d0;
  num->neg = num->c.n ? neg : 0;
  TzArray *result = tz_arr_new(type, 2, 2);
  TzDec *slots = (TzDec *)result->data;
  dec_store(&slots[0], num);
  dec_store(&slots[1], den);
  dec_free(&u);
  dec_free(&v);
  dec_free(&w);
  for (size_t i = 0; i < sizeof all / sizeof *all; ++i) {
    dec_free(all[i]);
  }

  return result;
}

static void narrow(Text *t, const uint16_t *s, size_t n) {
  text_reserve(t, n);
  for (size_t i = 0; i < n; ++i) {
    t->p[t->n++] = (char)s[i];
  }
}

typedef struct {
  const char *code;
  TzStr message;
  TzStr text;
} DecimalFailure;

