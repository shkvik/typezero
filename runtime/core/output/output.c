#include "runtime/core/output/output.h"
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

static void out_flush_all(void) { tz_out_flush(1); }

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

#ifdef _WIN32
static void crash_trace(EXCEPTION_POINTERS *info) {
  char line[128];
  HMODULE base = GetModuleHandleA(NULL);
  uintptr_t at = (uintptr_t)info->ExceptionRecord->ExceptionAddress;
  int n = snprintf(line, sizeof line, "tz: crash %08lx at +0x%llx (thread %d)\n",
                   (unsigned long)info->ExceptionRecord->ExceptionCode,
                   (unsigned long long)(at - (uintptr_t)base), (int)tz_thread_index());
  stream_write(&tz_err_stream, line, (size_t)n);
  uintptr_t *sp = (uintptr_t *)info->ContextRecord->Rsp;
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
  uintptr_t lo = (uintptr_t)base, hi = lo + nt->OptionalHeader.SizeOfImage;
  int shown = 0;
  for (int i = 0; i < 4096 && shown < 40; ++i) {
    if (sp[i] > lo && sp[i] < hi) {
      n = snprintf(line, sizeof line, "  +0x%llx\n", (unsigned long long)(sp[i] - lo));
      stream_write(&tz_err_stream, line, (size_t)n);
      ++shown;
    }
  }
}

static LONG WINAPI on_exception(EXCEPTION_POINTERS *info) {
  if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      getenv("TZ_CRASH_TRACE")) {
    crash_trace(info);
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  static const char message[] = "panic: stack overflow\n";
  tz_out_flush(1);
  stream_write(&tz_err_stream, message, sizeof message - 1);
  TerminateProcess(GetCurrentProcess(), 2);

  return EXCEPTION_CONTINUE_SEARCH;
}
#endif

extern int32_t tz_group_listing;

void tz_thread_stack_init(void) {
#ifdef _WIN32
  ULONG guarantee = 64 * 1024;
  SetThreadStackGuarantee(&guarantee);
#endif
}

void tz_rt_init(void) {
  tz_group_listing = getenv("TZ_CHECK_LEAKS") != NULL;
#ifdef _WIN32
  stream_init(&tz_out_stream, STD_OUTPUT_HANDLE);
  stream_init(&tz_err_stream, STD_ERROR_HANDLE);
  AddVectoredExceptionHandler(1, on_exception);
#else
  stream_init(&tz_out_stream, 1);
  stream_init(&tz_err_stream, 2);
#endif
  tz_thread_register();
  tz_thread_stack_init();
  tz_parallel_init();
  atexit(out_flush_all);
  tz_signal_init();
  tz_clock_init();
}

void tz_rt_main_done(void) {
  if (!getenv("TZ_CHECK_LEAKS")) {
    tz_out_flush(1);
    exit(tz_process_exit_status());
  }
}

void tz_rt_shutdown(void) {
  tz_process_shutdown();
  tz_signal_shutdown();
  tz_meta_shutdown();
  tz_fs_shutdown();
  tz_intl_shutdown();
  tz_out_flush(1);
  tz_group_shutdown();
  int64_t strings = tz_str_live_objects(), arrays = tz_arr_live_objects();
  int64_t objects = tz_obj_live_objects() + tz_json_live_objects();
  int64_t decimals = tz_dec_live_objects();
  if ((strings != 0 || arrays != 0 || objects != 0 || decimals != 0) && getenv("TZ_CHECK_LEAKS")) {
    char message[224];
    int len =
        snprintf(message, sizeof message,
                 "tz: %lld string object(s), %lld array(s), %lld long decimal(s) and %lld "
                 "object(s) or closure environment(s) leaked\n",
                 (long long)strings, (long long)arrays, (long long)decimals, (long long)objects);
    tz_out_flush(1);
    stream_write(&tz_err_stream, message, (size_t)len);
  }
  exit(tz_process_exit_status());
}

void tz_print_char(int32_t c) {
  char ch = (char)c;
  tz_out_write(&ch, 1);
  if (c == '\n' && tz_out_target->isConsole) {
    tz_out_flush(0);
  }
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

static void print_u64(uint64_t value, int negative) {
  char buf[24];
  char *p = buf + sizeof buf;
  do {
    *--p = (char)('0' + value % 10);
    value /= 10;
  } while (value != 0);
  if (negative) {
    *--p = '-';
  }
  tz_out_write(p, (size_t)(buf + sizeof buf - p));
}

void tz_print_i64(int64_t value) {
  if (value < 0) {
    print_u64(0 - (uint64_t)value, 1);
  } else {
    print_u64((uint64_t)value, 0);
  }
}

void tz_print_u64(uint64_t value) { print_u64(value, 0); }

void tz_print_bool(int32_t value) {
  if (value) {
    tz_out_write("true", 4);
  } else {
    tz_out_write("false", 5);
  }
}

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

void tz_print_f64(double value) {
  char buf[64];
  if (value == 0 && signbit(value)) {
    tz_out_write("-0", 2);
    return;
  }
  tz_out_write(buf, tz_format_f64(value, buf));
}

TZ_HOT double tz_math_round(double x) {
  double r = floor(x);
  if (x - r >= 0.5) {
    r += 1;
  }

  return r == 0 ? copysign(0.0, x) : r;
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

TZ_COLD TZ_NORETURN void tz_panic_int_does_not_fit(int64_t x, int32_t is_unsigned,
                                                   const char *type) {
  if (is_unsigned) {
    tz_panicf("%llu does not fit in %s", (unsigned long long)x, type);
  }
  tz_panicf("%lld does not fit in %s", (long long)x, type);
}

void tz_debugger(void) {
#ifdef _WIN32
  if (IsDebuggerPresent()) {
    __builtin_debugtrap();
  }
#endif
}

TZ_NORETURN void tz_panic(const char *message, int64_t len) {
  if (len < 0) {
    len = (int64_t)strlen(message);
  }
  tz_out_flush(1);
  stream_write(&tz_err_stream, "panic: ", 7);
  stream_write(&tz_err_stream, message, (size_t)len);
  stream_write(&tz_err_stream, "\n", 1);
  exit(2);
}

TZ_NORETURN void tz_panic_str(void *obj, uint64_t meta) {
  TzBuf units;
  tz_buf_init(&units);
  tz_buf_str(&units, (TzStr){obj, meta});
  char text[512];
  size_t n = 0;
  for (size_t i = 0; i < units.length && n + 1 < sizeof text; ++i) {
    text[n++] = units.data[i] < 0x80 ? (char)units.data[i] : '?';
  }
  tz_buf_free(&units);
  tz_panic(text, (int64_t)n);
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
