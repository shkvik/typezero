#ifndef TZ_NUMBER_H
#define TZ_NUMBER_H

#include <stddef.h>
#include <stdint.h>

int tz_f64_digits(double value, char *digits, int32_t *exponent);

double tz_number_from_ascii(const char *text, size_t length, int complete);
double tz_parse_int_ascii(const char *text, size_t length, int32_t radix);
double tz_parse_float_ascii(const char *text, size_t length);
int tz_parse_int64_ascii(const char *text, size_t length, int32_t radix, int64_t *out);

#endif
