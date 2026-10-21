// Numbers in text: Number(s), parseInt and parseFloat, which read the ASCII
// text string.c hands them, and x.toFixed(digits).

#include "tzrt.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

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

// strtod of text[0..n), which holds a valid decimal number, correctly
// rounded as JavaScript requires.
static double decimal_value(const char *text, size_t n) {
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
  if (digits < 0 || digits > 100)
    tz_panicf("RangeError: toFixed() digits argument must be between 0 and 100");
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
