#include "runtime/core/number/number.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/output/output.h"
#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "runtime/core/number/number_tables.h"

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int digit_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  c |= 0x20;
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 10;
  }

  return 36;
}

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
  if (digits == 0) {
    return 0;
  }
  if (j < n && (text[j] == 'e' || text[j] == 'E')) {
    size_t k = j + 1;
    if (k < n && (text[k] == '+' || text[k] == '-')) {
      ++k;
    }
    if (k < n && is_digit(text[k])) {
      while (k < n && is_digit(text[k])) {
        ++k;
      }
      j = k;
    }
  }

  return j - i;
}

static const double exact_pow10[23] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                       1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                       1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

static int decimal_value_fast(const char *text, size_t n, double *out) {
  size_t i = 0;
  int negative = 0;
  if (i < n && (text[i] == '+' || text[i] == '-')) {
    negative = text[i++] == '-';
  }
  uint64_t m = 0;
  int digits = 0;
  int32_t e10 = 0;
  int point = 0;
  for (; i < n; ++i) {
    char c = text[i];
    if (c == '.') {
      point = 1;
      continue;
    }
    if (!is_digit(c)) {
      break;
    }
    if (m == 0 && c == '0') {
      e10 -= point;
      continue;
    }
    if (++digits > 19) {
      return 0;
    }
    m = m * 10 + (uint64_t)(c - '0');
    e10 -= point;
  }
  if (i < n) {
    ++i;
    int exponent_negative = 0;
    if (text[i] == '+' || text[i] == '-') {
      exponent_negative = text[i++] == '-';
    }
    int32_t exponent = 0;
    for (; i < n; ++i) {
      if (exponent > 100000) {
        return 0;
      }
      exponent = exponent * 10 + (text[i] - '0');
    }
    e10 += exponent_negative ? -exponent : exponent;
  }
  double value;
  if (m == 0) {
    value = 0;
  } else if (m > (1ull << 53) || e10 < -22 || e10 > 22) {
    return 0;
  } else {
    value = e10 >= 0 ? (double)m * exact_pow10[e10] : (double)m / exact_pow10[-e10];
  }
  *out = negative ? -value : value;

  return 1;
}

static double decimal_value(const char *text, size_t n) {
  double fast;
  if (decimal_value_fast(text, n, &fast)) {
    return fast;
  }
  char local[64];
  char *copy = n < sizeof local ? local : (char *)tz_alloc(n + 1);
  memcpy(copy, text, n);
  copy[n] = 0;
  double value = strtod(copy, NULL);
  if (copy != local) {
    tz_free(copy);
  }

  return value;
}

double tz_number_from_ascii(const char *text, size_t n, int complete) {
  if (!complete) {
    return NAN;
  }
  if (n == 0) {
    return 0;
  }
  if (n > 2 && text[0] == '0') {
    char kind = text[1] | 0x20;
    int radix = kind == 'x' ? 16 : kind == 'o' ? 8 : kind == 'b' ? 2 : 0;
    if (radix) {
      double value = 0;
      for (size_t i = 2; i < n; ++i) {
        int digit = digit_value(text[i]);
        if (digit >= radix) {
          return NAN;
        }
        value = value * radix + digit;
      }
      return value;
    }
  }
  size_t i = text[0] == '+' || text[0] == '-' ? 1 : 0;
  if (n - i == 8 && memcmp(text + i, "Infinity", 8) == 0) {
    return text[0] == '-' ? -INFINITY : INFINITY;
  }
  size_t length = decimal_length(text, i, n);
  if (length == 0 || length != n - i) {
    return NAN;
  }

  return decimal_value(text, n);
}

double tz_parse_int_ascii(const char *text, size_t n, int32_t radix) {
  size_t i = 0;
  int negative = 0;
  if (i < n && (text[i] == '+' || text[i] == '-')) {
    negative = text[i++] == '-';
  }
  int strip_prefix = 1;
  if (radix != 0) {
    if (radix < 2 || radix > 36) {
      return NAN;
    }
    strip_prefix = radix == 16;
  } else {
    radix = 10;
  }
  if (strip_prefix && n - i >= 2 && text[i] == '0' && (text[i + 1] | 0x20) == 'x') {
    i += 2;
    radix = 16;
  }
  size_t start = i;
  while (i < n && digit_value(text[i]) < radix) {
    ++i;
  }
  if (i == start) {
    return NAN;
  }
  double value;
  if (radix == 10) {
    value = decimal_value(text + start, i - start);
  } else {
    value = 0;
    for (size_t k = start; k < i; ++k) {
      value = value * radix + digit_value(text[k]);
    }
  }

  return negative ? -value : value;
}

double tz_parse_float_ascii(const char *text, size_t n) {
  size_t i = n > 0 && (text[0] == '+' || text[0] == '-') ? 1 : 0;
  if (n - i >= 8 && memcmp(text + i, "Infinity", 8) == 0) {
    return text[0] == '-' ? -INFINITY : INFINITY;
  }
  size_t length = decimal_length(text, i, n);
  if (length == 0) {
    return NAN;
  }

  return decimal_value(text, i + length);
}

#define BIG_LIMBS 16

typedef struct {
  uint32_t limb[BIG_LIMBS];
  int n;
} Big;

