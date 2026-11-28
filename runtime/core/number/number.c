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

static inline int32_t pow5_bits(int32_t e) { return (int32_t)(((uint32_t)e * 1217359) >> 19) + 1; }

static inline uint32_t log10_pow2(int32_t e) { return ((uint32_t)e * 78913) >> 18; }
static inline uint32_t log10_pow5(int32_t e) { return ((uint32_t)e * 732923) >> 20; }

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

static inline uint64_t mul_shift(uint64_t m, const uint64_t *mul, int32_t j) {
#ifdef __SIZEOF_INT128__
  unsigned __int128 low = (unsigned __int128)m * mul[0];
  unsigned __int128 high = (unsigned __int128)m * mul[1];

  return (uint64_t)(((low >> 64) + high) >> (j - 64));
#else
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

int tz_f64_digits(double value, char *digits, int32_t *exponent) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof bits);
  uint64_t mantissa = bits & ((1ull << 52) - 1);
  uint32_t biased = (uint32_t)(bits >> 52) & 0x7FF;

  uint64_t output;
  int32_t e10;
  int32_t e2 = (int32_t)biased - 1075;
  if (biased != 0 && e2 <= 0 && e2 >= -52 && (mantissa & ((1ull << -e2) - 1)) == 0) {
    output = ((1ull << 52) | mantissa) >> -e2;
    e10 = 0;
    while (output % 10 == 0) {
      output /= 10;
      ++e10;
    }
  } else {
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
        if (mv % 5 == 0) {
          vr_zeros = pow5_divides(mv, q);
        } else if (even) {
          vm_zeros = pow5_divides(mv - 1 - mm_shift, q);
        } else {
          vp -= pow5_divides(mv + 2, q);
        }
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
        vr_zeros = 1;
        if (even) {
          vm_zeros = mm_shift == 1;
        } else {
          --vp;
        }
      } else if (q < 63) {
        vr_zeros = pow2_divides(mv, q);
      }
    }

    int32_t removed = 0;
    uint32_t last = 0;
    if (vm_zeros || vr_zeros) {
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
      if (vr_zeros && last == 5 && vr % 2 == 0) {
        last = 4;
      }
      output = vr + ((vr == vm && (!even || !vm_zeros)) || last >= 5);
    } else {
      int round_up = 0;
      if (vp / 100 > vm / 100) {
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
  for (int i = 0; i < count; ++i) {
    digits[i] = reversed[count - 1 - i];
  }
  *exponent = e10;

  return count;
}
