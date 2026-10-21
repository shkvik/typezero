// The typezero runtime, linked into every program: buffered output for
// console.log, number formatting and panics.
//
// The compiler calls the functions declared in src/codegen/CodeGen.cpp
// (CodeGen::declareRuntime); keep both in sync.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define TZ_NORETURN __declspec(noreturn)
#else
#include <unistd.h>
#define TZ_NORETURN __attribute__((noreturn))
#endif

//===----------------------------------------------------------------------===//
// Writing to stdout and stderr
//===----------------------------------------------------------------------===//

typedef struct {
#ifdef _WIN32
  HANDLE handle;
#else
  int fd;
#endif
  int isConsole;
} Stream;

static Stream out_stream;
static Stream err_stream;

#ifdef _WIN32

static void stream_init(Stream *s, DWORD which) {
  DWORD mode;
  s->handle = GetStdHandle(which);
  s->isConsole = s->handle != NULL && s->handle != INVALID_HANDLE_VALUE &&
                 GetConsoleMode(s->handle, &mode);
}

// Returns the length of the longest prefix of `data` that does not end in the
// middle of a UTF-8 sequence.
static size_t utf8_complete_prefix(const char *data, size_t len) {
  size_t i = len, continuation = 0;
  while (continuation < 3 && i > 0 && ((unsigned char)data[i - 1] & 0xC0) == 0x80) {
    --i;
    ++continuation;
  }
  if (i == 0)
    return len;
  unsigned char lead = (unsigned char)data[i - 1];
  size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
  return continuation + 1 >= need ? len : i - 1;
}

static void stream_write(Stream *s, const char *data, size_t len) {
  if (s->handle == NULL || s->handle == INVALID_HANDLE_VALUE)
    return;
  if (!s->isConsole) {
    while (len > 0) {
      DWORD chunk = len > 0x40000000 ? 0x40000000 : (DWORD)len, written = 0;
      if (!WriteFile(s->handle, data, chunk, &written, NULL) || written == 0)
        return;
      data += written;
      len -= written;
    }
    return;
  }
  // The console shows UTF-8 correctly only if its code page says so, which it
  // usually does not. Writing UTF-16 always works.
  wchar_t wide[4096];
  while (len > 0) {
    size_t chunk = len < 4096 ? len : utf8_complete_prefix(data, 4096);
    int count = MultiByteToWideChar(CP_UTF8, 0, data, (int)chunk, wide, 4096);
    DWORD written;
    if (count > 0)
      WriteConsoleW(s->handle, wide, (DWORD)count, &written, NULL);
    data += chunk;
    len -= chunk;
  }
}

#else

static void stream_init(Stream *s, int fd) {
  s->fd = fd;
  s->isConsole = isatty(fd);
}

static size_t utf8_complete_prefix(const char *data, size_t len) {
  (void)data;
  return len;
}

static void stream_write(Stream *s, const char *data, size_t len) {
  while (len > 0) {
    ssize_t written = write(s->fd, data, len);
    if (written <= 0)
      return;
    data += written;
    len -= (size_t)written;
  }
}

#endif

// stdout is buffered; a console gets every line as soon as it is complete.
static char out_buf[8192];
static size_t out_len;

static void out_flush(int all) {
  size_t n = all ? out_len : utf8_complete_prefix(out_buf, out_len);
  stream_write(&out_stream, out_buf, n);
  memmove(out_buf, out_buf + n, out_len - n);
  out_len -= n;
}

static void out_flush_all(void) { out_flush(1); }

static void out_write(const char *data, size_t len) {
  if (len > sizeof out_buf - out_len) {
    out_flush(1);
    if (len >= sizeof out_buf) {
      stream_write(&out_stream, data, len);
      return;
    }
  }
  memcpy(out_buf + out_len, data, len);
  out_len += len;
}

void tz_rt_init(void) {
#ifdef _WIN32
  stream_init(&out_stream, STD_OUTPUT_HANDLE);
  stream_init(&err_stream, STD_ERROR_HANDLE);
#else
  stream_init(&out_stream, 1);
  stream_init(&err_stream, 2);
#endif
  atexit(out_flush_all);
}

//===----------------------------------------------------------------------===//
// console.log
//===----------------------------------------------------------------------===//

void tz_print_str(const char *data, int64_t len) { out_write(data, (size_t)len); }

void tz_print_char(int32_t c) {
  char ch = (char)c;
  out_write(&ch, 1);
  if (c == '\n' && out_stream.isConsole)
    out_flush(0);
}

static void print_u64(uint64_t value, int negative) {
  char buf[24];
  char *p = buf + sizeof buf;
  do {
    *--p = (char)('0' + value % 10);
    value /= 10;
  } while (value != 0);
  if (negative)
    *--p = '-';
  out_write(p, (size_t)(buf + sizeof buf - p));
}

void tz_print_i64(int64_t value) {
  if (value < 0)
    print_u64(0 - (uint64_t)value, 1);
  else
    print_u64((uint64_t)value, 0);
}

void tz_print_i32(int32_t value) { tz_print_i64(value); }

void tz_print_bool(int32_t value) {
  if (value)
    out_write("true", 4);
  else
    out_write("false", 5);
}

// Formats `value` like JavaScript's Number.prototype.toString(): the shortest
// digits that read back as the same double, in plain notation for decimal
// exponents from -7 to 20 and in exponential notation otherwise.
static size_t format_f64(double value, char *out) {
  char *p = out;
  if (isnan(value)) {
    memcpy(out, "NaN", 3);
    return 3;
  }
  if (value < 0) {
    *p++ = '-';
    value = -value;
  }
  if (isinf(value)) {
    memcpy(p, "Infinity", 8);
    return (size_t)(p - out) + 8;
  }
  if (value == 0) { // also -0, which JavaScript prints as "0"
    out[0] = '0';
    return 1;
  }

  char sci[32];
  for (int precision = 1; precision <= 17; ++precision) {
    snprintf(sci, sizeof sci, "%.*e", precision - 1, value);
    if (strtod(sci, NULL) == value)
      break;
  }

  // `sci` is "d.ddde+XX" or "de+XX". Collect the digits and the exponent.
  char digits[20];
  int k = 0;
  const char *s = sci;
  digits[k++] = *s++;
  if (*s == '.')
    for (++s; *s != 'e'; ++s)
      digits[k++] = *s;
  int exponent = atoi(s + 1);
  while (k > 1 && digits[k - 1] == '0')
    --k;

  int n = exponent + 1; // position of the decimal point relative to the digits
  if (k <= n && n <= 21) {
    memcpy(p, digits, (size_t)k);
    p += k;
    for (int i = k; i < n; ++i)
      *p++ = '0';
  } else if (0 < n && n <= 21) {
    memcpy(p, digits, (size_t)n);
    p += n;
    *p++ = '.';
    memcpy(p, digits + n, (size_t)(k - n));
    p += k - n;
  } else if (-6 < n && n <= 0) {
    *p++ = '0';
    *p++ = '.';
    for (int i = n; i < 0; ++i)
      *p++ = '0';
    memcpy(p, digits, (size_t)k);
    p += k;
  } else {
    *p++ = digits[0];
    if (k > 1) {
      *p++ = '.';
      memcpy(p, digits + 1, (size_t)(k - 1));
      p += k - 1;
    }
    p += snprintf(p, 8, "e%c%d", n - 1 < 0 ? '-' : '+', abs(n - 1));
  }
  return (size_t)(p - out);
}

void tz_print_f64(double value) {
  char buf[64];
  out_write(buf, format_f64(value, buf));
}

//===----------------------------------------------------------------------===//
// Panics
//===----------------------------------------------------------------------===//

TZ_NORETURN void tz_panic(const char *message, int64_t len) {
  out_flush(1);
  stream_write(&err_stream, "panic: ", 7);
  stream_write(&err_stream, message, (size_t)len);
  stream_write(&err_stream, "\n", 1);
  exit(2);
}
