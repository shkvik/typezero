// The process and the clock: process.argv, process.env, process.exit and
// process.exitCode, process.cwd(), process.execPath and process.platform,
// Date.now() and performance.now(). Writing to stdout and stderr lives in
// output.c.
//
// On Windows everything goes through the UTF-16 ("W") functions of the
// system, so arguments, environment variables and paths in any language
// arrive whole; the code pages of the console play no part.

#include "runtime/library/process/process/process.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/group/group.h"
#include "runtime/core/map/map.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/publish/publish.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// _configure_wide_argv and __wargv: the C library splits the command line
// into arguments by the same rules as for wmain.
#include <corecrt_startup.h>
#else
#include <time.h>
#include <unistd.h>
#endif

// None of this is static: see the rules in tzrt.h.
int32_t tz_exit_code; // process.exitCode, or 0 if it is not set
int32_t tz_has_exit_code;
TzArray *tz_argv; // process.argv, made at its first use

//===----------------------------------------------------------------------===//
// Text from the system
//===----------------------------------------------------------------------===//

#ifndef _WIN32
// The arguments, which a constructor gets from the C library before main
// starts (glibc and the macOS loader pass them to constructors as to main).
int tz_posix_argc;
char **tz_posix_argv;

__attribute__((constructor)) static void keep_arguments(int argc, char **argv) {
  tz_posix_argc = argc;
  tz_posix_argv = argv;
}

// A string from UTF-8; bytes that do not form a character become U+FFFD.
static TzStr str_from_utf8(const char *text, size_t n) {
  TzBuf b;
  tz_buf_init(&b);
  for (size_t i = 0; i < n;) {
    unsigned char lead = (unsigned char)text[i];
    size_t extra = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
    int bad = (lead >= 0x80 && lead < 0xC2) || lead > 0xF4;
    uint32_t c = extra ? lead & (0x3Fu >> extra) : lead;
    size_t k = 1;
    while (k <= extra && i + k < n && ((unsigned char)text[i + k] & 0xC0) == 0x80) {
      c = (c << 6) | ((unsigned char)text[i + k++] & 0x3F);
    }
    if (bad || k <= extra || (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) {
      c = 0xFFFD;
    }
    i += k;
    if (c >= 0x10000) {
      c -= 0x10000;
      tz_buf_push(&b, (uint16_t)(0xD800 + (c >> 10)));
      tz_buf_push(&b, (uint16_t)(0xDC00 + (c & 0x3FF)));
    } else {
      tz_buf_push(&b, (uint16_t)c);
    }
  }

  return tz_buf_take(&b);
}

// The string as UTF-8 with a terminating zero, which the caller frees with
// tz_free. Lone surrogates become U+FFFD.
static char *utf8_of(TzStr s) {
  TzBuf b;
  tz_buf_init(&b);
  tz_buf_str(&b, s);
  char *out = (char *)tz_alloc(b.length * 3 + 1);
  size_t n = 0;
  for (size_t i = 0; i < b.length; ++i) {
    uint32_t c = b.data[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < b.length && b.data[i + 1] >= 0xDC00 &&
        b.data[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (b.data[++i] - 0xDC00);
    } else if (c >= 0xD800 && c <= 0xDFFF) {
      c = 0xFFFD;
    }
    if (c < 0x80) {
      out[n++] = (char)c;
    } else if (c < 0x800) {
      out[n++] = (char)(0xC0 | (c >> 6));
      out[n++] = (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out[n++] = (char)(0xE0 | (c >> 12));
      out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
      out[n++] = (char)(0x80 | (c & 0x3F));
    } else {
      out[n++] = (char)(0xF0 | (c >> 18));
      out[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
      out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
      out[n++] = (char)(0x80 | (c & 0x3F));
    }
  }
  out[n] = 0;
  tz_buf_free(&b);

  return out;
}
#endif

// The full path of the executable.
static TzStr exec_path(void) {
#ifdef _WIN32
  // GetModuleFileNameW fills the whole buffer when the path does not fit.
  DWORD capacity = MAX_PATH;
  for (;;) {
    wchar_t *path = (wchar_t *)tz_alloc(capacity * sizeof(wchar_t));
    DWORD length = GetModuleFileNameW(NULL, path, capacity);
    if (length < capacity) {
      TzStr s = tz_str_from_utf16((const uint16_t *)path, length);
      tz_free(path);
      return s;
    }
    tz_free(path);
    capacity *= 2;
  }
#else
  char path[4096];
  ssize_t length = readlink("/proc/self/exe", path, sizeof path);
  if (length > 0 && (size_t)length < sizeof path) {
    return str_from_utf8(path, (size_t)length);
  }
  const char *argv0 = tz_posix_argc > 0 ? tz_posix_argv[0] : "";
  return str_from_utf8(argv0, strlen(argv0));
#endif
}

//===----------------------------------------------------------------------===//
// process
//===----------------------------------------------------------------------===//

// process.argv, as in Node: the executable, then the executable again where
// Node has the path of the script, then the arguments. So argv.slice(2) is
// the arguments, as in any script for Node. One array, which the program may
// change, as in Node; the runtime keeps a reference to it.
// Made by the first thread that asks, under the lock, and shared from then
// on (publish.c): every thread sees the one array, and the one map.
TzLock tz_process_lock;

TzArray *tz_process_argv(const TzType *string_type) {
  tz_lock(&tz_process_lock);
  if (!tz_argv) {
#ifdef _WIN32
    _configure_wide_argv(_crt_argv_unexpanded_arguments);
    int argc = __argc;
    wchar_t **argv = __wargv;
#else
    int argc = tz_posix_argc;
    char **argv = tz_posix_argv;
#endif
    TzArray *a = tz_arr_new(string_type, 0, argc > 0 ? argc + 1 : 2);
    TzStr exe = exec_path();
    tz_str_retain_value(exe);
    tz_arr_push_value(a, &exe);
    tz_arr_push_value(a, &exe);
    for (int i = 1; i < argc && argv; ++i) {
#ifdef _WIN32
      TzStr arg = tz_str_from_utf16((const uint16_t *)argv[i], wcslen(argv[i]));
#else
      TzStr arg = str_from_utf8(argv[i], strlen(argv[i]));
#endif
      tz_arr_push_value(a, &arg);
    }
#if TZ_PARALLEL
    const TzType array_type = {TZ_T_ARRAY, sizeof(TzArray *), string_type};
    tz_publish_value(&array_type, &a);
#endif
    tz_argv = a;
  }
  tz_rc_inc(&tz_argv->rc);
  TzArray *argv = tz_argv;
  tz_unlock(&tz_process_lock);

  return argv;
}

// process.env[name]: whether the variable is set, and its value in `out`
// (the empty string when it is not). On Windows the names ignore case, as in
// Node there.
int32_t tz_process_env(TzStr *out, void *obj, uint64_t meta) {
  TzStr name = {obj, meta};
  out->obj = NULL;
  out->meta = 0;
  // A name with a zero character in it would name another variable.
  TzBuf b;
  tz_buf_init(&b);
  tz_buf_str(&b, name);
  int found = 1;
  for (size_t i = 0; i < b.length; ++i) {
    found &= b.data[i] != 0;
  }
#ifdef _WIN32
  tz_buf_push(&b, 0);
  if (found) {
    wchar_t small[256];
    wchar_t *value = small;
    DWORD capacity = 256, length;
    for (;;) {
      // An empty variable also gives 0, but without this error.
      SetLastError(ERROR_SUCCESS);
      length = GetEnvironmentVariableW((const wchar_t *)b.data, value, capacity);
      if (length < capacity) {
        break;
      }
      if (value != small) {
        tz_free(value);
      }
      capacity = length; // the size needed, with the terminating zero
      value = (wchar_t *)tz_alloc(capacity * sizeof(wchar_t));
    }
    found = length > 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
    if (found) {
      *out = tz_str_from_utf16((const uint16_t *)value, length);
    }
    if (value != small) {
      tz_free(value);
    }
  }
#else
  if (found) {
    char *key = utf8_of(name);
    const char *value = getenv(key);
    tz_free(key);
    found = value != NULL;
    if (found) {
      *out = str_from_utf8(value, strlen(value));
    }
  }
#endif
  tz_buf_free(&b);

  return found;
}

// process.env as a whole, a Record<string, string> made at its first use from
// the environment of the process; as in Node, one object, which the program
// may change. Setting a key sets the variable and deleting one removes it, so
// that programs started later see the change. On Windows the names ignore
// case, and the hidden variables of drives ("=C:") are left out, as in Node.
TzMap *tz_env_map;

TzMap *tz_process_env_map(const TzMapLayout *layout) {
  tz_lock(&tz_process_lock);
  if (!tz_env_map) {
    TzMap *m = tz_map_new(layout);
    int32_t added;
#ifdef _WIN32
    m->flags = TZ_MAP_FOLD;
    wchar_t *block = GetEnvironmentStringsW();
    for (const wchar_t *p = block; p && *p; p += wcslen(p) + 1) {
      const wchar_t *equals = wcschr(p + 1, L'=');
      if (*p == L'=' || !equals) {
        continue;
      }
      TzStr name = tz_str_from_utf16((const uint16_t *)p, (size_t)(equals - p));
      TzStr value = tz_str_from_utf16((const uint16_t *)(equals + 1), wcslen(equals + 1));
#else
    extern char **environ;
    for (char **p = environ; p && *p; ++p) {
      const char *equals = strchr(*p, '=');
      if (!equals) {
        continue;
      }
      TzStr name = str_from_utf8(*p, (size_t)(equals - *p));
      TzStr value = str_from_utf8(equals + 1, strlen(equals + 1));
#endif
      char *e = (char *)tz_map_add(m, &name, &added);
      TzStr *slot = (TzStr *)(e + layout->value_offset);
      tz_str_release_value(*slot); // a name given twice: the last value
      *slot = value;
      tz_str_release_value(name);
    }
#ifdef _WIN32
    FreeEnvironmentStringsW(block);
#endif
    m->flags |= TZ_MAP_ENV;
#if TZ_PARALLEL
    tz_publish_object(m);
#endif
    tz_env_map = m;
  }
  tz_rc_inc(&tz_env_map->rc);
  TzMap *env = tz_env_map;
  tz_unlock(&tz_process_lock);

  return env;
}

void tz_process_env_changed(const TzStr *name, const TzStr *value) {
#ifdef _WIN32
  TzBuf n, v;
  tz_buf_init(&n);
  tz_buf_init(&v);
  tz_buf_str(&n, *name);
  tz_buf_push(&n, 0);
  if (value) {
    tz_buf_str(&v, *value);
    tz_buf_push(&v, 0);
  }
  SetEnvironmentVariableW((const wchar_t *)n.data, value ? (const wchar_t *)v.data : NULL);
  tz_buf_free(&n);
  tz_buf_free(&v);
#else
  char *n = utf8_of(*name);
  if (value) {
    char *v = utf8_of(*value);
    setenv(n, v, 1);
    tz_free(v);
  } else {
    unsetenv(n);
  }
  tz_free(n);
#endif
}

// process.cwd(). As in Node, without a trailing separator, except for the
// root of a drive.
void tz_process_cwd(TzStr *out) {
#ifdef _WIN32
  DWORD capacity = GetCurrentDirectoryW(0, NULL);
  wchar_t *path = (wchar_t *)tz_alloc((capacity + 1) * sizeof(wchar_t));
  DWORD length = GetCurrentDirectoryW(capacity + 1, path);
  if (length > capacity) {
    length = 0; // changed in between: hardly possible in one thread
  }
  if (length > 3 && path[length - 1] == L'\\') {
    --length;
  }
  *out = tz_str_from_utf16((const uint16_t *)path, length);
  tz_free(path);
#else
  char path[4096];
  if (!getcwd(path, sizeof path)) {
    path[0] = 0;
  }
  *out = str_from_utf8(path, strlen(path));
#endif
}

void tz_process_exec_path(TzStr *out) { *out = exec_path(); }

void tz_process_platform(TzStr *out) {
#if defined(_WIN32)
  *out = tz_str_from_ascii("win32", 5);
#elif defined(__APPLE__)
  *out = tz_str_from_ascii("darwin", 6);
#else
  *out = tz_str_from_ascii("linux", 5);
#endif
}

// process.exit(code): the program ends at once, as in Node, with what is
// printed so far written out. Without a code it ends with process.exitCode.
TZ_NORETURN void tz_process_exit(int32_t code, int32_t has) { exit(has ? code : tz_exit_code); }

int32_t tz_process_get_exit_code(int32_t *has) {
  *has = tz_has_exit_code;

  return tz_exit_code;
}

void tz_process_set_exit_code(int32_t code, int32_t has) {
  tz_exit_code = has ? code : 0;
  tz_has_exit_code = has;
}

int32_t tz_process_exit_status(void) { return tz_exit_code; }

// Releases what the runtime holds, for the leak check at the end.
void tz_process_shutdown(void) {
  TzArray *a = tz_argv;
  tz_argv = NULL;
  if (a && tz_rc_dec(&a->rc)) {
    tz_arr_free(a);
  }
  TzMap *env = tz_env_map;
  tz_env_map = NULL;
  if (env && tz_rc_dec(&env->rc)) {
    tz_map_drop(env);
  }
}

//===----------------------------------------------------------------------===//
// The clock
//===----------------------------------------------------------------------===//

// performance.now() counts from the start of the program, as in Node.
#ifdef _WIN32
int64_t tz_clock_start, tz_clock_frequency;
#else
int64_t tz_clock_start; // in nanoseconds
#endif

void tz_clock_init(void) {
#ifdef _WIN32
  LARGE_INTEGER now, frequency;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&frequency);
  tz_clock_start = now.QuadPart;
  tz_clock_frequency = frequency.QuadPart;
#else
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  tz_clock_start = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
#endif
}

// Date.now(): whole milliseconds since 1970-01-01 UTC.
double tz_date_now(void) {
#ifdef _WIN32
  FILETIME time;
  GetSystemTimePreciseAsFileTime(&time);
  // 100-nanosecond intervals since 1601-01-01.
  uint64_t ticks = ((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime;

  return (double)((int64_t)(ticks - 116444736000000000ull) / 10000);
#else
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  return (double)((int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
#endif
}

// performance.now(): milliseconds, with a fraction, since the start.
double tz_performance_now(void) {
#ifdef _WIN32
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  int64_t ticks = now.QuadPart - tz_clock_start;

  // Whole seconds and the rest separately, so that nothing overflows.
  return (double)(ticks / tz_clock_frequency) * 1000.0 +
         (double)(ticks % tz_clock_frequency) * 1000.0 / (double)tz_clock_frequency;
#else
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (double)((int64_t)now.tv_sec * 1000000000 + now.tv_nsec - tz_clock_start) / 1e6;
#endif
}
