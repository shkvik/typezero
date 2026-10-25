// The parser of date strings, for `new Date(s)` and Date.parse(s), and the
// arithmetic of time values (see date.h).
//
// The parser is a port of V8's DateParser (src/date/dateparser*), so that a
// string gives what it gives in node: the ISO format of the standard
// (`2026-10-23T10:00:00.000+06:00`), and the formats the standard leaves to
// each engine, which V8 reads as Safari does: "2026/10/23", "Oct 23 2026
// 10:00", "Fri, 23 Oct 2026 10:00:00 GMT" (what toUTCString writes), what
// toString writes, and many more. Words before the first number are skipped,
// text in parentheses too.
//
// tz builds this file into itself as C++ (src/support/date_syntax.cpp) to
// check the date strings written in a program, so the code keeps to what C
// and C++ share.

#include "date.h"

#include <math.h>

//===----------------------------------------------------------------------===//
// Time values
//===----------------------------------------------------------------------===//

// ToIntegerOrInfinity, with -0 as +0.
static double to_integer(double x) {
  if (isnan(x))
    return 0;
  if (!isfinite(x))
    return x;
  return trunc(x) + 0.0;
}

// As V8's MakeDay: the years and months it takes are limited, so that the
// day fits in an int on the way.
double tz_date_make_day(double year, double month, double date) {
  if (!(year >= -1000000 && year <= 1000000) || !(month >= -10000000 && month <= 10000000) ||
      !isfinite(date))
    return NAN;
  int y = (int)year;
  int m = (int)month;
  y += m / 12;
  m %= 12;
  if (m < 0) {
    m += 12;
    y -= 1;
  }
  // Shifted by a multiple of 400 years (less one), so that the divisions
  // below see positive numbers.
  const int delta = 399999;
  const int base =
      365 * (1970 + delta) + (1970 + delta) / 4 - (1970 + delta) / 100 + (1970 + delta) / 400;
  int day = 365 * (y + delta) + (y + delta) / 4 - (y + delta) / 100 + (y + delta) / 400 - base;
  static const int before[2][12] = {
      {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334},
      {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335},
  };
  int leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
  day += before[leap][m];
  return (double)(day - 1) + to_integer(date);
}

double tz_date_make_time(double hour, double min, double sec, double ms) {
  if (!isfinite(hour) || !isfinite(min) || !isfinite(sec) || !isfinite(ms))
    return NAN;
  return to_integer(hour) * 3600000.0 + to_integer(min) * 60000.0 + to_integer(sec) * 1000.0 +
         to_integer(ms);
}

double tz_date_make_date(double day, double time) {
  if (!isfinite(day) || !isfinite(time))
    return NAN;
  if (time == 0.0 && day != 0.0)
    return day * 86400000.0;
  return time + day * 86400000.0;
}

double tz_date_time_clip(double time) {
  if (!(time >= -TZ_DATE_MAX_TIME && time <= TZ_DATE_MAX_TIME))
    return NAN;
  return to_integer(time);
}

//===----------------------------------------------------------------------===//
// Reading characters
//===----------------------------------------------------------------------===//

enum { NONE = 0x7FFFFFFF }; // V8's kNone: no value yet

// Digits of a number past the first nine are dropped.
enum { MAX_SIGNIFICANT_DIGITS = 9 };

typedef struct {
  const uint16_t *units;
  size_t length;
  size_t index; // one past the current character
  uint32_t ch;  // the current character, 0 at the end (and at a NUL, as in V8)
} Reader;

static void next_char(Reader *in) {
  in->ch = in->index < in->length ? in->units[in->index] : 0;
  in->index++;
}

static int is_digit(uint32_t c) { return c >= '0' && c <= '9'; }

// WhiteSpace of ECMAScript: tab, vertical tab, form feed, the BOM and the
// spaces of Unicode.
static int is_white_space(uint32_t c) {
  switch (c) {
  case 0x09:
  case 0x0B:
  case 0x0C:
  case 0x20:
  case 0xA0:
  case 0x1680:
  case 0x202F:
  case 0x205F:
  case 0x3000:
  case 0xFEFF:
    return 1;
  default:
    return c >= 0x2000 && c <= 0x200A;
  }
}

static int is_white_space_or_line_terminator(uint32_t c) {
  return is_white_space(c) || c == 0x0A || c == 0x0D || c == 0x2028 || c == 0x2029;
}

static int skip_char(Reader *in, uint32_t c) {
  if (in->ch != c)
    return 0;
  next_char(in);
  return 1;
}

static int read_number(Reader *in) {
  int n = 0, i = 0;
  while (in->ch == '0')
    next_char(in);
  while (is_digit(in->ch)) {
    if (i < MAX_SIGNIFICANT_DIGITS)
      n = n * 10 + (int)(in->ch - '0');
    i++;
    next_char(in);
  }
  return n;
}

// A word of characters from 'A' on that are not spaces: its first three in
// lower case (in ASCII's way, by setting bit 5), and its length.
static int read_word(Reader *in, uint32_t prefix[3]) {
  int length = 0;
  for (; in->ch >= 'A' && !is_white_space(in->ch); next_char(in), length++)
    if (length < 3)
      prefix[length] = in->ch | 0x20;
  for (int i = length; i < 3; i++)
    prefix[i] = 0;
  return length;
}

//===----------------------------------------------------------------------===//
// Tokens
//===----------------------------------------------------------------------===//

enum {
  TOKEN_INVALID = -6,
  TOKEN_UNKNOWN = -5,
  TOKEN_SPACE = -4,
  TOKEN_NUMBER = -3,
  TOKEN_SYMBOL = -2,
  TOKEN_END = -1,
  // Words, from 0: the kind of keyword.
  WORD_OTHER = 0,
  WORD_MONTH = 1,
  WORD_ZONE = 2,
  WORD_T = 3, // the separator of an ISO date and time
  WORD_AM_PM = 4,
};

typedef struct {
  int tag, length, value;
} Token;

static Token make_token(int tag, int length, int value) {
  Token t;
  t.tag = tag;
  t.length = length;
  t.value = value;
  return t;
}

static const struct {
  char prefix[3];
  int type, value;
} keywords[] = {
    {{'j', 'a', 'n'}, WORD_MONTH, 1},  {{'f', 'e', 'b'}, WORD_MONTH, 2},
    {{'m', 'a', 'r'}, WORD_MONTH, 3},  {{'a', 'p', 'r'}, WORD_MONTH, 4},
    {{'m', 'a', 'y'}, WORD_MONTH, 5},  {{'j', 'u', 'n'}, WORD_MONTH, 6},
    {{'j', 'u', 'l'}, WORD_MONTH, 7},  {{'a', 'u', 'g'}, WORD_MONTH, 8},
    {{'s', 'e', 'p'}, WORD_MONTH, 9},  {{'o', 'c', 't'}, WORD_MONTH, 10},
    {{'n', 'o', 'v'}, WORD_MONTH, 11}, {{'d', 'e', 'c'}, WORD_MONTH, 12},
    {{'a', 'm', 0}, WORD_AM_PM, 0},    {{'p', 'm', 0}, WORD_AM_PM, 12},
    {{'u', 't', 0}, WORD_ZONE, 0},     {{'u', 't', 'c'}, WORD_ZONE, 0},
    {{'z', 0, 0}, WORD_ZONE, 0},       {{'g', 'm', 't'}, WORD_ZONE, 0},
    {{'c', 'd', 't'}, WORD_ZONE, -5},  {{'c', 's', 't'}, WORD_ZONE, -6},
    {{'e', 'd', 't'}, WORD_ZONE, -4},  {{'e', 's', 't'}, WORD_ZONE, -5},
    {{'m', 'd', 't'}, WORD_ZONE, -6},  {{'m', 's', 't'}, WORD_ZONE, -7},
    {{'p', 'd', 't'}, WORD_ZONE, -7},  {{'p', 's', 't'}, WORD_ZONE, -8},
    {{'t', 0, 0}, WORD_T, 0},
};

// A word longer than its keyword names it only for months: "October".
static Token keyword(const uint32_t prefix[3], int length) {
  for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) {
    int j = 0;
    while (j < 3 && prefix[j] == (uint32_t)(unsigned char)keywords[i].prefix[j])
      j++;
    if (j == 3 && (length <= 3 || keywords[i].type == WORD_MONTH))
      return make_token(keywords[i].type, length, keywords[i].value);
  }
  return make_token(WORD_OTHER, length, 0);
}

static Token scan(Reader *in) {
  size_t start = in->index;
  if (in->ch == 0)
    return make_token(TOKEN_END, 0, -1);
  if (is_digit(in->ch)) {
    int n = read_number(in);
    return make_token(TOKEN_NUMBER, (int)(in->index - start), n);
  }
  static const char symbols[] = ":-+.)";
  for (const char *s = symbols; *s; s++)
    if (skip_char(in, (uint32_t)*s))
      return make_token(TOKEN_SYMBOL, 1, *s);
  if (in->ch >= 'A' && !is_white_space(in->ch)) {
    uint32_t prefix[3];
    int length = read_word(in, prefix);
    return keyword(prefix, length);
  }
  if (is_white_space_or_line_terminator(in->ch)) {
    next_char(in);
    return make_token(TOKEN_SPACE, (int)(in->index - start), -1);
  }
  if (in->ch == '(') {
    int balance = 0;
    do {
      if (in->ch == ')')
        --balance;
      else if (in->ch == '(')
        ++balance;
      next_char(in);
    } while (balance > 0 && in->ch);
    return make_token(TOKEN_UNKNOWN, 1, -1);
  }
  next_char(in);
  return make_token(TOKEN_UNKNOWN, 1, -1);
}

// The tokens with one of look-ahead.
typedef struct {
  Reader *in;
  Token next;
} Scanner;

static Token next_token(Scanner *s) {
  Token t = s->next;
  s->next = scan(s->in);
  return t;
}

static int is_symbol(Token t, char c) { return t.tag == TOKEN_SYMBOL && t.value == c; }
static int is_sign(Token t) { return is_symbol(t, '+') || is_symbol(t, '-'); }
static int sign_of(Token t) { return 44 - t.value; } // '+' is 43, '-' 45
static int is_fixed_number(Token t, int length) {
  return t.tag == TOKEN_NUMBER && t.length == length;
}
static int is_keyword(Token t) { return t.tag >= 0; }
static int is_z(Token t) { return t.tag == WORD_ZONE && t.length == 1 && t.value == 0; }

static int skip_symbol(Scanner *s, char c) {
  if (!is_symbol(s->next, c))
    return 0;
  s->next = scan(s->in);
  return 1;
}

static int between(int x, int lo, int hi) { return (unsigned)(x - lo) <= (unsigned)(hi - lo); }
static int is_month(int x) { return between(x, 1, 12); }
static int is_day(int x) { return between(x, 1, 31); }
static int is_hour(int x) { return between(x, 0, 23); }
static int is_minute(int x) { return between(x, 0, 59); }
static int is_second(int x) { return between(x, 0, 59); }
static int is_hour12(int x) { return between(x, 0, 12); }
static int is_millisecond(int x) { return between(x, 0, 999); }

// A number of milliseconds of any length: "5" is 500, "12345" is 123.
static int read_milliseconds(Token number) {
  int n = number.value, length = number.length;
  if (length == 1) {
    n *= 100;
  } else if (length == 2) {
    n *= 10;
  } else if (length > 3) {
    if (length > MAX_SIGNIFICANT_DIGITS)
      length = MAX_SIGNIFICANT_DIGITS;
    int factor = 1;
    do {
      factor *= 10;
      length--;
    } while (length > 3);
    n /= factor;
  }
  return n;
}

//===----------------------------------------------------------------------===//
// Composing the fields
//===----------------------------------------------------------------------===//

typedef struct {
  int comp[3];
  int index;
  int named_month;
  int iso;
} Day;

static int day_add(Day *d, int n) {
  if (d->index == 3)
    return 0;
  d->comp[d->index++] = n;
  return 1;
}

static int day_write(Day *d, TzDateFields *out) {
  if (d->index < 1)
    return 0;
  // The day and the month default to 1.
  while (d->index < 3)
    d->comp[d->index++] = 1;
  int year = 0; // 2000, for KJS
  int month = NONE, day = NONE;
  if (d->named_month == NONE) {
    if (d->iso || (d->index == 3 && !is_day(d->comp[0]))) {
      year = d->comp[0]; // YMD
      month = d->comp[1];
      day = d->comp[2];
    } else {
      month = d->comp[0]; // MD(Y)
      day = d->comp[1];
      if (d->index == 3)
        year = d->comp[2];
    }
  } else {
    month = d->named_month;
    if (d->index == 1) {
      day = d->comp[0];
    } else if (!is_day(d->comp[0])) {
      year = d->comp[0]; // YMD, MYD or YDM
      day = d->comp[1];
    } else {
      day = d->comp[0]; // DMY, MDY or DYM
      year = d->comp[1];
    }
  }
  if (!d->iso) {
    if (between(year, 0, 49))
      year += 2000;
    else if (between(year, 50, 99))
      year += 1900;
  }
  if (!is_month(month) || !is_day(day))
    return 0;
  out->year = year;
  out->month = month - 1;
  out->day = day;
  return 1;
}

typedef struct {
  int comp[4];
  int index;
  int hour_offset;
} Time;

static int time_is_expecting(const Time *t, int n) {
  return (t->index == 1 && is_minute(n)) || (t->index == 2 && is_second(n)) ||
         (t->index == 3 && is_millisecond(n));
}

static int time_add(Time *t, int n) {
  if (t->index >= 4)
    return 0;
  t->comp[t->index++] = n;
  return 1;
}

static int time_add_final(Time *t, int n) {
  if (!time_add(t, n))
    return 0;
  while (t->index < 4)
    t->comp[t->index++] = 0;
  return 1;
}

static int time_write(Time *t, TzDateFields *out) {
  while (t->index < 4)
    t->comp[t->index++] = 0;
  int hour = t->comp[0], minute = t->comp[1], second = t->comp[2], ms = t->comp[3];
  if (t->hour_offset != NONE) {
    if (!is_hour12(hour))
      return 0;
    hour %= 12;
    hour += t->hour_offset;
  }
  if (!is_hour(hour) || !is_minute(minute) || !is_second(second) || !is_millisecond(ms)) {
    // 24:00:00.000 is the end of the day.
    if (hour != 24 || minute != 0 || second != 0 || ms != 0)
      return 0;
  }
  out->hour = hour;
  out->minute = minute;
  out->second = second;
  out->ms = ms;
  return 1;
}

typedef struct {
  int sign, hour, minute;
} Zone;

static void zone_set(Zone *z, int hours) {
  z->sign = hours < 0 ? -1 : 1;
  z->hour = hours * z->sign;
  z->minute = 0;
}

static int zone_is_expecting(const Zone *z, int n) {
  return z->hour != NONE && z->minute == NONE && is_minute(n);
}

static int zone_is_utc(const Zone *z) { return z->hour == 0 && z->minute == 0; }

static int zone_write(Zone *z, TzDateFields *out) {
  if (z->sign == NONE) {
    out->offset = NAN;
    return 1;
  }
  if (z->hour == NONE)
    z->hour = 0;
  if (z->minute == NONE)
    z->minute = 0;
  // In unsigned arithmetic, as V8 does, which then rejects what does not fit
  // in a small integer.
  unsigned total = (unsigned)z->hour * 3600u + (unsigned)z->minute * 60u;
  if (total > 0x7FFFFFFFu)
    return 0;
  out->offset = z->sign < 0 ? -(double)total : (double)total;
  return 1;
}

//===----------------------------------------------------------------------===//
// The parser
//===----------------------------------------------------------------------===//

// The ISO format: [('-'|'+')yy]yyyy[-MM[-DD]][THH:mm[:ss[.sss]][Z|(+|-)hh:mm]].
// Returns the first token it does not take, for the legacy parser, or the
// end; an invalid token for a string that is surely not a date.
static Token parse_iso(Scanner *s, Day *day, Time *time, Zone *zone) {
  if (is_sign(s->next)) {
    // The sign stays for the legacy parser, so that what is not a date is
    // found to be none.
    Token sign = next_token(s);
    if (!is_fixed_number(s->next, 6))
      return sign;
    int year = next_token(s).value;
    if (sign_of(sign) < 0 && year == 0)
      return sign;
    day_add(day, sign_of(sign) * year);
  } else if (is_fixed_number(s->next, 4)) {
    day_add(day, next_token(s).value);
  } else {
    return next_token(s);
  }
  if (skip_symbol(s, '-')) {
    if (!is_fixed_number(s->next, 2) || !is_month(s->next.value))
      return next_token(s);
    day_add(day, next_token(s).value);
    if (skip_symbol(s, '-')) {
      if (!is_fixed_number(s->next, 2) || !is_day(s->next.value))
        return next_token(s);
      day_add(day, next_token(s).value);
    }
  }
  if (s->next.tag != WORD_T) {
    if (s->next.tag != TOKEN_END)
      return next_token(s);
  } else {
    next_token(s);
    if (!is_fixed_number(s->next, 2) || !between(s->next.value, 0, 24))
      return make_token(TOKEN_INVALID, 0, -1);
    // 24:00[:00[.000]], and no other time with hour 24.
    int hour_is_24 = s->next.value == 24;
    time_add(time, next_token(s).value);
    if (!skip_symbol(s, ':'))
      return make_token(TOKEN_INVALID, 0, -1);
    if (!is_fixed_number(s->next, 2) || !is_minute(s->next.value) ||
        (hour_is_24 && s->next.value > 0))
      return make_token(TOKEN_INVALID, 0, -1);
    time_add(time, next_token(s).value);
    if (skip_symbol(s, ':')) {
      if (!is_fixed_number(s->next, 2) || !is_second(s->next.value) ||
          (hour_is_24 && s->next.value > 0))
        return make_token(TOKEN_INVALID, 0, -1);
      time_add(time, next_token(s).value);
      if (skip_symbol(s, '.')) {
        if (s->next.tag != TOKEN_NUMBER || (hour_is_24 && s->next.value > 0))
          return make_token(TOKEN_INVALID, 0, -1);
        // More or fewer than three digits are fine.
        time_add(time, read_milliseconds(next_token(s)));
      }
    }
    if (is_z(s->next)) {
      next_token(s);
      zone_set(zone, 0);
    } else if (is_sign(s->next)) {
      zone->sign = next_token(s).value == '+' ? 1 : -1;
      if (is_fixed_number(s->next, 4)) {
        // hhmm, an extension.
        int hourmin = next_token(s).value;
        int hour = hourmin / 100, min = hourmin % 100;
        if (!is_hour(hour) || !is_minute(min))
          return make_token(TOKEN_INVALID, 0, -1);
        zone->hour = hour;
        zone->minute = min;
      } else {
        if (!is_fixed_number(s->next, 2) || !is_hour(s->next.value))
          return make_token(TOKEN_INVALID, 0, -1);
        zone->hour = next_token(s).value;
        if (!skip_symbol(s, ':'))
          return make_token(TOKEN_INVALID, 0, -1);
        if (!is_fixed_number(s->next, 2) || !is_minute(s->next.value))
          return make_token(TOKEN_INVALID, 0, -1);
        zone->minute = next_token(s).value;
      }
    }
    if (s->next.tag != TOKEN_END)
      return make_token(TOKEN_INVALID, 0, -1);
  }
  // A date without a time is UTC, a date with a time local (ECMA-262,
  // 21.4.3.2).
  if (zone->hour == NONE && time->index == 0)
    zone_set(zone, 0);
  day->iso = 1;
  return make_token(TOKEN_END, 0, -1);
}

int tz_date_parse_fields(const uint16_t *units, size_t length, TzDateFields *out) {
  Reader in;
  in.units = units;
  in.length = length;
  in.index = 0;
  next_char(&in);
  Scanner s;
  s.in = &in;
  s.next = scan(&in);
  Day day = {{0, 0, 0}, 0, NONE, 0};
  Time time = {{0, 0, 0, 0}, 0, NONE};
  Zone zone = {NONE, NONE, NONE};

  Token token = parse_iso(&s, &day, &time, &zone);
  if (token.tag == TOKEN_INVALID)
    return 0;
  int iso_date_only = token.tag == TOKEN_END && day.iso && time.index == 0;
  int has_read_number = day.index > 0;
  // Whatever is left goes to the legacy parser.
  for (; token.tag != TOKEN_END; token = next_token(&s)) {
    if (token.tag == TOKEN_NUMBER) {
      has_read_number = 1;
      int n = token.value;
      if (skip_symbol(&s, ':')) {
        if (skip_symbol(&s, ':')) {
          // n::
          if (time.index != 0)
            return 0;
          time_add(&time, n);
          time_add(&time, 0);
        } else {
          // n:
          if (!time_add(&time, n))
            return 0;
          if (is_symbol(s.next, '.'))
            next_token(&s);
        }
      } else if (skip_symbol(&s, '.') && time_is_expecting(&time, n)) {
        time_add(&time, n);
        if (s.next.tag != TOKEN_NUMBER)
          return 0;
        int ms = read_milliseconds(next_token(&s));
        if (ms < 0)
          return 0;
        time_add_final(&time, ms);
      } else if (zone_is_expecting(&zone, n)) {
        zone.minute = n;
      } else if (time_is_expecting(&time, n)) {
        time_add_final(&time, n);
        // The end, a space, "Z", "+" or "-" must follow a complete time.
        Token peek = s.next;
        if (peek.tag != TOKEN_END && peek.tag != TOKEN_SPACE && !is_z(peek) && !is_sign(peek))
          return 0;
      } else {
        if (!day_add(&day, n))
          return 0;
        skip_symbol(&s, '-');
      }
    } else if (is_keyword(token)) {
      if (token.tag == WORD_AM_PM && time.index != 0) {
        time.hour_offset = token.value;
      } else if (token.tag == WORD_MONTH) {
        day.named_month = token.value;
        skip_symbol(&s, '-');
      } else if (token.tag == WORD_ZONE && has_read_number) {
        zone_set(&zone, token.value);
      } else {
        // Words are not allowed once a number is read, and the first number
        // must be apart from the words before it.
        if (has_read_number)
          return 0;
        if (s.next.tag == TOKEN_NUMBER)
          return 0;
      }
    } else if (is_sign(token) && (zone_is_utc(&zone) || time.index != 0)) {
      // An offset from UTC, after "UTC" or a time.
      zone.sign = sign_of(token) < 0 ? -1 : 1;
      int n = 0, digits = 0;
      if (s.next.tag == TOKEN_NUMBER) {
        Token number = next_token(&s);
        digits = number.length;
        n = number.value;
      }
      has_read_number = 1;
      if (is_symbol(s.next, ':')) {
        zone.hour = n;
        zone.minute = NONE;
      } else if (digits == 1 || digits == 2) {
        zone.hour = n; // GMT-8
        zone.minute = 0;
      } else if (digits == 3 || digits == 4) {
        zone.hour = n / 100; // GMT-0800
        zone.minute = n % 100;
      } else {
        return 0;
      }
    } else if ((is_sign(token) || is_symbol(token, ')')) && has_read_number) {
      return 0;
    }
    // Anything else is skipped.
  }
  if (!day_write(&day, out) || !time_write(&time, out) || !zone_write(&zone, out))
    return 0;
  out->date_only = iso_date_only;
  return 1;
}
