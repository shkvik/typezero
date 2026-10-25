// Numbers and text: the shortest digits of a double, and numbers read from text.

#ifndef TZ_NUMBER_H
#define TZ_NUMBER_H

#include <stddef.h>
#include <stdint.h>

// The shortest digits of a finite positive double (number.c), at most 17;
// returns their count, and the double is those digits times 10^*exponent.
int tz_f64_digits(double value, char *digits, int32_t *exponent);

// Numbers in text (number.c), for Number(s), parseInt and parseFloat once
// string.c has taken off the whitespace around them: `text` holds the ASCII
// characters of the string up to its first other one, and `complete` says
// whether that was all of it.
double tz_number_from_ascii(const char *text, size_t length, int complete);
double tz_parse_int_ascii(const char *text, size_t length, int32_t radix);
double tz_parse_float_ascii(const char *text, size_t length);

#endif
