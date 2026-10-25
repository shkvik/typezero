// Numbers in text: Number(s), parseInt and parseFloat, which read the ASCII
// text string.c hands them, x.toFixed(digits), and the shortest digits of a
// double, which String(x) writes.

#include "tzrt.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "number_tables.h"

static int is_digit(char c) { return c >= '0' && c <= '9'; }

// The value of `c` as a digit of any radix up to 36, or 36 for a non-digit.
static int digit_value(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  c |= 0x20; // lower case
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 10;
  return 36;
}

// The length of the decimal number at text[i] (StrUnsignedDecimalLiteral
// without Infinity): digits, a point and digits, an exponent. 0 if none.
static size_t decimal_length(const char *text, size_t i, size_t n) {
  size_t j = i, digits = 0;
  while (j < n && is_digit(text[j])) {
    ++j;
    ++digits;
  }
  if (j < n && text[j] == '.') {
    ++j;
    while (j < n && is_digit(text[j])) {
      ++j;
      ++digits;
    }
  }
  if (digits == 0)
    return 0;
  if (j < n && (text[j] == 'e' || text[j] == 'E')) {
    size_t k = j + 1;
    if (k < n && (text[k] == '+' || text[k] == '-'))
      ++k;
    if (k < n && is_digit(text[k])) {
      while (k < n && is_digit(text[k]))
        ++k;
      j = k;
    }
  }
  return j - i;
}

// The powers of ten that a double holds exactly.
static const double exact_pow10[23] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                       1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                       1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

// decimal_value without strtod, for the usual numbers: when the significant
// digits make an integer below 2^53 and the power of ten is exact, one
// multiplication or division of exact doubles rounds correctly (Clinger's
// fast path). Returns 0 when it does not apply.
static int decimal_value_fast(const char *text, size_t n, double *out) {
  size_t i = 0;
  int negative = 0;
  if (i < n && (text[i] == '+' || text[i] == '-'))
    negative = text[i++] == '-';
  uint64_t m = 0;
  int digits = 0;   // significant ones, in m
  int32_t e10 = 0; // the value is m * 10^e10
  int point = 0;
  for (; i < n; ++i) {
    char c = text[i];
    if (c == '.') {
      point = 1;
      continue;
    }
    if (!is_digit(c))
      break;
    if (m == 0 && c == '0') { // a leading zero
      e10 -= point;
      continue;
    }
    if (++digits > 19)
      return 0;
    m = m * 10 + (uint64_t)(c - '0');
    e10 -= point;
  }
  if (i < n) { // the exponent, which decimal_length made sure has digits
    ++i;
    int exponent_negative = 0;
    if (text[i] == '+' || text[i] == '-')
      exponent_negative = text[i++] == '-';
    int32_t exponent = 0;
    for (; i < n; ++i) {
      if (exponent > 100000)
        return 0;
      exponent = exponent * 10 + (text[i] - '0');
    }
    e10 += exponent_negative ? -exponent : exponent;
  }
  double value;
  if (m == 0)
    value = 0;
  else if (m > (1ull << 53) || e10 < -22 || e10 > 22)
    return 0;
  else
    value = e10 >= 0 ? (double)m * exact_pow10[e10] : (double)m / exact_pow10[-e10];
  *out = negative ? -value : value;
  return 1;
}

// strtod of text[0..n), which holds a valid decimal number, correctly
// rounded as JavaScript requires.
static double decimal_value(const char *text, size_t n) {
  double fast;
  if (decimal_value_fast(text, n, &fast))
    return fast;
  char local[64];
  char *copy = n < sizeof local ? local : (char *)tz_alloc(n + 1);
  memcpy(copy, text, n);
  copy[n] = 0;
  double value = strtod(copy, NULL);
  if (copy != local)
    tz_free(copy);
  return value;
}

// Number(s) once the whitespace around s is gone: a decimal number,
// Infinity, or 0x, 0o and 0b followed by digits; anything else is NaN.
double tz_number_from_ascii(const char *text, size_t n, int complete) {
  if (!complete)
    return NAN;
  if (n == 0)
    return 0;
  if (n > 2 && text[0] == '0') {
    char kind = text[1] | 0x20;
    int radix = kind == 'x' ? 16 : kind == 'o' ? 8 : kind == 'b' ? 2 : 0;
    if (radix) {
      double value = 0;
      for (size_t i = 2; i < n; ++i) {
        int digit = digit_value(text[i]);
        if (digit >= radix)
          return NAN;
        value = value * radix + digit;
      }
      return value;
    }
  }
  size_t i = text[0] == '+' || text[0] == '-' ? 1 : 0;
  if (n - i == 8 && memcmp(text + i, "Infinity", 8) == 0)
    return text[0] == '-' ? -INFINITY : INFINITY;
  size_t length = decimal_length(text, i, n);
  if (length == 0 || length != n - i) // a sign alone is not a number either
    return NAN;
  return decimal_value(text, n);
}

// parseInt: a sign, then the longest run of digits of the radix (10 unless
// given, or 16 after "0x"); NaN without digits.
double tz_parse_int_ascii(const char *text, size_t n, int32_t radix) {
  size_t i = 0;
  int negative = 0;
  if (i < n && (text[i] == '+' || text[i] == '-'))
    negative = text[i++] == '-';
  int strip_prefix = 1;
  if (radix != 0) {
    if (radix < 2 || radix > 36)
      return NAN;
    strip_prefix = radix == 16;
  } else {
    radix = 10;
  }
  if (strip_prefix && n - i >= 2 && text[i] == '0' && (text[i + 1] | 0x20) == 'x') {
    i += 2;
    radix = 16;
  }
  size_t start = i;
  while (i < n && digit_value(text[i]) < radix)
    ++i;
  if (i == start)
    return NAN;
  double value;
  if (radix == 10) {
    value = decimal_value(text + start, i - start); // correctly rounded, also when long
  } else {
    value = 0;
    for (size_t k = start; k < i; ++k)
      value = value * radix + digit_value(text[k]);
  }
  return negative ? -value : value; // "-0" gives -0, as in JavaScript
}

// parseFloat: the longest decimal number (or Infinity) at the start.
double tz_parse_float_ascii(const char *text, size_t n) {
  size_t i = n > 0 && (text[0] == '+' || text[0] == '-') ? 1 : 0;
  if (n - i >= 8 && memcmp(text + i, "Infinity", 8) == 0)
    return text[0] == '-' ? -INFINITY : INFINITY;
  size_t length = decimal_length(text, i, n);
  if (length == 0)
    return NAN;
  return decimal_value(text, i + length);
}

//===----------------------------------------------------------------------===//
// toFixed
//===----------------------------------------------------------------------===//

// Unsigned integers of up to 512 bits, in 32-bit limbs, least significant
// first. toFixed needs at most m * 10^100 * 2^17 < 2^403.
#define BIG_LIMBS 16

typedef struct {
  uint32_t limb[BIG_LIMBS];
  int n; // limbs in use; the top one is not 0
} Big;

static void big_set(Big *b, uint64_t value) {
  b->n = 0;
  for (; value; value >>= 32)
    b->limb[b->n++] = (uint32_t)value;
}

static void big_mul_small(Big *b, uint32_t factor) {
  uint64_t carry = 0;
  for (int i = 0; i < b->n; ++i) {
    uint64_t t = (uint64_t)b->limb[i] * factor + carry;
    b->limb[i] = (uint32_t)t;
    carry = t >> 32;
  }
  if (carry)
    b->limb[b->n++] = (uint32_t)carry;
}

// Divides by `divisor` in place and returns the remainder.
static uint32_t big_div_small(Big *b, uint32_t divisor) {
  uint64_t rest = 0;
  for (int i = b->n - 1; i >= 0; --i) {
    uint64_t t = (rest << 32) | b->limb[i];
    b->limb[i] = (uint32_t)(t / divisor);
    rest = t % divisor;
  }
  while (b->n > 0 && b->limb[b->n - 1] == 0)
    --b->n;
  return (uint32_t)rest;
}

static int big_bit_length(const Big *b) {
  if (b->n == 0)
    return 0;
  int bits = 32 * (b->n - 1);
  for (uint32_t top = b->limb[b->n - 1]; top; top >>= 1)
    ++bits;
  return bits;
}

static void big_shift_left(Big *b, int k) {
  int words = k / 32, bits = k % 32;
  if (b->n == 0)
    return;
  uint32_t carry = 0;
  int n = b->n;
  uint32_t shifted[BIG_LIMBS + 1] = {0};
  for (int i = 0; i < n; ++i) {
    shifted[i + words] = (b->limb[i] << bits) | carry;
    carry = bits ? b->limb[i] >> (32 - bits) : 0;
  }
  shifted[n + words] = carry;
  b->n = n + words + 1;
  memcpy(b->limb, shifted, sizeof b->limb);
  while (b->n > 0 && b->limb[b->n - 1] == 0)
    --b->n;
}

static void big_shift_right(Big *b, int k) {
  int words = k / 32, bits = k % 32;
  if (words >= b->n) {
    b->n = 0;
    return;
  }
  for (int i = 0; i + words < b->n; ++i) {
    uint64_t pair = b->limb[i + words];
    if (i + words + 1 < b->n)
      pair |= (uint64_t)b->limb[i + words + 1] << 32;
    b->limb[i] = (uint32_t)(pair >> bits);
  }
  b->n -= words;
  while (b->n > 0 && b->limb[b->n - 1] == 0)
    --b->n;
}

// b += 2^k
static void big_add_power_of_two(Big *b, int k) {
  int word = k / 32;
  while (b->n <= word)
    b->limb[b->n++] = 0;
  uint64_t carry = (uint64_t)1 << (k % 32);
  for (int i = word; carry && i < BIG_LIMBS; ++i) {
    if (i == b->n)
      b->limb[b->n++] = 0;
    uint64_t t = (uint64_t)b->limb[i] + carry;
    b->limb[i] = (uint32_t)t;
    carry = t >> 32;
  }
}

// x.toFixed(digits) as JavaScript defines it: the integer n for which
// n / 10^digits - x is closest to 0 (the larger one on a tie), written out
// with `digits` decimals. Exact, because x = m * 2^e is scaled by 10^digits
// with big integers.
void tz_num_to_fixed(TzStr *out, double x, int64_t digits) {
  if (digits < 0 || digits > 100) {
    tz_throw(TZ_RANGE_ERROR, "toFixed() digits argument must be between 0 and 100");
    *out = tz_str_from_ascii("", 0);
    return;
  }
  if (isnan(x)) {
    *out = tz_str_from_ascii("NaN", 3);
    return;
  }
  if (fabs(x) >= 1e21) {
    tz_str_from_f64(out, x);
    return;
  }
  int negative = x < 0; // not -0
  if (negative)
    x = -x;
  Big n;
  int e = 0;
  big_set(&n, 0);
  if (x != 0) {
    double fraction = frexp(x, &e); // x = fraction * 2^e, 0.5 <= fraction < 1
    big_set(&n, (uint64_t)ldexp(fraction, 53));
    e -= 53;
  }
  for (int64_t i = 0; i < digits; ++i)
    big_mul_small(&n, 10);
  if (e >= 0) {
    big_shift_left(&n, e);
  } else if (big_bit_length(&n) < -e) { // below one half
    big_set(&n, 0);
  } else { // round half up: (n + 2^(k-1)) / 2^k
    big_add_power_of_two(&n, -e - 1);
    big_shift_right(&n, -e);
  }

  // The decimal digits of n, at least digits + 1 of them.
  char reversed[160];
  int count = 0;
  while (n.n > 0)
    reversed[count++] = (char)('0' + big_div_small(&n, 10));
  while (count < digits + 1)
    reversed[count++] = '0';
  char text[170];
  size_t len = 0;
  if (negative)
    text[len++] = '-';
  for (int i = count - 1; i >= 0; --i) {
    text[len++] = reversed[i];
    if (i == digits && digits > 0)
      text[len++] = '.';
  }
  *out = tz_str_from_ascii(text, len);
}

//===----------------------------------------------------------------------===//
// The shortest digits of a double
//===----------------------------------------------------------------------===//

// Ryū (Ulf Adams, "Ryū: fast float-to-string conversion", PLDI 2018) finds
// the shortest decimal that reads back as the same double, and of those the
// closest to it, a tie going to the even one: the digits Number#toString
// writes in JavaScript. It scales the bounds of the interval that rounds to
// the double by a power of ten with 128-bit arithmetic, then removes digits
// while the bounds stay apart.

// ceil(log2(5^e)) for 0 < e <= 3528, and 1 for e = 0: the bits of 5^e.
static inline int32_t pow5_bits(int32_t e) { return (int32_t)(((uint32_t)e * 1217359) >> 19) + 1; }

// floor(log10(2^e)) for 0 <= e <= 1650, and floor(log10(5^e)) for 0 <= e <=
// 2620.
static inline uint32_t log10_pow2(int32_t e) { return ((uint32_t)e * 78913) >> 18; }
static inline uint32_t log10_pow5(int32_t e) { return ((uint32_t)e * 732923) >> 20; }

// Whether 5^p divides `value` (not 0), and 2^p (p < 64).
static int pow5_divides(uint64_t value, uint32_t p) {
  uint32_t count = 0;
  while (value % 5 == 0) {
    value /= 5;
    ++count;
  }
  return count >= p;
}

static inline int pow2_divides(uint64_t value, uint32_t p) {
  return (value & ((1ull << p) - 1)) == 0;
}

// (m * mul) >> j of a 128-bit `mul`, low word first; 64 < j < 128.
static inline uint64_t mul_shift(uint64_t m, const uint64_t *mul, int32_t j) {
#ifdef __SIZEOF_INT128__
  unsigned __int128 low = (unsigned __int128)m * mul[0];
  unsigned __int128 high = (unsigned __int128)m * mul[1];
  return (uint64_t)(((low >> 64) + high) >> (j - 64));
#else
  // The four products of the 32-bit halves.
  uint64_t a = m >> 32, b = (uint32_t)m;
  uint64_t parts[2][2];
  for (int w = 0; w < 2; ++w) {
    uint64_t c = mul[w] >> 32, d = (uint32_t)mul[w];
    uint64_t bd = b * d, ad = a * d, bc = b * c, ac = a * c;
    uint64_t middle = (bd >> 32) + (uint32_t)ad + (uint32_t)bc;
    parts[w][0] = (middle << 32) | (uint32_t)bd;
    parts[w][1] = ac + (ad >> 32) + (bc >> 32) + (middle >> 32);
  }
  uint64_t sum = parts[0][1] + parts[1][0];
  uint64_t high = parts[1][1] + (sum < parts[0][1]);
  int32_t shift = j - 64;
  return (high << (64 - shift)) | (sum >> shift);
#endif
}

// The shortest digits of a finite positive double, written to `digits` (17
// at most, no terminator); returns how many, and sets `*exponent` so that the
// double is those digits times 10^exponent.
int tz_f64_digits(double value, char *digits, int32_t *exponent) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof bits);
  uint64_t mantissa = bits & ((1ull << 52) - 1);
  uint32_t biased = (uint32_t)(bits >> 52) & 0x7FF;

  uint64_t output;
  int32_t e10;
  int32_t e2 = (int32_t)biased - 1075;
  if (biased != 0 && e2 <= 0 && e2 >= -52 &&
      (mantissa & ((1ull << -e2) - 1)) == 0) {
    // An integer below 2^53: its own digits, without the zeros at the end.
    output = ((1ull << 52) | mantissa) >> -e2;
    e10 = 0;
    while (output % 10 == 0) {
      output /= 10;
      ++e10;
    }
  } else {
    // m2 * 2^e2 with two more bits for the bounds, which are mv - mm_shift - 1
    // and mv + 2 (in quarters): the interval of the reals that round to the
    // double, closed when its mantissa is even.
    uint64_t m2;
    if (biased == 0) {
      e2 = 1 - 1023 - 52 - 2;
      m2 = mantissa;
    } else {
      e2 = (int32_t)biased - 1023 - 52 - 2;
      m2 = (1ull << 52) | mantissa;
    }
    int even = (m2 & 1) == 0;
    uint64_t mv = 4 * m2;
    uint32_t mm_shift = mantissa != 0 || biased <= 1;

    // The bounds and the value scaled by 10^-e10, and whether the digits
    // dropped from vm and vr in that are all zeros.
    uint64_t vr, vp, vm;
    int vm_zeros = 0, vr_zeros = 0;
    if (e2 >= 0) {
      uint32_t q = log10_pow2(e2) - (e2 > 3);
      e10 = (int32_t)q;
      int32_t k = POW5_BITS + pow5_bits((int32_t)q) - 1;
      int32_t i = -e2 + (int32_t)q + k;
      vr = mul_shift(4 * m2, pow5_inv[q], i);
      vp = mul_shift(4 * m2 + 2, pow5_inv[q], i);
      vm = mul_shift(4 * m2 - 1 - mm_shift, pow5_inv[q], i);
      if (q <= 21) {
        // Only one of mp, mv and mm can be a multiple of 5.
        if (mv % 5 == 0)
          vr_zeros = pow5_divides(mv, q);
        else if (even)
          vm_zeros = pow5_divides(mv - 1 - mm_shift, q);
        else
          vp -= pow5_divides(mv + 2, q);
      }
    } else {
      uint32_t q = log10_pow5(-e2) - (-e2 > 1);
      e10 = (int32_t)q + e2;
      int32_t i = -e2 - (int32_t)q;
      int32_t k = pow5_bits(i) - POW5_BITS;
      int32_t j = (int32_t)q - k;
      vr = mul_shift(4 * m2, pow5[i], j);
      vp = mul_shift(4 * m2 + 2, pow5[i], j);
      vm = mul_shift(4 * m2 - 1 - mm_shift, pow5[i], j);
      if (q <= 1) {
        // mv has at least two trailing zero bits, mp one, mm one only when
        // mm_shift is 1.
        vr_zeros = 1;
        if (even)
          vm_zeros = mm_shift == 1;
        else
          --vp;
      } else if (q < 63) {
        vr_zeros = pow2_divides(mv, q);
      }
    }

    // Remove digits while the bounds stay apart; the last one removed from
    // vr says how to round.
    int32_t removed = 0;
    uint32_t last = 0;
    if (vm_zeros || vr_zeros) {
      // The general case, which needs to know about exact ties (rare).
      while (vp / 10 > vm / 10) {
        vm_zeros &= vm % 10 == 0;
        vr_zeros &= last == 0;
        last = (uint32_t)(vr % 10);
        vr /= 10;
        vp /= 10;
        vm /= 10;
        ++removed;
      }
      if (vm_zeros) {
        while (vm % 10 == 0) {
          vr_zeros &= last == 0;
          last = (uint32_t)(vr % 10);
          vr /= 10;
          vp /= 10;
          vm /= 10;
          ++removed;
        }
      }
      if (vr_zeros && last == 5 && vr % 2 == 0)
        last = 4; // exactly halfway: round to even
      output = vr + ((vr == vm && (!even || !vm_zeros)) || last >= 5);
    } else {
      int round_up = 0;
      if (vp / 100 > vm / 100) { // two digits at once, the usual start
        round_up = vr % 100 >= 50;
        vr /= 100;
        vp /= 100;
        vm /= 100;
        removed += 2;
      }
      while (vp / 10 > vm / 10) {
        round_up = vr % 10 >= 5;
        vr /= 10;
        vp /= 10;
        vm /= 10;
        ++removed;
      }
      output = vr + (vr == vm || round_up);
    }
    e10 += removed;
  }

  char reversed[20];
  int count = 0;
  do {
    reversed[count++] = (char)('0' + output % 10);
    output /= 10;
  } while (output != 0);
  for (int i = 0; i < count; ++i)
    digits[i] = reversed[count - 1 - i];
  *exponent = e10;
  return count;
}
