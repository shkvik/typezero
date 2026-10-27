#ifndef TZ_DECIMAL_TEXT_H
#define TZ_DECIMAL_TEXT_H

#include "runtime/core/unicode/unicode.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static inline int tz_decimal_digit(uint16_t c) { return c >= '0' && c <= '9'; }

static inline int tz_decimal_plain(const uint16_t *s, size_t n) {
  size_t i = 0;
  if (i < n && s[i] == '-') {
    ++i;
  }
  size_t whole = 0, fraction = 0;
  while (i < n && tz_decimal_digit(s[i])) {
    ++i;
    ++whole;
  }
  if (i < n && s[i] == '.') {
    ++i;
    while (i < n && tz_decimal_digit(s[i])) {
      ++i;
      ++fraction;
    }
    if (whole == 0 && fraction == 0) {
      return 0;
    }
  } else if (whole == 0) {
    return 0;
  }
  if (i < n && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    if (i < n && (s[i] == '+' || s[i] == '-')) {
      ++i;
    }
    size_t start = i;
    while (i < n && tz_decimal_digit(s[i])) {
      ++i;
    }
    if (i == start) {
      return 0;
    }
  }

  return i == n;
}

static inline int tz_decimal_base_digit(uint16_t c, int base) {
  int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : 99;

  return v < base;
}

static inline int tz_decimal_base_ok(const uint16_t *s, size_t n, int base) {
  int upper = 0, lower = 0;
  for (size_t i = 0; i < n; ++i) {
    upper |= s[i] >= 'A' && s[i] <= 'Z';
    lower |= s[i] >= 'a' && s[i] <= 'z';
  }
  if (upper && lower) {
    return 0;
  }
  int dot = 0, previous_digit = 0, any = 0;
  for (size_t i = 0; i < n; ++i) {
    uint16_t c = s[i];
    if (c >= 'A' && c <= 'Z') {
      c = (uint16_t)(c - 'A' + 'a');
    }
    if (tz_decimal_base_digit(c, base)) {
      previous_digit = any = 1;
      continue;
    }
    if (c == '_' && previous_digit && i + 1 < n) {
      previous_digit = 0;
      continue;
    }
    if (c == '.' && (i == 0 || (!dot && previous_digit))) {
      if (i + 1 == n) {
        break;
      }
      dot = 1;
      previous_digit = 0;
      continue;
    }
    return 0;
  }

  return any || dot;
}

static inline int tz_decimal_named(const uint16_t *s, size_t n, const char *word) {
  size_t k = 0;
  while (word[k]) {
    ++k;
  }
  if (n != k) {
    return 0;
  }
  for (size_t i = 0; i < k; ++i) {
    if (s[i] != (uint16_t)word[i]) {
      return 0;
    }
  }

  return 1;
}

static inline int tz_decimal_text_ok(const uint16_t *s, size_t n) {
  if (tz_decimal_plain(s, n)) {
    return 1;
  }
  size_t i = 0, j = n;
  while (i < n && tz_js_space(s[i])) {
    ++i;
  }
  if (i < n && s[i] == '+' && !(i + 1 < n && s[i + 1] == '-')) {
    ++i;
  }
  while (j > i && tz_js_space(s[j - 1])) {
    --j;
  }
  const uint16_t *t = s + i;
  size_t m = j - i;
  int neg = m > 0 && t[0] == '-';
  const uint16_t *body = t + neg;
  size_t bn = m - (size_t)neg;
  if (tz_decimal_named(body, bn, "Infinity") || tz_decimal_named(body, bn, "NaN")) {
    return 0;
  }
  if (bn > 2 && body[0] == '0' && body[2] != '.' &&
      ((body[1] | 0x20) == 'x' || (body[1] | 0x20) == 'b' || (body[1] | 0x20) == 'o')) {
    int base = (body[1] | 0x20) == 'x' ? 16 : (body[1] | 0x20) == 'b' ? 2 : 8;
    return tz_decimal_base_ok(body + 2, bn - 2, base);
  }
  uint16_t local[256];
  uint16_t *clean = m <= 256 ? local : (uint16_t *)malloc(m * sizeof(uint16_t));
  if (!clean) {
    return 0;
  }
  size_t k = 0;
  for (size_t p = 0; p < m; ++p) {
    if (t[p] == '_' && p > 0 && tz_decimal_digit(t[p - 1]) && p + 1 < m &&
        tz_decimal_digit(t[p + 1])) {
      continue;
    }
    clean[k++] = t[p];
  }
  int ok = tz_decimal_plain(clean, k);
  if (clean != local) {
    free(clean);
  }

  return ok;
}

#endif
