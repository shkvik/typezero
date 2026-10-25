// decimal: exact decimal numbers of any length with the semantics of
// BigNumber.js (docs/decimal.md), whose results programs are checked against.
//
// A value is two words (TzDec in tzrt.h). The short form is a number that
// fits in an int64 coefficient and a scale: `coef * 10^-scale`, the scale in
// the low 32 bits of `meta`. It needs no memory, and prices and the like add,
// subtract, multiply and compare with plain integer instructions: the TZ_HOT
// fast paths below, which programs inline. The short form is not normalized:
// 0.5 * 2 is 10 with scale 1, which prints and compares as 1.
//
// Anything longer is a TzDecObj: base 10^9 digits on the heap (TZ_DEC_HEAP,
// reference counted like a string) or a constant the compiler emitted for a
// long literal (TZ_DEC_LONG alone, not counted). A long value is normalized
// (no trailing zero digits) and never fits the short form.
//
// The slow paths work on Dec, a sign, a natural number of base 10^9 digits
// and a power of ten, and store results back in the shorter form. As in
// BigNumber.js, a result whose leading digit is beyond 10^MAX_EXP is an
// error (BigNumber.js gives Infinity) and one below 10^MIN_EXP is zero.

#include "runtime/core/tzrt/tzrt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE 1000000000u
#define MAX_EXP 10000000
#define MIN_EXP (-10000000)
// The scale of a short value fits in 31 bits.
#define SCALE_MAX 0x7fffffffu
// What `/`, sqrt and toString(base) round to, as BigNumber.js by default.
#define DECIMAL_PLACES 20
// The limit of the arguments of toFixed and the like, as in BigNumber.js.
#define ARG_MAX 1000000000

// The rounding modes, numbered as in BigNumber.js; CodeGen passes the number
// for a mode written as a string.
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

int64_t tz_dec_live_objects(void) { return tz_thread_sum(offsetof(TzThread, live_decimals)); }

static inline int is_short(uint64_t meta) { return (meta & TZ_DEC_LONG) == 0; }

static inline uint32_t scale_of(uint64_t meta) { return (uint32_t)meta; }

static inline uint64_t magnitude(int64_t v) { return v < 0 ? 0 - (uint64_t)v : (uint64_t)v; }

// The errors throw (see tz_throw): the function goes on to give zero, and
// the program checks for the error after calling it.
TZ_COLD static void overflow(void) { tz_throw(TZ_RANGE_ERROR, "Maximum decimal size exceeded"); }

TZ_COLD static void division_by_zero(void) { tz_throw(TZ_RANGE_ERROR, "Division by zero"); }

TZ_COLD static void out_of_range(int64_t value) {
  tz_throw(TZ_RANGE_ERROR, "Argument out of range: %lld", (long long)value);
}

static int rounding_mode(int32_t rm) {
  if (rm < ROUND_UP || rm > ROUND_HALF_FLOOR) {
    out_of_range(rm);
    return ROUND_HALF_UP;
  }
  return rm;
}

//===----------------------------------------------------------------------===//
// Reference counting
//===----------------------------------------------------------------------===//

// Frees a long value whose count reached zero. Called by the inline release
// code that the compiler emits.
void tz_dec_free(void *obj) {
  tz_free(obj);
  --tz_live_decimals;
}

void tz_dec_retain_value(TzDec v) {
  if (v.meta & TZ_DEC_HEAP)
    tz_rc_inc(&((TzDecObj *)(uintptr_t)v.coef)->rc);
}

void tz_dec_release_value(TzDec v) {
  if ((v.meta & TZ_DEC_HEAP) && tz_rc_dec(&((TzDecObj *)(uintptr_t)v.coef)->rc))
    tz_dec_free((void *)(uintptr_t)v.coef);
}

#if TZ_PARALLEL
// publish.c: a decimal other threads may reach.
void tz_dec_publish(TzDec v) {
  if (!(v.meta & TZ_DEC_HEAP))
    return;
  TzDecObj *o = (TzDecObj *)(uintptr_t)v.coef;
  if (o->rc > 0 && o->rc < TZ_RC_FOREVER)
    o->rc = TZ_RC_SHARED + o->rc;
}
#endif

//===----------------------------------------------------------------------===//
// Natural numbers in base 10^9, the lowest digit first
//===----------------------------------------------------------------------===//

typedef struct {
  uint32_t *d;
  uint32_t n, cap; // n == 0 is zero; d[n - 1] != 0 otherwise
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
  if (cap <= x->cap)
    return;
  // Far more than the exponent range lets a value have: no result that
  // fits comes this far.
  if (cap > (UINT64_C(1) << 28))
    tz_panicf("RangeError: Maximum decimal size exceeded");
  uint64_t grown = (uint64_t)x->cap * 2;
  uint32_t c = (uint32_t)(cap < 4 ? 4 : cap > grown ? cap : grown);
  uint32_t *d = (uint32_t *)tz_alloc((size_t)c * sizeof(uint32_t));
  if (x->n)
    memcpy(d, x->d, (size_t)x->n * sizeof(uint32_t));
  tz_free(x->d);
  x->d = d;
  x->cap = c;
}

static void nat_trim(Nat *x) {
  while (x->n > 0 && x->d[x->n - 1] == 0)
    --x->n;
}

static void nat_set_u64(Nat *x, uint64_t v) {
  nat_reserve(x, 3);
  x->n = 0;
  for (; v; v /= BASE)
    x->d[x->n++] = (uint32_t)(v % BASE);
}

static void nat_copy(Nat *r, const Nat *a) {
  if (r == a)
    return;
  nat_reserve(r, a->n);
  if (a->n)
    memcpy(r->d, a->d, (size_t)a->n * sizeof(uint32_t));
  r->n = a->n;
}

static void nat_swap(Nat *a, Nat *b) {
  Nat t = *a;
  *a = *b;
  *b = t;
}

static int nat_cmp(const Nat *a, const Nat *b) {
  if (a->n != b->n)
    return a->n < b->n ? -1 : 1;
  for (uint32_t i = a->n; i-- > 0;)
    if (a->d[i] != b->d[i])
      return a->d[i] < b->d[i] ? -1 : 1;
  return 0;
}

// r = a + b; r may be a or b.
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
  if (carry)
    r->d[r->n++] = 1;
}

// r = a - b for a >= b; r may be a or b.
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

// r = a * b; r must be neither a nor b.
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
    if (!ai)
      continue;
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

// x = x * m + add, for m <= BASE.
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

// q = a / m, returning a % m; q may be a.
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
  while (k < 10 && v >= pow10_32[k])
    ++k;
  return k;
}

// The number of decimal digits; 0 for zero.
static uint64_t nat_digits(const Nat *a) {
  return a->n ? (uint64_t)(a->n - 1) * 9 + (uint64_t)digits_u32(a->d[a->n - 1]) : 0;
}

// The decimal digit at position p, counting from the lowest at 0.
static uint32_t nat_digit(const Nat *a, uint64_t p) {
  uint64_t i = p / 9;
  return i < a->n ? a->d[i] / pow10_32[p % 9] % 10 : 0;
}

// Whether some digit below position p is not zero.
static int nat_any_below(const Nat *a, uint64_t p) {
  uint64_t i = p / 9;
  uint64_t whole = i < a->n ? i : a->n;
  for (uint64_t k = 0; k < whole; ++k)
    if (a->d[k])
      return 1;
  return i < a->n && a->d[i] % pow10_32[p % 9] != 0;
}

static uint64_t nat_trailing_zeros(const Nat *a) {
  uint64_t zeros = 0;
  uint32_t i = 0;
  while (i < a->n && a->d[i] == 0) {
    zeros += 9;
    ++i;
  }
  if (i < a->n)
    for (uint32_t v = a->d[i]; v % 10 == 0; v /= 10)
      ++zeros;
  return zeros;
}

// r = a / 10^k, rounded down; r may be a.
static void nat_drop_digits(Nat *r, const Nat *a, uint64_t k) {
  uint64_t limbs = k / 9;
  unsigned rest = (unsigned)(k % 9);
  if (limbs >= a->n) {
    r->n = 0;
    return;
  }
  uint32_t an = a->n, n = an - (uint32_t)limbs;
  nat_reserve(r, an); // a no larger reserve, which keeps a->d valid when r == a
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

// x = x mod 10^k
static void nat_keep_digits(Nat *x, uint64_t k) {
  uint64_t limbs = k / 9;
  unsigned rest = (unsigned)(k % 9);
  if (limbs >= x->n)
    return;
  if (rest) {
    x->d[limbs] %= pow10_32[rest];
    x->n = (uint32_t)limbs + 1;
  } else {
    x->n = (uint32_t)limbs;
  }
  nat_trim(x);
}

// r = a * 10^k; r may be a.
static void nat_shift_digits(Nat *r, const Nat *a, uint64_t k) {
  nat_copy(r, a);
  if (!r->n || !k)
    return;
  unsigned rest = (unsigned)(k % 9);
  uint64_t limbs = k / 9;
  if (rest)
    nat_mul_add(r, pow10_32[rest], 0);
  if (limbs) {
    nat_reserve(r, r->n + limbs);
    memmove(r->d + limbs, r->d, (size_t)r->n * sizeof(uint32_t));
    memset(r->d, 0, (size_t)limbs * sizeof(uint32_t));
    r->n += (uint32_t)limbs;
  }
}

// q = a / b and r = a % b for b != 0, by Knuth's algorithm D. q and r may be
// NULL, and must not be a or b.
static void nat_divmod(Nat *q, Nat *r, const Nat *a, const Nat *b) {
  if (nat_cmp(a, b) < 0) {
    if (q)
      q->n = 0;
    if (r)
      nat_copy(r, a);
    return;
  }
  if (b->n == 1) {
    Nat scratch;
    nat_init(&scratch);
    uint32_t rest = nat_div_small(q ? q : &scratch, a, b->d[0]);
    nat_free(&scratch);
    if (r)
      nat_set_u64(r, rest);
    return;
  }
  uint32_t n = b->n, m = a->n - b->n;
  // Scaling both makes the top digit of the divisor at least BASE / 2, so
  // that the estimated quotient digits are at most two too large.
  uint32_t f = BASE / (b->d[n - 1] + 1);
  Nat u, v;
  nat_init(&u);
  nat_init(&v);
  nat_copy(&u, a);
  nat_copy(&v, b);
  nat_mul_add(&u, f, 0);
  nat_mul_add(&v, f, 0);
  nat_reserve(&u, (uint64_t)a->n + 1);
  while (u.n < a->n + 1)
    u.d[u.n++] = 0;
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
      if (rhat >= BASE)
        break;
    }
    // u[j..j+n] -= qhat * v
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
      // qhat was one too large: add v back.
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
    if (q)
      q->d[j] = (uint32_t)qhat;
  }
  if (q)
    nat_trim(q);
  if (r) {
    u.n = n;
    nat_trim(&u);
    nat_div_small(r, &u, f);
  }
  nat_free(&u);
  nat_free(&v);
}

// r = floor(sqrt(a)), by Newton's method from above.
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
  nat_shift_digits(&x, &x, (nat_digits(a) + 1) / 2); // above sqrt(a)
  for (;;) {
    nat_divmod(&t, NULL, a, &x);
    nat_add(&t, &t, &x);
    nat_div_small(&y, &t, 2);
    if (nat_cmp(&y, &x) >= 0)
      break;
    nat_swap(&x, &y);
  }
  nat_copy(r, &x);
  nat_free(&x);
  nat_free(&y);
  nat_free(&t);
}

//===----------------------------------------------------------------------===//
// Decimals of any length
//===----------------------------------------------------------------------===//

// (-1)^neg * c * 10^e; zero is never negative.
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

// Drops the trailing zero digits into the exponent.
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

// The exponent of the leading digit, which BigNumber.js calls `e`; 0 for zero.
static int64_t dec_top(const Dec *x) { return x->c.n ? x->e + (int64_t)nat_digits(&x->c) - 1 : 0; }

// Stores `x` as a value of the language, short if it fits; frees nothing.
static void dec_store(TzDec *out, Dec *x) {
  dec_normalize(x);
  if (!x->c.n) {
    out->coef = 0;
    out->meta = 0;
    return;
  }
  int64_t top = dec_top(x);
  if (top > MAX_EXP)
    overflow();
  if (top > MAX_EXP || top < MIN_EXP) {
    out->coef = 0;
    out->meta = 0;
    return;
  }
  uint64_t digits = nat_digits(&x->c);
  if (digits <= 19) {
    uint64_t v = 0;
    for (uint32_t i = x->c.n; i-- > 0;)
      v = v * BASE + x->c.d[i];
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

// The digits of x and y over the exponent of the one with more decimals.
static int64_t dec_align(Nat *a, Nat *b, const Dec *x, const Dec *y) {
  int64_t e = x->e < y->e ? x->e : y->e;
  nat_shift_digits(a, &x->c, (uint64_t)(x->e - e));
  nat_shift_digits(b, &y->c, (uint64_t)(y->e - e));
  return e;
}

static int dec_cmp_abs(const Dec *x, const Dec *y) {
  if (!x->c.n || !y->c.n)
    return x->c.n ? 1 : y->c.n ? -1 : 0;
  int64_t tx = dec_top(x), ty = dec_top(y);
  if (tx != ty)
    return tx < ty ? -1 : 1;
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
  if (sx != sy)
    return sx < sy ? -1 : 1;
  int c = dec_cmp_abs(x, y);
  return sx < 0 ? -c : c;
}

// r = x + y, or x - y with `negate`; r must be neither.
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

// r = x * y; r must be neither.
static void dec_mul(Dec *r, const Dec *x, const Dec *y) {
  nat_mul(&r->c, &x->c, &y->c);
  r->e = x->e + y->e;
  r->neg = r->c.n ? x->neg ^ y->neg : 0;
}

// Whether a result whose dropped part is `half` (-1 below a half, 0 exactly
// a half, 1 above) and not zero when `inexact` is rounded away from zero.
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
    return half > 0 || (half == 0 && neg); // ROUND_HALF_FLOOR
  }
}

// Rounds x to dp decimal places (a negative dp rounds to tens, hundreds...).
// Keeps the sign of a value that rounds to zero, for formatting.
static void dec_round(Dec *x, int64_t dp, int rm) {
  int64_t target = -dp;
  if (!x->c.n || x->e >= target)
    return;
  uint64_t k = (uint64_t)(target - x->e); // digits to drop
  uint32_t digit = nat_digit(&x->c, k - 1);
  int sticky = nat_any_below(&x->c, k - 1);
  nat_drop_digits(&x->c, &x->c, k);
  int half = digit > 5 ? 1 : digit < 5 ? -1 : sticky ? 1 : 0;
  int odd = x->c.n && (x->c.d[0] & 1);
  if (round_away(rm, x->neg, half, digit || sticky, odd))
    nat_mul_add(&x->c, 1, 1);
  x->e = target;
}

// r = x / y rounded to dp decimal places; r must be neither.
static void dec_div(Dec *r, const Dec *x, const Dec *y, int64_t dp, int rm) {
  if (dec_is_zero(y))
    division_by_zero();
  if (dec_is_zero(x) || dec_is_zero(y)) {
    r->c.n = 0;
    r->e = 0;
    r->neg = 0;
    return;
  }
  // q = x.c * 10^(x.e - y.e + dp) / y.c is the result in units of 10^-dp.
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
  if (round_away(rm, neg, half, rest.n != 0, odd))
    nat_mul_add(&r->c, 1, 1);
  r->e = -dp;
  r->neg = r->c.n ? neg : 0;
  nat_free(&num);
  nat_free(&den);
  nat_free(&rest);
}

// r = x % y with the sign of x, as JavaScript's % and BigNumber.js's mod.
static void dec_rem(Dec *r, const Dec *x, const Dec *y) {
  if (dec_is_zero(y)) {
    division_by_zero();
    r->c.n = 0;
    r->e = 0;
    r->neg = 0;
    return;
  }
  Nat a, b;
  nat_init(&a);
  nat_init(&b);
  r->e = dec_align(&a, &b, x, y);
  nat_divmod(NULL, &r->c, &a, &b);
  r->neg = r->c.n ? x->neg : 0;
  nat_free(&a);
  nat_free(&b);
}

// log10(|x|) for x != 0, roughly.
static double dec_log10(const Dec *x) {
  double lead = 0;
  for (uint32_t i = x->c.n, k = 0; i-- > 0 && k < 3; ++k)
    lead = lead * BASE + x->c.d[i];
  uint32_t used = x->c.n < 3 ? x->c.n : 3;
  return log10(lead) + 9.0 * (double)(x->c.n - used) + (double)x->e;
}

// r = x^n, exactly for n >= 0; for n < 0, 1 / x^-n rounded as `/` rounds.
static void dec_pow(Dec *r, const Dec *x, int64_t n) {
  if (n == 0) {
    dec_set_u64(r, 1, 0, 0);
    return;
  }
  if (dec_is_zero(x)) {
    if (n < 0)
      division_by_zero();
    dec_set_u64(r, 0, 0, 0);
    return;
  }
  uint64_t m = magnitude(n);
  // The result's size, to stop before computing one that is out of range
  // (or that 1 / x^m turns into a zero).
  double size = dec_log10(x) * (double)n;
  if (size > MAX_EXP + 2.0)
    overflow();
  if (size > MAX_EXP + 2.0 || size < (n < 0 ? -(DECIMAL_PLACES + 2.0) : MIN_EXP - 2.0)) {
    dec_set_u64(r, 0, 0, 0);
    return;
  }
  Dec base, acc, t;
  dec_init(&base);
  dec_init(&acc);
  dec_init(&t);
  dec_copy(&base, x);
  dec_set_u64(&acc, 1, 0, 0);
  for (;;) {
    if (m & 1) {
      dec_mul(&t, &acc, &base);
      dec_swap(&t, &acc);
      dec_normalize(&acc);
    }
    m >>= 1;
    if (!m)
      break;
    dec_mul(&t, &base, &base);
    dec_swap(&t, &base);
    dec_normalize(&base);
  }
  if (n < 0) {
    Dec one;
    dec_init(&one);
    dec_set_u64(&one, 1, 0, 0);
    dec_div(r, &one, &acc, DECIMAL_PLACES, ROUND_HALF_UP);
    dec_free(&one);
  } else {
    dec_swap(r, &acc);
  }
  dec_free(&base);
  dec_free(&acc);
  dec_free(&t);
}

//===----------------------------------------------------------------------===//
// Arithmetic of values
//===----------------------------------------------------------------------===//

// x * 10^k, if it fits in an int64.
static int scale_up(int64_t *x, uint32_t k) {
  if (k >= 19)
    return *x == 0;
  return !__builtin_mul_overflow(*x, (int64_t)pow10_64[k], x);
}

TZ_NOINLINE void tz_dec_add_slow(TzDec *out, int64_t a, uint64_t am, int64_t b, uint64_t bm,
                                 int32_t negate) {
  if (is_short(am) && is_short(bm)) {
    // Brought to one scale, short values add as integers unless the sum
    // overflows.
    uint32_t sa = scale_of(am), sb = scale_of(bm);
    int64_t x = a, y = b, sum;
    int fits = sa < sb ? scale_up(&x, sb - sa) : scale_up(&y, sa - sb);
    if (fits && negate)
      fits = y != INT64_MIN ? (y = -y, 1) : 0;
    if (fits && !__builtin_add_overflow(x, y, &sum)) {
      out->coef = sum;
      out->meta = sa > sb ? sa : sb;
      return;
    }
  }
  Dec x, y, r;
  dec_init(&x);
  dec_init(&y);
  dec_init(&r);
  dec_load(&x, a, am);
  dec_load(&y, b, bm);
  dec_add(&r, &x, &y, negate);
  dec_store_free(out, &r);
  dec_free(&x);
  dec_free(&y);
}

TZ_HOT void tz_dec_add(TzDec *out, int64_t a, uint64_t am, int64_t b, uint64_t bm) {
  int64_t sum;
  if (am == bm && is_short(am) && !__builtin_add_overflow(a, b, &sum)) {
    out->coef = sum;
    out->meta = am;
    return;
  }
  tz_dec_add_slow(out, a, am, b, bm, 0);
}

TZ_HOT void tz_dec_sub(TzDec *out, int64_t a, uint64_t am, int64_t b, uint64_t bm) {
  int64_t difference;
  if (am == bm && is_short(am) && !__builtin_sub_overflow(a, b, &difference)) {
    out->coef = difference;
    out->meta = am;
    return;
  }
  tz_dec_add_slow(out, a, am, b, bm, 1);
}

TZ_NOINLINE void tz_dec_mul_slow(TzDec *out, int64_t a, uint64_t am, int64_t b, uint64_t bm) {
  Dec x, y, r;
  dec_init(&x);
  dec_init(&y);
  dec_init(&r);
  dec_load(&x, a, am);
  dec_load(&y, b, bm);
  dec_mul(&r, &x, &y);
  dec_store_free(out, &r);
  dec_free(&x);
  dec_free(&y);
}

TZ_HOT void tz_dec_mul(TzDec *out, int64_t a, uint64_t am, int64_t b, uint64_t bm) {
  int64_t product;
  uint64_t scale = am + bm;
  if (is_short(am | bm) && scale <= SCALE_MAX && !__builtin_mul_overflow(a, b, &product)) {
    out->coef = product;
    out->meta = scale;
    return;
  }
  tz_dec_mul_slow(out, a, am, b, bm);
}

TZ_NOINLINE void tz_dec_neg_slow(TzDec *out, int64_t a, uint64_t am) {
  Dec x;
  dec_init(&x);
  dec_load(&x, a, am);
  x.neg = x.c.n ? !x.neg : 0;
  dec_store_free(out, &x);
}

TZ_HOT void tz_dec_neg(TzDec *out, int64_t a, uint64_t am) {
  if (is_short(am) && a != INT64_MIN) {
    out->coef = -a;
    out->meta = am;
    return;
  }
  tz_dec_neg_slow(out, a, am);
}

void tz_dec_abs(TzDec *out, int64_t a, uint64_t am) {
  if (is_short(am) ? a >= 0 : !((const TzDecObj *)(uintptr_t)a)->neg) {
    out->coef = a;
    out->meta = am;
    tz_dec_retain_value(*out);
    return;
  }
  tz_dec_neg(out, a, am);
}

// -1, 0 or 1.
TZ_NOINLINE int32_t tz_dec_cmp_slow(int64_t a, uint64_t am, int64_t b, uint64_t bm) {
  if (is_short(am) && is_short(bm)) {
    uint32_t sa = scale_of(am), sb = scale_of(bm);
    int64_t x = a, y = b;
    // A value that does not fit in an int64 when scaled up is beyond any
    // that does.
    if (sa < sb && !scale_up(&x, sb - sa))
      return a < 0 ? -1 : 1;
    if (sb < sa && !scale_up(&y, sa - sb))
      return b < 0 ? 1 : -1;
    return (x > y) - (x < y);
  }
  Dec x, y;
  dec_init(&x);
  dec_init(&y);
  dec_load(&x, a, am);
  dec_load(&y, b, bm);
  int c = dec_cmp(&x, &y);
  dec_free(&x);
  dec_free(&y);
  return c;
}

TZ_HOT int32_t tz_dec_cmp(int64_t a, uint64_t am, int64_t b, uint64_t bm) {
  if (am == bm && is_short(am))
    return (a > b) - (a < b);
  return tz_dec_cmp_slow(a, am, b, bm);
}

TZ_HOT int32_t tz_dec_equal(int64_t a, uint64_t am, int64_t b, uint64_t bm) {
  if (am == bm && is_short(am))
    return a == b;
  return tz_dec_cmp_slow(a, am, b, bm) == 0;
}

// The sign of a comparison function's result: -1, 0 or 1.
TZ_HOT int32_t tz_dec_sign(int64_t a, uint64_t am) {
  if (is_short(am))
    return (a > 0) - (a < 0);
  return ((const TzDecObj *)(uintptr_t)a)->neg ? -1 : 1;
}

// x / y rounded to dp decimal places: `/` passes 20 and half-up.
void tz_dec_div(TzDec *out, int64_t a, uint64_t am, int64_t b, uint64_t bm, int64_t dp,
                int32_t rm) {
  if (dp < 0 || dp > ARG_MAX) {
    out_of_range(dp);
    dp = 0;
  }
  rm = rounding_mode(rm);
  Dec x, y, r;
  dec_init(&x);
  dec_init(&y);
  dec_init(&r);
  dec_load(&x, a, am);
  dec_load(&y, b, bm);
  dec_div(&r, &x, &y, dp, rm);
  dec_store_free(out, &r);
  dec_free(&x);
  dec_free(&y);
}

void tz_dec_rem(TzDec *out, int64_t a, uint64_t am, int64_t b, uint64_t bm) {
  if (is_short(am) && is_short(bm) && b != 0) {
    uint32_t sa = scale_of(am), sb = scale_of(bm);
    int64_t x = a, y = b;
    int fits = sa < sb ? scale_up(&x, sb - sa) : scale_up(&y, sa - sb);
    if (fits && y != -1) { // INT64_MIN % -1 traps
      out->coef = x % y;
      out->meta = sa > sb ? sa : sb;
      return;
    }
  }
  Dec x, y, r;
  dec_init(&x);
  dec_init(&y);
  dec_init(&r);
  dec_load(&x, a, am);
  dec_load(&y, b, bm);
  dec_rem(&r, &x, &y);
  dec_store_free(out, &r);
  dec_free(&x);
  dec_free(&y);
}

void tz_dec_pow(TzDec *out, int64_t a, uint64_t am, int64_t n) {
  Dec x, r;
  dec_init(&x);
  dec_init(&r);
  dec_load(&x, a, am);
  dec_pow(&r, &x, n);
  dec_store_free(out, &r);
  dec_free(&x);
}

//===----------------------------------------------------------------------===//
// Methods
//===----------------------------------------------------------------------===//

// round(dp, mode): BigNumber.js's decimalPlaces(dp, mode).
void tz_dec_round(TzDec *out, int64_t a, uint64_t am, int64_t dp, int32_t rm) {
  if (dp < -ARG_MAX || dp > ARG_MAX) {
    out_of_range(dp);
    dp = 0;
  }
  Dec x;
  dec_init(&x);
  dec_load(&x, a, am);
  dec_round(&x, dp, rounding_mode(rm));
  if (!x.c.n)
    x.neg = 0;
  dec_store_free(out, &x);
}

void tz_dec_sqrt(TzDec *out, int64_t a, uint64_t am) {
  Dec x;
  dec_init(&x);
  dec_load(&x, a, am);
  if (x.neg) {
    tz_throw(TZ_RANGE_ERROR, "Square root of a negative decimal");
    x.c.n = 0;
    x.neg = 0;
  }
  if (dec_is_zero(&x)) {
    dec_store_free(out, &x);
    return;
  }
  // With y = sqrt(x) * 10^20, the result is y rounded half up, which is
  // floor((floor(2y) + 1) / 2), and floor(2y) = isqrt(floor(4x * 10^40)).
  Nat t, s;
  nat_init(&t);
  nat_init(&s);
  nat_copy(&t, &x.c);
  nat_mul_add(&t, 4, 0);
  int64_t shift = x.e + 2 * DECIMAL_PLACES;
  if (shift >= 0)
    nat_shift_digits(&t, &t, (uint64_t)shift);
  else
    nat_drop_digits(&t, &t, (uint64_t)-shift);
  nat_isqrt(&s, &t);
  nat_mul_add(&s, 1, 1);
  nat_div_small(&x.c, &s, 2);
  x.e = -DECIMAL_PLACES;
  nat_free(&t);
  nat_free(&s);
  dec_store_free(out, &x);
}

int32_t tz_dec_is_integer(int64_t a, uint64_t am) {
  if (is_short(am)) {
    uint32_t scale = scale_of(am);
    return scale == 0 || (scale < 19 ? a % (int64_t)pow10_64[scale] == 0 : a == 0);
  }
  return ((const TzDecObj *)(uintptr_t)a)->exp >= 0; // normalized
}

// decimalPlaces(): the number of digits after the point.
int32_t tz_dec_decimal_places(int64_t a, uint64_t am) {
  Dec x;
  dec_init(&x);
  dec_load(&x, a, am);
  int64_t places = x.e < 0 ? -x.e : 0;
  dec_free(&x);
  return (int32_t)places;
}

// shiftedBy(k): x * 10^k.
void tz_dec_shifted_by(TzDec *out, int64_t a, uint64_t am, int64_t k) {
  if (k < -9007199254740991LL || k > 9007199254740991LL) {
    out_of_range(k);
    k = 0;
  }
  Dec x;
  dec_init(&x);
  dec_load(&x, a, am);
  if (!dec_is_zero(&x)) {
    // Out of range either way, and the exponent must not wrap around.
    if (k > 2 * (int64_t)MAX_EXP) {
      overflow();
      k = 0;
      x.c.n = 0;
    }
    x.e = k < 2 * (int64_t)MIN_EXP ? 2 * (int64_t)MIN_EXP - 100 : x.e + k;
  }
  dec_store_free(out, &x);
}

// decimal.sum, decimal.max and decimal.min over `n` values: `acc` holds the
// result so far (owned) and `*has` says whether there is one.
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

TZ_COLD void tz_dec_no_values(int32_t op) {
  tz_throw(TZ_RANGE_ERROR, "decimal.%s() of no values", op == FOLD_MAX ? "max" : "min");
}

//===----------------------------------------------------------------------===//
// Text
//===----------------------------------------------------------------------===//

// A growable ASCII buffer.
typedef struct {
  char *p;
  size_t n, cap;
} Text;

static void text_init(Text *t) {
  t->p = NULL;
  t->n = t->cap = 0;
}

static void text_reserve(Text *t, size_t more) {
  if (t->n + more <= t->cap)
    return;
  size_t cap = t->cap * 2 + more + 32;
  char *p = (char *)tz_alloc(cap);
  if (t->n)
    memcpy(p, t->p, t->n);
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
  if (count <= 0)
    return;
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

// The digits of |x| (normalized), most significant first, as BigNumber.js's
// coeffToString gives them: "0" for zero.
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

// toFixedPoint of BigNumber.js: the digits `s` (n of them) with the leading
// one at 10^e, without an exponent.
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

// toExponential of BigNumber.js: "1.25e+3".
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

// String(x): the plain digits with a point, never an exponent.
static void put_plain(Text *t, Dec *x) {
  dec_normalize(x);
  Text digits;
  text_init(&digits);
  put_coefficient(&digits, x);
  if (x->neg)
    text_char(t, '-');
  put_fixed_point(t, digits.p, digits.n, dec_top(x));
  tz_free(digits.p);
}

// The text of a short value, if short enough for `out` (64 bytes).
static size_t format_short(char *out, int64_t coef, uint32_t scale) {
  uint64_t mag = magnitude(coef);
  while (scale > 0 && mag % 10 == 0) {
    mag /= 10;
    --scale;
  }
  char digits[24];
  int n = 0;
  do {
    digits[sizeof digits - 1 - n++] = (char)('0' + mag % 10);
    mag /= 10;
  } while (mag);
  const char *d = digits + sizeof digits - n;
  size_t len = 0;
  if (coef < 0)
    out[len++] = '-';
  if (scale == 0) {
    memcpy(out + len, d, (size_t)n);
    return len + (size_t)n;
  }
  if ((uint32_t)n > scale) {
    memcpy(out + len, d, (size_t)n - scale);
    len += (size_t)n - scale;
    out[len++] = '.';
    memcpy(out + len, d + n - scale, scale);
    return len + scale;
  }
  out[len++] = '0';
  out[len++] = '.';
  memset(out + len, '0', scale - (uint32_t)n);
  len += scale - (uint32_t)n;
  memcpy(out + len, d, (size_t)n);
  return len + (size_t)n;
}

static int fits_short_text(uint64_t meta) { return is_short(meta) && scale_of(meta) <= 30; }

static void put_value(Text *t, int64_t a, uint64_t am) {
  if (fits_short_text(am)) {
    char buf[64];
    text_put(t, buf, format_short(buf, a, scale_of(am)));
    return;
  }
  Dec x;
  dec_init(&x);
  dec_load(&x, a, am);
  put_plain(t, &x);
  dec_free(&x);
}

void tz_dec_to_string(TzStr *out, int64_t a, uint64_t am) {
  if (fits_short_text(am)) {
    char buf[64];
    *out = tz_str_from_ascii(buf, format_short(buf, a, scale_of(am)));
    return;
  }
  Text t;
  text_init(&t);
  put_value(&t, a, am);
  *out = text_take(&t);
}

void tz_print_decimal(int64_t a, uint64_t am) {
  if (fits_short_text(am)) {
    char buf[64];
    tz_out_write(buf, format_short(buf, a, scale_of(am)));
    return;
  }
  Text t;
  text_init(&t);
  put_value(&t, a, am);
  tz_out_write(t.p, t.n);
  tz_free(t.p);
}

void tz_buf_decimal(TzBuf *out, TzDec v) {
  Text t;
  text_init(&t);
  put_value(&t, v.coef, v.meta);
  tz_buf_ascii_n(out, t.p, t.n);
  tz_free(t.p);
}

// format() of BigNumber.js: toFixed (id 0), toExponential (id 1) and
// toPrecision (id 2). With `has_i`, the value is rounded to i significant
// digits first. As in BigNumber.js, a negative value that rounds to zero
// keeps its minus sign: decimal("-0.001").toFixed(2) is "-0.00".
static void format(Text *out, Dec *x, int has_i, int64_t i, int rm, int id) {
  dec_normalize(x);
  int negative = x->neg && x->c.n;
  int64_t ne = dec_top(x);
  Text s;
  text_init(&s);
  if (negative)
    text_char(out, '-');
  if (!has_i) {
    put_coefficient(&s, x);
    if (id == 1 || (id == 2 && (ne <= -7 || ne >= 21)))
      put_exponential(out, s.p, s.n, ne);
    else
      put_fixed_point(out, s.p, s.n, ne);
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
        if (e + 1 == len)
          text_char(out, '.');
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

void tz_dec_to_fixed(TzStr *out, int64_t a, uint64_t am, int64_t dp, int32_t has_dp, int32_t rm) {
  if (has_dp && (dp < -ARG_MAX || dp > ARG_MAX)) {
    out_of_range(dp);
    dp = 0;
  }
  rm = rounding_mode(rm);
  Text t;
  text_init(&t);
  format_value(&t, a, am, has_dp, has_dp ? dp + significant(a, am) + 1 : 0, rm, 0);
  *out = text_take(&t);
}

void tz_dec_to_exponential(TzStr *out, int64_t a, uint64_t am, int64_t dp, int32_t has_dp,
                           int32_t rm) {
  if (has_dp && (dp < 0 || dp > ARG_MAX)) {
    out_of_range(dp);
    dp = 0;
  }
  rm = rounding_mode(rm);
  Text t;
  text_init(&t);
  format_value(&t, a, am, has_dp, dp + 1, rm, 1);
  *out = text_take(&t);
}

void tz_dec_to_precision(TzStr *out, int64_t a, uint64_t am, int64_t sd, int32_t has_sd,
                         int32_t rm) {
  if (has_sd && (sd < 1 || sd > ARG_MAX)) {
    out_of_range(sd);
    sd = 1;
  }
  rm = rounding_mode(rm);
  Text t;
  text_init(&t);
  format_value(&t, a, am, has_sd, sd, rm, 2);
  *out = text_take(&t);
}

// toFormat(dp, mode): toFixed with the integer digits in groups of three,
// as BigNumber.js's default FORMAT has it: "1,234,567.89".
void tz_dec_to_format(TzStr *out, int64_t a, uint64_t am, int64_t dp, int32_t has_dp, int32_t rm) {
  if (has_dp && (dp < -ARG_MAX || dp > ARG_MAX)) {
    out_of_range(dp);
    dp = 0;
  }
  rm = rounding_mode(rm);
  Text fixed;
  text_init(&fixed);
  format_value(&fixed, a, am, has_dp, has_dp ? dp + significant(a, am) + 1 : 0, rm, 0);
  const char *s = fixed.p;
  size_t n = fixed.n;
  Text t;
  text_init(&t);
  if (n && s[0] == '-') {
    text_char(&t, '-');
    ++s;
    --n;
  }
  size_t whole = 0;
  while (whole < n && s[whole] != '.')
    ++whole;
  size_t first = whole % 3 ? whole % 3 : 3;
  text_put(&t, s, first < whole ? first : whole);
  for (size_t k = first; k < whole; k += 3) {
    text_char(&t, ',');
    text_put(&t, s + k, 3);
  }
  text_put(&t, s + whole, n - whole);
  tz_free(fixed.p);
  *out = text_take(&t);
}

static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";

// toString(base), as BigNumber.js: a fraction is rounded half up to 20
// digits of that base, where a rounding digit of at least base / 2 rounds up.
void tz_dec_to_string_base(TzStr *out, int64_t a, uint64_t am, int64_t base) {
  if (base < 2 || base > 36) {
    tz_throw(TZ_RANGE_ERROR, "toString() radix must be between 2 and 36");
    base = 10;
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
  // The integer part, and the fraction as f / 10^k.
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
  // The fraction's digits in the base, one more than kept, to round.
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
    if (round_up)
      frac[j] = 0;
  }
  if (round_up)
    nat_mul_add(&whole, 1, 1);
  while (places > 0 && frac[places - 1] == 0)
    --places;
  // The integer digits, from the lowest.
  Text ints;
  text_init(&ints);
  if (!whole.n)
    text_char(&ints, '0');
  while (whole.n)
    text_char(&ints, alphabet[nat_div_small(&whole, &whole, (uint32_t)base)]);
  if (x.neg)
    text_char(&t, '-');
  for (size_t j = ints.n; j-- > 0;)
    text_char(&t, ints.p[j]);
  if (places > 0) {
    text_char(&t, '.');
    for (int j = 0; j < places; ++j)
      text_char(&t, alphabet[frac[j]]);
  }
  tz_free(ints.p);
  nat_free(&whole);
  nat_free(&fraction);
  dec_free(&x);
  *out = text_take(&t);
}

//===----------------------------------------------------------------------===//
// toFraction
//===----------------------------------------------------------------------===//

// r = a + q * b
static void dec_add_product(Dec *r, const Dec *a, const Dec *q, const Dec *b) {
  Dec p;
  dec_init(&p);
  dec_mul(&p, q, b);
  dec_add(r, a, &p, 0);
  dec_free(&p);
}

// toFraction(maxDenominator) of BigNumber.js: the fraction with the
// smallest denominator up to the maximum (or the exact one) that is closest
// to x, as an array [numerator, denominator].
TzArray *tz_dec_to_fraction(const TzType *type, int64_t a, uint64_t am, int64_t mc, uint64_t mm,
                            int32_t has_max) {
  Dec x, md, d, n0, n1, d0, d1, d2, n, q, t, xn, xd;
  Dec *all[] = {&x, &md, &d, &n0, &n1, &d0, &d1, &d2, &n, &q, &t, &xn, &xd};
  for (size_t i = 0; i < sizeof all / sizeof *all; ++i)
    dec_init(all[i]);
  dec_load(&x, a, am);
  if (has_max) {
    dec_load(&md, mc, mm);
    if (md.e < 0 || md.neg || dec_is_zero(&md)) {
      tz_throw(TZ_RANGE_ERROR, md.e < 0
                                   ? "toFraction() maximum denominator is not an integer"
                                   : "toFraction() maximum denominator must be at least 1");
      has_max = 0;
    }
  }
  int neg = x.neg;
  // |x| = n / d exactly, with d a power of ten (below 1 for an integer that
  // ends in zeros).
  uint64_t digits = x.c.n ? nat_digits(&x.c) : 1;
  int64_t e = (int64_t)digits - dec_top(&x) - 1;
  dec_set_u64(&d, 1, e, 0);
  if (!has_max || dec_cmp(&md, &d) > 0) {
    if (e > 0)
      dec_copy(&md, &d);
    else
      dec_set_u64(&md, 1, 0, 0);
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
    if (dec_is_zero(&d))
      break; // BigNumber.js divides by zero here, and the Infinity ends the loop
    dec_div(&q, &n, &d, 0, ROUND_DOWN);
    dec_add_product(&d2, &d0, &q, &d1);
    if (dec_cmp(&d2, &md) > 0)
      break;
    dec_swap(&d0, &d1); // d0 = d1
    dec_swap(&d1, &d2); // d1 = d2
    dec_add_product(&t, &n0, &q, &n1);
    dec_swap(&n0, &n1); // n0 = n1
    dec_swap(&n1, &t);  // n1 = n0 + q * n1
    dec_mul(&t, &q, &d);
    dec_add(&d2, &n, &t, 1);
    dec_swap(&n, &d);  // n = d
    dec_swap(&d, &d2); // d = n - q * d
  }
  // d2 = floor((md - d0) / d1)
  dec_add(&t, &md, &d0, 1);
  dec_div(&d2, &t, &d1, 0, ROUND_DOWN);
  dec_add_product(&t, &n0, &d2, &n1);
  dec_swap(&n0, &t);
  dec_add_product(&t, &d0, &d2, &d1);
  dec_swap(&d0, &t);
  // Which of n1 / d1 and n0 / d0 is closer to xn / xd: compare
  // |n1 * xd - xn * d1| * d0 with |n0 * xd - xn * d0| * d1.
  Dec u, v, w;
  dec_init(&u);
  dec_init(&v);
  dec_init(&w);
  dec_mul(&u, &n1, &xd);
  dec_mul(&v, &xn, &d1);
  dec_add(&w, &u, &v, 1);
  w.neg = 0;
  dec_mul(&u, &w, &d0); // left side
  dec_mul(&v, &n0, &xd);
  dec_mul(&w, &xn, &d0);
  dec_add(&t, &v, &w, 1);
  t.neg = 0;
  dec_mul(&v, &t, &d1); // right side
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
  for (size_t i = 0; i < sizeof all / sizeof *all; ++i)
    dec_free(all[i]);
  return result;
}

//===----------------------------------------------------------------------===//
// Conversions
//===----------------------------------------------------------------------===//

// Reads "digits[.digits][e[+-]digits]" (a known-valid decimal string without
// its sign), as BigNumber.js's parseValidString.
static void parse_valid(Dec *x, const char *s, size_t n, int neg) {
  size_t mantissa = 0;
  while (mantissa < n && s[mantissa] != 'e' && s[mantissa] != 'E')
    ++mantissa;
  int64_t exponent = 0;
  if (mantissa < n) {
    size_t i = mantissa + 1;
    int eneg = 0;
    if (i < n && (s[i] == '+' || s[i] == '-'))
      eneg = s[i++] == '-';
    for (; i < n; ++i)
      if (exponent < INT64_C(1) << 40) // far beyond the range either way
        exponent = exponent * 10 + (s[i] - '0');
    if (eneg)
      exponent = -exponent;
  }
  // The digits without the point, from the most significant.
  nat_reserve(&x->c, mantissa / 9 + 2);
  x->c.n = 0;
  int64_t point = -1;
  size_t count = 0;
  for (size_t i = 0; i < mantissa; ++i)
    if (s[i] == '.')
      point = (int64_t)count;
    else
      ++count;
  if (point < 0)
    point = (int64_t)count;
  // Fill the limbs from the least significant digit.
  uint32_t limb = 0, scale = 1;
  for (size_t i = mantissa; i-- > 0;) {
    if (s[i] == '.')
      continue;
    limb += (uint32_t)(s[i] - '0') * scale;
    scale *= 10;
    if (scale == BASE) {
      nat_reserve(&x->c, (uint64_t)x->c.n + 1);
      x->c.d[x->c.n++] = limb;
      limb = 0;
      scale = 1;
    }
  }
  if (scale > 1) {
    nat_reserve(&x->c, (uint64_t)x->c.n + 1);
    x->c.d[x->c.n++] = limb;
  }
  nat_trim(&x->c);
  x->e = exponent - ((int64_t)count - point);
  x->neg = x->c.n ? neg : 0;
  dec_normalize(x);
}

static int ascii_digit(uint16_t c) { return c >= '0' && c <= '9'; }

// BigNumber.js's isNumeric: /^-?(?:\d+(?:\.\d*)?|\.\d+)(?:e[+-]?\d+)?$/i
static int is_numeric(const uint16_t *s, size_t n) {
  size_t i = 0;
  if (i < n && s[i] == '-')
    ++i;
  size_t whole = 0, fraction = 0;
  while (i < n && ascii_digit(s[i])) {
    ++i;
    ++whole;
  }
  if (i < n && s[i] == '.') {
    ++i;
    while (i < n && ascii_digit(s[i])) {
      ++i;
      ++fraction;
    }
    if (whole == 0 && fraction == 0)
      return 0;
  } else if (whole == 0) {
    return 0;
  }
  if (i < n && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    if (i < n && (s[i] == '+' || s[i] == '-'))
      ++i;
    size_t start = i;
    while (i < n && ascii_digit(s[i]))
      ++i;
    if (i == start)
      return 0;
  }
  return i == n;
}

// `s`, known to be ASCII, as a char string in `t`.
static void narrow(Text *t, const uint16_t *s, size_t n) {
  text_reserve(t, n);
  for (size_t i = 0; i < n; ++i)
    t->p[t->n++] = (char)s[i];
}

TZ_COLD static void cannot_convert(const uint16_t *s, size_t n) {
  // The text in the message as UTF-8, cut short if it is long.
  char text[200];
  size_t len = 0;
  for (size_t i = 0; i < n && len + 4 < 160; ++i) {
    uint32_t c = s[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF)
      c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
    else if (c >= 0xD800 && c <= 0xDFFF)
      c = 0xFFFD;
    if (c < 0x80) {
      text[len++] = (char)c;
    } else if (c < 0x800) {
      text[len++] = (char)(0xC0 | (c >> 6));
      text[len++] = (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      text[len++] = (char)(0xE0 | (c >> 12));
      text[len++] = (char)(0x80 | ((c >> 6) & 0x3F));
      text[len++] = (char)(0x80 | (c & 0x3F));
    } else {
      text[len++] = (char)(0xF0 | (c >> 18));
      text[len++] = (char)(0x80 | ((c >> 12) & 0x3F));
      text[len++] = (char)(0x80 | ((c >> 6) & 0x3F));
      text[len++] = (char)(0x80 | (c & 0x3F));
    }
  }
  text[len] = 0;
  tz_throw(TZ_SYNTAX_ERROR, "Cannot convert %s%s to a decimal", text, len + 4 >= 160 ? "..." : "");
}

static int digit_in(uint16_t c, int base) {
  int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : 99;
  return v < base ? v : -1;
}

// "ff", "1010.1", "7_7" after 0x, 0b or 0o, as BigNumber.js's
// parseBaseString: underscores only between digits, upper case letters only
// in a string without lower case ones. A fraction is converted exactly and
// rounded half up to 20 decimal places.
static int parse_base(Dec *x, const uint16_t *s, size_t n, int base, int neg) {
  int upper = 0;
  for (size_t i = 0; i < n; ++i)
    if (s[i] >= 'A' && s[i] <= 'Z')
      upper = 1;
  if (upper)
    for (size_t i = 0; i < n; ++i)
      if (s[i] >= 'a' && s[i] <= 'z')
        return 0;
  Nat value;
  nat_init(&value);
  uint64_t fraction_digits = 0;
  int dot = 0, previous_digit = 0, any = 0;
  for (size_t i = 0; i < n; ++i) {
    uint16_t c = s[i];
    if (c >= 'A' && c <= 'Z')
      c = (uint16_t)(c - 'A' + 'a');
    int v = digit_in(c, base);
    if (v >= 0) {
      nat_mul_add(&value, (uint32_t)base, (uint32_t)v);
      if (dot)
        ++fraction_digits;
      previous_digit = any = 1;
      continue;
    }
    if (c == '_' && previous_digit && i + 1 < n) {
      previous_digit = 0;
      continue;
    }
    if (c == '.' && (i == 0 || (!dot && previous_digit))) {
      if (i + 1 == n)
        break; // a trailing point is dropped
      dot = 1;
      previous_digit = 0;
      continue;
    }
    nat_free(&value);
    return 0;
  }
  if (!any && !dot) {
    nat_free(&value);
    return 0;
  }
  if (!fraction_digits) {
    nat_copy(&x->c, &value);
    x->e = 0;
    x->neg = x->c.n ? neg : 0;
  } else {
    Dec num, den;
    dec_init(&num);
    dec_init(&den);
    nat_copy(&num.c, &value);
    num.neg = neg;
    nat_set_u64(&den.c, 1);
    for (uint64_t i = 0; i < fraction_digits; ++i)
      nat_mul_add(&den.c, (uint32_t)base, 0);
    dec_div(x, &num, &den, DECIMAL_PLACES, ROUND_HALF_UP);
    dec_free(&num);
    dec_free(&den);
  }
  nat_free(&value);
  dec_normalize(x);
  return 1;
}

static int equals_ascii(const uint16_t *s, size_t n, const char *word) {
  size_t k = strlen(word);
  if (n != k)
    return 0;
  for (size_t i = 0; i < k; ++i)
    if (s[i] != (uint16_t)word[i])
      return 0;
  return 1;
}

// decimal(s), as `new BigNumber(s)`: an invalid string is a SyntaxError
// (BigNumber.js throws too), and so are "NaN" and "Infinity", which no
// decimal can hold.
void tz_dec_from_string(TzDec *out, void *obj, uint64_t meta) {
  TzStr str = {obj, meta};
  TzBuf buf;
  tz_buf_init(&buf);
  tz_buf_str(&buf, str);
  const uint16_t *s = buf.data;
  size_t n = buf.length;
  Dec x;
  dec_init(&x);
  Text ascii;
  text_init(&ascii);
  if (is_numeric(s, n)) {
    narrow(&ascii, s, n);
    int neg = ascii.p[0] == '-';
    parse_valid(&x, ascii.p + neg, ascii.n - (size_t)neg, neg);
  } else {
    // Without the whitespace around it and a leading plus.
    size_t i = 0, j = n;
    while (i < n && tz_is_js_space(s[i]))
      ++i;
    if (i < n && s[i] == '+' && !(i + 1 < n && s[i + 1] == '-'))
      ++i;
    while (j > i && tz_is_js_space(s[j - 1]))
      --j;
    const uint16_t *t = s + i;
    size_t m = j - i;
    int neg = m > 0 && t[0] == '-';
    const uint16_t *body = t + neg;
    size_t bn = m - (size_t)neg;
    int ok = 0;
    if (equals_ascii(body, bn, "Infinity") || equals_ascii(body, bn, "NaN")) {
      ok = 0;
    } else if (bn > 2 && body[0] == '0' && body[2] != '.' &&
               ((body[1] | 0x20) == 'x' || (body[1] | 0x20) == 'b' || (body[1] | 0x20) == 'o')) {
      int base = (body[1] | 0x20) == 'x' ? 16 : (body[1] | 0x20) == 'b' ? 2 : 8;
      ok = parse_base(&x, body + 2, bn - 2, base, neg);
    } else {
      // Underscores between digits are dropped.
      uint16_t *clean = (uint16_t *)tz_alloc((m ? m : 1) * sizeof(uint16_t));
      size_t k = 0;
      for (size_t p = 0; p < m; ++p) {
        if (t[p] == '_' && p > 0 && ascii_digit(t[p - 1]) && p + 1 < m && ascii_digit(t[p + 1]))
          continue;
        clean[k++] = t[p];
      }
      if (is_numeric(clean, k)) {
        narrow(&ascii, clean, k);
        parse_valid(&x, ascii.p + neg, ascii.n - (size_t)neg, neg);
        ok = 1;
      }
      tz_free(clean);
    }
    if (!ok)
      cannot_convert(s, n);
  }
  tz_free(ascii.p);
  tz_buf_free(&buf);
  dec_store_free(out, &x);
}

// decimal(x) of a double: the shortest digits that read back as x, as
// String(x) gives them (and BigNumber.js takes them).
void tz_dec_from_f64(TzDec *out, double v) {
  if (isnan(v) || isinf(v)) {
    char text[32];
    text[tz_format_f64(v, text)] = 0;
    tz_throw(TZ_RANGE_ERROR,
             "The number %s cannot be converted to a decimal because it is not finite", text);
    out->coef = 0;
    out->meta = 0;
    return;
  }
  if (v == trunc(v) && fabs(v) < 9007199254740992.0) {
    out->coef = (int64_t)v; // -0 is 0
    out->meta = 0;
    return;
  }
  char text[32];
  size_t n = tz_format_f64(v, text);
  int neg = text[0] == '-';
  Dec x;
  dec_init(&x);
  parse_valid(&x, text + neg, n - (size_t)neg, neg);
  dec_store_free(out, &x);
}

// Number(x): the double nearest to x, as Number(x.toString()) gives it.
double tz_dec_to_f64(int64_t a, uint64_t am) {
  if (is_short(am) && scale_of(am) <= 22 && magnitude(a) <= (UINT64_C(1) << 53)) {
    // Both exact, so the quotient is correctly rounded.
    static const double powers[23] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                      1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                      1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    return (double)a / powers[scale_of(am)];
  }
  Dec x;
  dec_init(&x);
  dec_load(&x, a, am);
  Text t;
  text_init(&t);
  if (x.neg)
    text_char(&t, '-');
  put_coefficient(&t, &x);
  char exp[32];
  int k = snprintf(exp, sizeof exp, "e%lld", (long long)x.e);
  text_put(&t, exp, (size_t)k);
  text_char(&t, 0);
  double v = strtod(t.p, NULL);
  tz_free(t.p);
  dec_free(&x);
  return v;
}

// A rounding mode given as a string at run time; literals CodeGen maps itself.
int32_t tz_dec_rounding_mode(void *obj, uint64_t meta) {
  static const char *const names[] = {"up",        "down",      "ceil",      "floor",     "half-up",
                                      "half-down", "half-even", "half-ceil", "half-floor"};
  TzStr str = {obj, meta};
  TzBuf buf;
  tz_buf_init(&buf);
  tz_buf_str(&buf, str);
  for (int32_t m = 0; m < 9; ++m) {
    size_t k = strlen(names[m]);
    if (buf.length != k)
      continue;
    size_t i = 0;
    while (i < k && buf.data[i] == (uint16_t)names[m][i])
      ++i;
    if (i == k) {
      tz_buf_free(&buf);
      return m;
    }
  }
  Text t;
  text_init(&t);
  for (size_t i = 0; i < buf.length && i < 40; ++i)
    text_char(&t, buf.data[i] < 0x80 && buf.data[i] >= 0x20 ? (char)buf.data[i] : '?');
  text_char(&t, 0);
  tz_throw(TZ_RANGE_ERROR,
           "unknown rounding mode '%s'; the modes are 'up', 'down', 'ceil', 'floor', 'half-up', "
           "'half-down', 'half-even', 'half-ceil' and 'half-floor'",
           t.p);
  tz_free(t.p);
  tz_buf_free(&buf);
  return ROUND_HALF_UP;
}
