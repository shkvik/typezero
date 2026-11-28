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

#else

#endif

char tz_out_buf[8192];
size_t tz_out_len;
Stream *tz_out_target = &tz_out_stream;

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

