#include "runtime/core/output/output.h"
#include "runtime/core/output/stack_trace.h"
#include "runtime/core/array/array.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/error/error.h"
#include "runtime/core/group/group.h"
#include "runtime/core/number/number.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/decimal/decimal/decimal.h"
#include "runtime/library/fs/readline/readline.h"
#include "runtime/library/intl/intl/intl.h"
#include "runtime/library/json/json/json.h"
#include "runtime/library/process/process/process.h"
#include "runtime/library/process/signals/signals.h"
#include "runtime/library/reflect/meta/meta.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <unistd.h>
#endif

typedef struct {
#ifdef _WIN32
  HANDLE handle;
#else
  int fd;
#endif
  int isConsole;
  int gone;
} Stream;

Stream tz_out_stream;
Stream tz_err_stream;

#ifdef _WIN32

static void stream_init(Stream *s, DWORD which) {
  DWORD mode;
  s->handle = GetStdHandle(which);
  s->isConsole =
      s->handle != NULL && s->handle != INVALID_HANDLE_VALUE && GetConsoleMode(s->handle, &mode);
}

static size_t utf8_complete_prefix(const char *data, size_t len) {
  size_t i = len, continuation = 0;
  while (continuation < 3 && i > 0 && ((unsigned char)data[i - 1] & 0xC0) == 0x80) {
    --i;
    ++continuation;
  }
  if (i == 0) {
    return len;
  }
  unsigned char lead = (unsigned char)data[i - 1];
  size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;

  return continuation + 1 >= need ? len : i - 1;
}

static void stream_write(Stream *s, const char *data, size_t len) {
  if (s->handle == NULL || s->handle == INVALID_HANDLE_VALUE) {
    return;
  }
  if (!s->isConsole) {
    while (len > 0) {
      DWORD chunk = len > 0x40000000 ? 0x40000000 : (DWORD)len, written = 0;
      if (!WriteFile(s->handle, data, chunk, &written, NULL) || written == 0) {
        DWORD failure = GetLastError();
        if (failure == ERROR_NO_DATA || failure == ERROR_BROKEN_PIPE) {
          s->gone = 1;
        }
        return;
      }
      data += written;
      len -= written;
    }
    return;
  }
  wchar_t wide[4096];
  while (len > 0) {
    size_t chunk = len < 4096 ? len : utf8_complete_prefix(data, 4096);
    int count = MultiByteToWideChar(CP_UTF8, 0, data, (int)chunk, wide, 4096);
    DWORD written;
    if (count > 0) {
      WriteConsoleW(s->handle, wide, (DWORD)count, &written, NULL);
    }
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
    if (written <= 0) {
      if (written < 0 && errno == EPIPE) {
        s->gone = 1;
      }
      return;
    }
    data += written;
    len -= (size_t)written;
  }
}

#endif

char tz_out_buf[8192];
size_t tz_out_len;
Stream *tz_out_target = &tz_out_stream;

TZ_NOINLINE void tz_out_flush(int all) {
  size_t n = all ? tz_out_len : utf8_complete_prefix(tz_out_buf, tz_out_len);
  stream_write(tz_out_target, tz_out_buf, n);
  memmove(tz_out_buf, tz_out_buf + n, tz_out_len - n);
  tz_out_len -= n;
}

void tz_err_begin(void) {
  TZ_ORDERED();
  tz_out_flush(1);
  tz_out_target = &tz_err_stream;
}

void tz_err_end(void) {
  TZ_ORDERED();
  tz_out_flush(1);
  tz_out_target = &tz_out_stream;
}

void tz_out_idle(void) {
  if (tz_out_len != 0) {
    tz_out_flush(!tz_out_target->isConsole);
  }
}

void tz_out_rescue(void) {
  size_t n = __atomic_load_n(&tz_out_len, __ATOMIC_ACQUIRE);
  if (n > sizeof tz_out_buf) {
    n = sizeof tz_out_buf;
  }
  if (n == 0) {
    return;
  }
  stream_write(tz_out_target, tz_out_buf, n);
  __atomic_compare_exchange_n(&tz_out_len, &n, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

TZ_NOINLINE void tz_out_write_slow(const char *data, size_t len) {
  tz_out_flush(1);
  if (len >= sizeof tz_out_buf) {
    stream_write(&tz_out_stream, data, len);
    return;
  }
  memcpy(tz_out_buf + tz_out_len, data, len);
  tz_out_len += len;
}

void tz_out_write(const char *data, size_t len) {
  TZ_ORDERED();
  if (len > sizeof tz_out_buf - tz_out_len) {
    tz_out_write_slow(data, len);
    return;
  }
  memcpy(tz_out_buf + tz_out_len, data, len);
  tz_out_len += len;
}

void tz_err_raw(const char *data, size_t len) { stream_write(&tz_err_stream, data, len); }

void tz_thread_stack_init(void) {
#ifdef _WIN32
  ULONG guarantee = 64 * 1024;
  SetThreadStackGuarantee(&guarantee);
#endif
}

int32_t tz_stdout_write(void *obj, uint64_t meta) {
  tz_print_string(obj, meta);
  if (tz_out_target->isConsole) {
    tz_out_flush(0);
  }

  return 1;
}

void tz_std_write_bytes(int32_t fd, const uint8_t *data, size_t len) {
  if (fd == 2) {
    tz_err_begin();
  }
  tz_out_write((const char *)data, len);
  if (fd == 2) {
    tz_err_end();
  } else if (tz_out_target->isConsole) {
    tz_out_flush(0);
  }
}

int32_t tz_stderr_write(void *obj, uint64_t meta) {
  tz_err_begin();
  tz_print_string(obj, meta);
  tz_err_end();

  return 1;
}

int32_t tz_is_tty(int64_t fd) { return (fd == 2 ? &tz_err_stream : &tz_out_stream)->isConsole; }

int32_t tz_std_gone(int32_t fd) { return (fd == 2 ? &tz_err_stream : &tz_out_stream)->gone; }

size_t tz_format_f64(double value, char *out) {
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
  if (value == 0) {
    out[0] = '0';
    return 1;
  }

  char digits[20];
  int32_t exponent;
  int k = tz_f64_digits(value, digits, &exponent);

  int n = exponent + k;
  if (k <= n && n <= 21) {
    memcpy(p, digits, (size_t)k);
    p += k;
    for (int i = k; i < n; ++i) {
      *p++ = '0';
    }
  } else if (0 < n && n <= 21) {
    memcpy(p, digits, (size_t)n);
    p += n;
    *p++ = '.';
    memcpy(p, digits + n, (size_t)(k - n));
    p += k - n;
  } else if (-6 < n && n <= 0) {
    *p++ = '0';
    *p++ = '.';
    for (int i = n; i < 0; ++i) {
      *p++ = '0';
    }
    memcpy(p, digits, (size_t)k);
    p += k;
  } else {
    *p++ = digits[0];
    if (k > 1) {
      *p++ = '.';
      memcpy(p, digits + 1, (size_t)(k - 1));
      p += k - 1;
    }
    *p++ = 'e';
    *p++ = n - 1 < 0 ? '-' : '+';
    int e = abs(n - 1);
    if (e >= 100) {
      *p++ = (char)('0' + e / 100);
    }
    if (e >= 10) {
      *p++ = (char)('0' + e / 10 % 10);
    }
    *p++ = (char)('0' + e % 10);
  }

  return (size_t)(p - out);
}

TZ_NOINLINE int32_t tz_to_int32_slow(double x) {
  if (!isfinite(x)) {
    return 0;
  }
  double m = fmod(trunc(x), 4294967296.0);
  if (m < 0) {
    m += 4294967296.0;
  }
  if (m >= 2147483648.0) {
    m -= 4294967296.0;
  }

  return (int32_t)m;
}

TZ_HOT int32_t tz_to_int32(double x) {
  if (x > -2147483649.0 && x < 2147483648.0) {
    return (int32_t)x;
  }

  return tz_to_int32_slow(x);
}

TZ_COLD TZ_NORETURN void tz_panic_does_not_fit(double x, const char *type) {
  char text[32];
  text[tz_format_f64(x, text)] = 0;
  tz_panicf("%s does not fit in %s", text, type);
}

static void err_stream_ready(void) {
#ifdef _WIN32
  if (tz_err_stream.handle == NULL) {
    stream_init(&tz_err_stream, STD_ERROR_HANDLE);
  }
#else
  if (tz_err_stream.fd == 0) {
    stream_init(&tz_err_stream, 2);
  }
#endif
}

TZ_NORETURN void tz_panic(const char *message, int64_t len) {
  if (len < 0) {
    len = (int64_t)strlen(message);
  }
  err_stream_ready();
  tz_out_flush(1);
  stream_write(&tz_err_stream, "panic: ", 7);
  stream_write(&tz_err_stream, message, (size_t)len);
  stream_write(&tz_err_stream, "\n", 1);
  tz_stack_print(NULL);
  exit(2);
}

TZ_NORETURN void tz_panicf(const char *format, ...) {
  char message[256];
  va_list args;
  va_start(args, format);
  int len = vsnprintf(message, sizeof message, format, args);
  va_end(args);
  if (len < 0) {
    len = 0;
  }
  if ((size_t)len >= sizeof message) {
    len = (int)sizeof message - 1;
  }
  tz_panic(message, len);
}
