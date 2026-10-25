// node:fs (docs/io.md): the synchronous functions for files and
// directories, and the errors of the system they throw as Node does; those
// of node:fs/promises, which wait on the thread pool (io.c); stdin, for
// readLine of tz:io, readFileSync(0) and node:readline.
//
// An error of the system becomes a SystemError (an Error with code, errno,
// syscall, path and dest), which the program makes from TzSystemErrorParts
// with the function it passes in; then it is tz_thrown, as with tz_throw.
// The codes, numbers and texts are libuv's, which Node uses: on Windows
// `ENOENT` is -4058 and the error of the system is translated as libuv
// translates it, so the code is the one Node gives for the same failure.
//
// On Windows, as in Node, a path is resolved and given to the system in its
// `\\?\` form, which takes long paths; errors show it resolved, without the
// prefix. Everything goes through the UTF-16 ("W") functions, so names in
// any language work.

#ifdef _WIN32
#define _CRT_RAND_S // rand_s
#else
#define _DEFAULT_SOURCE // lstat, realpath, d_type and the like, which -std=c17 hides
#endif

#include "tzrt.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

//===----------------------------------------------------------------------===//
// Bytes
//===----------------------------------------------------------------------===//

typedef struct {
  uint8_t *data;
  size_t length, capacity;
} Bytes;

static void bytes_init(Bytes *b) {
  b->data = NULL;
  b->length = b->capacity = 0;
}

static void bytes_free(Bytes *b) {
  tz_free(b->data);
  bytes_init(b);
}

static void bytes_reserve(Bytes *b, size_t extra) {
  if (b->length + extra <= b->capacity)
    return;
  size_t capacity = b->capacity + b->capacity / 2 + 256;
  if (capacity < b->length + extra)
    capacity = b->length + extra;
  uint8_t *data = (uint8_t *)tz_alloc(capacity);
  if (b->length)
    memcpy(data, b->data, b->length);
  tz_free(b->data);
  b->data = data;
  b->capacity = capacity;
}

static void bytes_append(Bytes *b, const void *data, size_t n) {
  bytes_reserve(b, n);
  memcpy(b->data + b->length, data, n);
  b->length += n;
}

// Drops the first `n` bytes.
static void bytes_consume(Bytes *b, size_t n) {
  memmove(b->data, b->data + n, b->length - n);
  b->length -= n;
}

// UTF-16 as UTF-8 into `p`, which has room for 3 bytes a unit; a lone
// surrogate becomes U+FFFD, as Buffer.from does. Returns the end.
static uint8_t *utf8_encode(uint8_t *p, const uint16_t *s, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    uint32_t c = s[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF)
      c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
    else if (c >= 0xD800 && c <= 0xDFFF)
      c = 0xFFFD;
    if (c < 0x80) {
      *p++ = (uint8_t)c;
    } else if (c < 0x800) {
      *p++ = (uint8_t)(0xC0 | (c >> 6));
      *p++ = (uint8_t)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      *p++ = (uint8_t)(0xE0 | (c >> 12));
      *p++ = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
      *p++ = (uint8_t)(0x80 | (c & 0x3F));
    } else {
      *p++ = (uint8_t)(0xF0 | (c >> 18));
      *p++ = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
      *p++ = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
      *p++ = (uint8_t)(0x80 | (c & 0x3F));
    }
  }
  return p;
}

static void bytes_utf8(Bytes *out, const uint16_t *s, size_t n) {
  bytes_reserve(out, n * 3);
  out->length = (size_t)(utf8_encode(out->data + out->length, s, n) - out->data);
}

static void bytes_str(Bytes *out, TzStr s) {
  TzBuf units;
  tz_buf_init(&units);
  tz_buf_str(&units, s);
  bytes_utf8(out, units.data, units.length);
  tz_buf_free(&units);
}

// What the threads of the pool (io.c) write into: memory of malloc, which is
// thread-safe, as the allocator of the runtime is not. UTF-16 lies there as
// pairs of bytes.
typedef struct {
  uint8_t *data;
  size_t length, capacity;
} Raw;

static void raw_reserve(Raw *r, size_t extra) {
  if (r->length + extra <= r->capacity)
    return;
  size_t capacity = r->capacity + r->capacity / 2 + 256;
  if (capacity < r->length + extra)
    capacity = r->length + extra;
  uint8_t *data = (uint8_t *)realloc(r->data, capacity);
  if (!data)
    abort(); // on a thread of the pool, which cannot report it
  r->data = data;
  r->capacity = capacity;
}

static void raw_append(Raw *r, const void *data, size_t n) {
  raw_reserve(r, n);
  if (n)
    memcpy(r->data + r->length, data, n);
  r->length += n;
}

static void raw_free(Raw *r) {
  free(r->data);
  r->data = NULL;
  r->length = r->capacity = 0;
}

#ifdef _WIN32
// UTF-16 units of a Raw.
static void raw_units(Raw *r, const uint16_t *units, size_t n) { raw_append(r, units, n * 2); }
static void raw_unit(Raw *r, uint16_t unit) { raw_append(r, &unit, 2); }
static const uint16_t *raw_wide(const Raw *r) { return (const uint16_t *)r->data; }
static size_t raw_count(const Raw *r) { return r->length / 2; }
#endif

// UTF-8 as a string, as Node decodes it: each maximal part of a sequence
// that is not UTF-8 becomes one U+FFFD (the WHATWG decoder). A byte order
// mark stays, as in Node.
static TzStr str_of_utf8(const uint8_t *s, size_t n) {
  size_t ascii = 0;
  while (ascii < n && s[ascii] < 0x80)
    ++ascii;
  if (ascii == n)
    return tz_str_from_ascii((const char *)s, n);
  TzBuf b;
  tz_buf_init(&b);
  tz_buf_bytes(&b, s, ascii);
  for (size_t i = ascii; i < n;) {
    uint8_t lead = s[i];
    if (lead < 0x80) {
      tz_buf_push(&b, lead);
      ++i;
      continue;
    }
    uint32_t c;
    size_t need;
    uint8_t lower = 0x80, upper = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
      need = 1;
      c = lead & 0x1F;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
      need = 2;
      c = lead & 0x0F;
      if (lead == 0xE0)
        lower = 0xA0;
      if (lead == 0xED)
        upper = 0x9F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      need = 3;
      c = lead & 0x07;
      if (lead == 0xF0)
        lower = 0x90;
      if (lead == 0xF4)
        upper = 0x8F;
    } else {
      tz_buf_push(&b, 0xFFFD);
      ++i;
      continue;
    }
    size_t j = i + 1, k = 0;
    for (; k < need && j < n; ++k, ++j) {
      if (s[j] < lower || s[j] > upper)
        break;
      lower = 0x80;
      upper = 0xBF;
      c = (c << 6) | (s[j] & 0x3F);
    }
    i = j;
    if (k < need) {
      tz_buf_push(&b, 0xFFFD);
      continue;
    }
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

//===----------------------------------------------------------------------===//
// Errors of the system
//===----------------------------------------------------------------------===//

// libuv's errors that files can give: the code, its text, and the number
// Node gives on Windows (on other systems it is -errno).
typedef struct {
  const char *code;
  const char *message;
  int32_t windows;
} SysError;

enum {
  E_UNKNOWN,
  E_ENOENT,
  E_EEXIST,
  E_EACCES,
  E_EPERM,
  E_EISDIR,
  E_ENOTDIR,
  E_ENOTEMPTY,
  E_EBUSY,
  E_EINVAL,
  E_EXDEV,
  E_EMFILE,
  E_ENFILE,
  E_ENAMETOOLONG,
  E_ENOSPC,
  E_EBADF,
  E_ELOOP,
  E_EIO,
  E_EROFS,
  E_ENOMEM,
  E_EOF,
  E_EPIPE,
  E_ENOTSUP,
  E_ECANCELED,
  E_ETIMEDOUT,
  E_E2BIG,
  E_EFAULT,
  E_ENOSYS,
  E_ECHARSET,
  E_EAGAIN,
  E_EFBIG,
  E_ESPIPE,
  E_EMLINK,
  E_ETXTBSY,
  E_ENXIO,
  E_EINTR,
  E_ENODEV,
};

static const SysError kErrors[] = {
    {"UNKNOWN", "unknown error", -4094},
    {"ENOENT", "no such file or directory", -4058},
    {"EEXIST", "file already exists", -4075},
    {"EACCES", "permission denied", -4092},
    {"EPERM", "operation not permitted", -4048},
    {"EISDIR", "illegal operation on a directory", -4068},
    {"ENOTDIR", "not a directory", -4052},
    {"ENOTEMPTY", "directory not empty", -4051},
    {"EBUSY", "resource busy or locked", -4082},
    {"EINVAL", "invalid argument", -4071},
    {"EXDEV", "cross-device link not permitted", -4037},
    {"EMFILE", "too many open files", -4066},
    {"ENFILE", "file table overflow", -4061},
    {"ENAMETOOLONG", "name too long", -4064},
    {"ENOSPC", "no space left on device", -4055},
    {"EBADF", "bad file descriptor", -4083},
    {"ELOOP", "too many symbolic links encountered", -4067},
    {"EIO", "i/o error", -4070},
    {"EROFS", "read-only file system", -4043},
    {"ENOMEM", "not enough memory", -4057},
    {"EOF", "end of file", -4095},
    {"EPIPE", "broken pipe", -4047},
    {"ENOTSUP", "operation not supported on socket", -4049},
    {"ECANCELED", "operation canceled", -4081},
    {"ETIMEDOUT", "connection timed out", -4039},
    {"E2BIG", "argument list too long", -4093},
    {"EFAULT", "bad address in system call argument", -4074},
    {"ENOSYS", "function not implemented", -4054},
    {"ECHARSET", "invalid Unicode character", -4080},
    {"EAGAIN", "resource temporarily unavailable", -4088},
    {"EFBIG", "file too large", -4036},
    {"ESPIPE", "invalid seek", -4041},
    {"EMLINK", "too many links", -4032},
    {"ETXTBSY", "text file is busy", -4038},
    {"ENXIO", "no such device or address", -4033},
    {"EINTR", "interrupted system call", -4072},
    {"ENODEV", "no such device", -4059},
};

// A failure: which error, and the number the program sees.
typedef struct {
  int index;
  int32_t number;
} Failure;

#ifdef _WIN32
// An error of Windows as libuv translates it (uv_translate_sys_error).
static Failure win32_failure(DWORD error) {
  int index;
  switch (error) {
  case ERROR_NOACCESS:
  case ERROR_ELEVATION_REQUIRED:
  case ERROR_CANT_ACCESS_FILE:
    index = E_EACCES;
    break;
  case ERROR_INVALID_FLAGS:
  case ERROR_INVALID_HANDLE:
    index = E_EBADF;
    break;
  case ERROR_LOCK_VIOLATION:
  case ERROR_PIPE_BUSY:
  case ERROR_SHARING_VIOLATION:
    index = E_EBUSY;
    break;
  case ERROR_OPERATION_ABORTED:
    index = E_ECANCELED;
    break;
  case ERROR_NO_UNICODE_TRANSLATION:
    index = E_ECHARSET;
    break;
  case ERROR_ALREADY_EXISTS:
  case ERROR_FILE_EXISTS:
    index = E_EEXIST;
    break;
  case ERROR_BUFFER_OVERFLOW:
    index = E_EFAULT;
    break;
  case ERROR_INSUFFICIENT_BUFFER:
  case ERROR_INVALID_DATA:
  case ERROR_INVALID_PARAMETER:
  case ERROR_SYMLINK_NOT_SUPPORTED:
    index = E_EINVAL;
    break;
  case ERROR_BEGINNING_OF_MEDIA:
  case ERROR_BUS_RESET:
  case ERROR_CRC:
  case ERROR_DEVICE_DOOR_OPEN:
  case ERROR_DEVICE_REQUIRES_CLEANING:
  case ERROR_DISK_CORRUPT:
  case ERROR_EOM_OVERFLOW:
  case ERROR_FILEMARK_DETECTED:
  case ERROR_GEN_FAILURE:
  case ERROR_INVALID_BLOCK_LENGTH:
  case ERROR_IO_DEVICE:
  case ERROR_NO_DATA_DETECTED:
  case ERROR_NO_SIGNAL_SENT:
  case ERROR_OPEN_FAILED:
  case ERROR_SETMARK_DETECTED:
  case ERROR_SIGNAL_REFUSED:
    index = E_EIO;
    break;
  case ERROR_CANT_RESOLVE_FILENAME:
    index = E_ELOOP;
    break;
  case ERROR_TOO_MANY_OPEN_FILES:
    index = E_EMFILE;
    break;
  case ERROR_BAD_PATHNAME:
  case ERROR_DIRECTORY:
  case ERROR_ENVVAR_NOT_FOUND:
  case ERROR_FILE_NOT_FOUND:
  case ERROR_INVALID_NAME:
  case ERROR_INVALID_DRIVE:
  case ERROR_INVALID_REPARSE_DATA:
  case ERROR_MOD_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
    index = E_ENOENT;
    break;
  case ERROR_NOT_ENOUGH_MEMORY:
  case ERROR_OUTOFMEMORY:
    index = E_ENOMEM;
    break;
  case ERROR_CANNOT_MAKE:
  case ERROR_DISK_FULL:
  case ERROR_EA_TABLE_FULL:
  case ERROR_END_OF_MEDIA:
  case ERROR_HANDLE_DISK_FULL:
    index = E_ENOSPC;
    break;
  case ERROR_DIR_NOT_EMPTY:
    index = E_ENOTEMPTY;
    break;
  case ERROR_NOT_SUPPORTED:
    index = E_ENOTSUP;
    break;
  case ERROR_BROKEN_PIPE:
    index = E_EOF;
    break;
  case ERROR_ACCESS_DENIED:
  case ERROR_PRIVILEGE_NOT_HELD:
    index = E_EPERM;
    break;
  case ERROR_BAD_PIPE:
  case ERROR_NO_DATA:
  case ERROR_PIPE_NOT_CONNECTED:
    index = E_EPIPE;
    break;
  case ERROR_WRITE_PROTECT:
    index = E_EROFS;
    break;
  case ERROR_SEM_TIMEOUT:
    index = E_ETIMEDOUT;
    break;
  case ERROR_NOT_SAME_DEVICE:
    index = E_EXDEV;
    break;
  case ERROR_INVALID_FUNCTION:
    index = E_EISDIR;
    break;
  case ERROR_META_EXPANSION_TOO_LONG:
    index = E_E2BIG;
    break;
  case ERROR_FILENAME_EXCED_RANGE:
    index = E_ENAMETOOLONG;
    break;
  default:
    index = E_UNKNOWN;
    break;
  }
  return (Failure){index, kErrors[index].windows};
}
#else
// An errno, which Node gives negated.
static Failure errno_failure(int e) {
  int index;
  switch (e) {
  case ENOENT:
    index = E_ENOENT;
    break;
  case EEXIST:
    index = E_EEXIST;
    break;
  case EACCES:
    index = E_EACCES;
    break;
  case EPERM:
    index = E_EPERM;
    break;
  case EISDIR:
    index = E_EISDIR;
    break;
  case ENOTDIR:
    index = E_ENOTDIR;
    break;
  case ENOTEMPTY:
    index = E_ENOTEMPTY;
    break;
  case EBUSY:
    index = E_EBUSY;
    break;
  case EINVAL:
    index = E_EINVAL;
    break;
  case EXDEV:
    index = E_EXDEV;
    break;
  case EMFILE:
    index = E_EMFILE;
    break;
  case ENFILE:
    index = E_ENFILE;
    break;
  case ENAMETOOLONG:
    index = E_ENAMETOOLONG;
    break;
  case ENOSPC:
    index = E_ENOSPC;
    break;
  case EBADF:
    index = E_EBADF;
    break;
  case ELOOP:
    index = E_ELOOP;
    break;
  case EIO:
    index = E_EIO;
    break;
  case EROFS:
    index = E_EROFS;
    break;
  case ENOMEM:
    index = E_ENOMEM;
    break;
  case EPIPE:
    index = E_EPIPE;
    break;
  case ENOTSUP:
    index = E_ENOTSUP;
    break;
  case ECANCELED:
    index = E_ECANCELED;
    break;
  case ETIMEDOUT:
    index = E_ETIMEDOUT;
    break;
  case E2BIG:
    index = E_E2BIG;
    break;
  case EFAULT:
    index = E_EFAULT;
    break;
  case ENOSYS:
    index = E_ENOSYS;
    break;
  case EAGAIN:
    index = E_EAGAIN;
    break;
  case EFBIG:
    index = E_EFBIG;
    break;
  case ESPIPE:
    index = E_ESPIPE;
    break;
  case EMLINK:
    index = E_EMLINK;
    break;
  case ETXTBSY:
    index = E_ETXTBSY;
    break;
  case ENXIO:
    index = E_ENXIO;
    break;
  case EINTR:
    index = E_EINTR;
    break;
  case ENODEV:
    index = E_ENODEV;
    break;
  default:
    index = E_UNKNOWN;
    break;
  }
  return (Failure){index, -e};
}
#endif

static TzOptStr opt_of(const TzBuf *text) {
  TzOptStr o;
  memset(&o, 0, sizeof o);
  if (text) {
    o.value = tz_str_from_utf16(text->data, text->length);
    o.has = 1;
  }
  return o;
}

static TzStr ascii(const char *text) { return tz_str_from_ascii(text, strlen(text)); }

static void throw_parts(TzMaker make, TzSystemErrorParts *parts) {
  void *error = make(parts);
  tz_thrown = error;
}

// "ENOENT: no such file or directory, open 'C:\x.txt'", with the path and
// the second path (of rename and copyFile) as errors show them.
static void throw_failure(TzMaker make, Failure f, const char *syscall, const TzBuf *path,
                          const TzBuf *dest) {
  const SysError *e = &kErrors[f.index];
  TzBuf m;
  tz_buf_init(&m);
  tz_buf_ascii(&m, e->code);
  tz_buf_ascii(&m, ": ");
  tz_buf_ascii(&m, e->message);
  tz_buf_ascii(&m, ", ");
  tz_buf_ascii(&m, syscall);
  if (path) {
    tz_buf_ascii(&m, " '");
    tz_buf_units(&m, path->data, path->length);
    tz_buf_push(&m, '\'');
  }
  if (dest) {
    tz_buf_ascii(&m, " -> '");
    tz_buf_units(&m, dest->data, dest->length);
    tz_buf_push(&m, '\'');
  }
  TzSystemErrorParts parts;
  memset(&parts, 0, sizeof parts);
  parts.message = tz_buf_take(&m);
  parts.name = ascii("Error");
  parts.errno_value = f.number;
  parts.code = ascii(e->code);
  parts.syscall = ascii(syscall);
  parts.path = opt_of(path);
  parts.dest = opt_of(dest);
  throw_parts(make, &parts);
}

//===----------------------------------------------------------------------===//
// Paths
//===----------------------------------------------------------------------===//

// A path the program gave: as it wrote it, as the system gets it, and as
// errors show it.
typedef struct {
  TzBuf given;
  TzBuf shown;
#ifdef _WIN32
  TzBuf wide; // the `\\?\` form, ending with a zero
#else
  Bytes utf8; // ending with a zero
#endif
} Path;

static void path_from_units(Path *p) {
  tz_buf_init(&p->shown);
#ifdef _WIN32
  tz_buf_init(&p->wide);
  tz_path_namespaced_units(&p->wide, p->given.data, p->given.length);
  // Node shows the path without the prefix (StringFromPath).
  const uint16_t *w = p->wide.data;
  size_t n = p->wide.length;
  static const uint16_t unc[] = {'\\', '\\', '?', '\\', 'U', 'N', 'C', '\\'};
  if (n >= 8 && memcmp(w, unc, sizeof unc) == 0) {
    tz_buf_ascii(&p->shown, "\\\\");
    tz_buf_units(&p->shown, w + 8, n - 8);
  } else if (n >= 4 && w[0] == '\\' && w[1] == '\\' && w[2] == '?' && w[3] == '\\') {
    tz_buf_units(&p->shown, w + 4, n - 4);
  } else {
    tz_buf_units(&p->shown, w, n);
  }
  tz_buf_push(&p->wide, 0);
#else
  tz_buf_units(&p->shown, p->given.data, p->given.length);
  bytes_init(&p->utf8);
  bytes_utf8(&p->utf8, p->given.data, p->given.length);
  bytes_append(&p->utf8, "", 1);
#endif
}

static void path_free(Path *p) {
  tz_buf_free(&p->given);
  tz_buf_free(&p->shown);
#ifdef _WIN32
  tz_buf_free(&p->wide);
#else
  bytes_free(&p->utf8);
#endif
}

// A path of the program; false, with a TypeError thrown, if it holds a zero
// character, which would name another file.
static int path_init(Path *p, void *obj, uint64_t meta) {
  tz_buf_init(&p->given);
  tz_buf_str(&p->given, (TzStr){obj, meta});
  for (size_t i = 0; i < p->given.length; ++i) {
    if (p->given.data[i] != 0)
      continue;
    TzBuf m;
    tz_buf_init(&m);
    tz_buf_ascii(&m, "The argument 'path' must be a string, Uint8Array, or URL without null bytes. "
                     "Received '");
    for (size_t k = 0; k < p->given.length; ++k)
      if (p->given.data[k] == 0)
        tz_buf_ascii(&m, "\\x00");
      else
        tz_buf_push(&m, p->given.data[k]);
    tz_buf_push(&m, '\'');
    TzStr message = tz_buf_take(&m);
    tz_throw_str(TZ_TYPE_ERROR, message.obj, message.meta);
    tz_str_release_value(message);
    tz_buf_free(&p->given);
    return 0;
  }
  path_from_units(p);
  return 1;
}

// A path made by the runtime, in the form the program sees.
static void path_of_units(Path *p, const uint16_t *units, size_t n) {
  tz_buf_init(&p->given);
  tz_buf_units(&p->given, units, n);
  path_from_units(p);
}

#ifdef _WIN32
static const wchar_t *wide(const Path *p) { return (const wchar_t *)p->wide.data; }

static void fail_win32(TzMaker make, DWORD error, const char *syscall, const Path *path,
                       const Path *dest) {
  throw_failure(make, win32_failure(error), syscall, path ? &path->shown : NULL,
                dest ? &dest->shown : NULL);
}

static void fail_index(TzMaker make, int index, const char *syscall, const Path *path) {
  throw_failure(make, (Failure){index, kErrors[index].windows}, syscall, path ? &path->shown : NULL,
                NULL);
}
#else
static const char *native(const Path *p) { return (const char *)p->utf8.data; }

static void fail_errno(TzMaker make, int e, const char *syscall, const Path *path,
                       const Path *dest) {
  throw_failure(make, errno_failure(e), syscall, path ? &path->shown : NULL,
                dest ? &dest->shown : NULL);
}
#endif

//===----------------------------------------------------------------------===//
// Flags of open
//===----------------------------------------------------------------------===//

typedef struct {
  int read, write, append, create, truncate, exclusive;
} OpenFlags;

// The `flag` option, as Node reads it ("r", "w", "a+", "wx"...); 0 if it is
// none of them.
static int parse_flags(const TzBuf *flag, OpenFlags *f) {
  static const struct {
    const char *text;
    OpenFlags flags;
  } table[] = {
      {"r", {1, 0, 0, 0, 0, 0}},   {"rs", {1, 0, 0, 0, 0, 0}},  {"sr", {1, 0, 0, 0, 0, 0}},
      {"r+", {1, 1, 0, 0, 0, 0}},  {"rs+", {1, 1, 0, 0, 0, 0}}, {"sr+", {1, 1, 0, 0, 0, 0}},
      {"w", {0, 1, 0, 1, 1, 0}},   {"wx", {0, 1, 0, 1, 1, 1}},  {"xw", {0, 1, 0, 1, 1, 1}},
      {"w+", {1, 1, 0, 1, 1, 0}},  {"wx+", {1, 1, 0, 1, 1, 1}}, {"xw+", {1, 1, 0, 1, 1, 1}},
      {"a", {0, 1, 1, 1, 0, 0}},   {"ax", {0, 1, 1, 1, 0, 1}},  {"xa", {0, 1, 1, 1, 0, 1}},
      {"as", {0, 1, 1, 1, 0, 0}},  {"sa", {0, 1, 1, 1, 0, 0}},  {"a+", {1, 1, 1, 1, 0, 0}},
      {"ax+", {1, 1, 1, 1, 0, 1}}, {"xa+", {1, 1, 1, 1, 0, 1}}, {"as+", {1, 1, 1, 1, 0, 0}},
      {"sa+", {1, 1, 1, 1, 0, 0}},
  };
  for (size_t k = 0; k < sizeof table / sizeof *table; ++k) {
    size_t n = strlen(table[k].text), i = 0;
    if (n != flag->length)
      continue;
    while (i < n && flag->data[i] == (uint8_t)table[k].text[i])
      ++i;
    if (i == n) {
      *f = table[k].flags;
      return 1;
    }
  }
  return 0;
}

// The flags of `flag`, or a TypeError as Node throws it.
static int flags_of(void *obj, uint64_t meta, OpenFlags *f) {
  TzBuf flag;
  tz_buf_init(&flag);
  tz_buf_str(&flag, (TzStr){obj, meta});
  int ok = parse_flags(&flag, f);
  if (!ok) {
    TzBuf m;
    tz_buf_init(&m);
    tz_buf_ascii(&m, "The argument 'flags' is invalid. Received '");
    tz_buf_units(&m, flag.data, flag.length);
    tz_buf_push(&m, '\'');
    TzStr message = tz_buf_take(&m);
    tz_throw_str(TZ_TYPE_ERROR, message.obj, message.meta);
    tz_str_release_value(message);
  }
  tz_buf_free(&flag);
  return ok;
}

//===----------------------------------------------------------------------===//
// Files
//===----------------------------------------------------------------------===//

#ifdef _WIN32
typedef HANDLE File;
#define NO_FILE INVALID_HANDLE_VALUE

// As libuv opens files: a directory opens too (and then fails to read), and
// a file created without write permission in `mode` is read-only. NO_FILE
// with the failure of "open"; the system calls only, for the pool too.
static File open_raw(const Path *p, OpenFlags f, int32_t mode, Failure *failure) {
  DWORD access = f.write ? (f.read ? FILE_GENERIC_READ | FILE_GENERIC_WRITE : FILE_GENERIC_WRITE)
                         : FILE_GENERIC_READ;
  if (f.append) {
    access &= ~(DWORD)FILE_WRITE_DATA;
    access |= FILE_APPEND_DATA;
  }
  DWORD disposition = f.create && f.exclusive  ? CREATE_NEW
                      : f.create && f.truncate ? CREATE_ALWAYS
                      : f.create               ? OPEN_ALWAYS
                      : f.truncate             ? TRUNCATE_EXISTING
                                               : OPEN_EXISTING;
  DWORD attributes = FILE_ATTRIBUTE_NORMAL;
  if (f.create && !(mode & 0200))
    attributes |= FILE_ATTRIBUTE_READONLY;
  HANDLE h = CreateFileW(wide(p), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, disposition, attributes | FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    // To create a file where a directory is.
    if (error == ERROR_FILE_EXISTS && f.create && !f.exclusive)
      *failure = (Failure){E_EISDIR, kErrors[E_EISDIR].windows};
    else
      *failure = win32_failure(error);
  }
  return h;
}

static void close_file(File f) { CloseHandle(f); }

// Reads to the end; false with the error thrown ("read", without a path).
static int read_all(File f, Bytes *out, TzMaker make) {
  LARGE_INTEGER size;
  if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < ((int64_t)1 << 31))
    bytes_reserve(out, (size_t)size.QuadPart + 1);
  for (;;) {
    bytes_reserve(out, 65536);
    DWORD got = 0;
    if (!ReadFile(f, out->data + out->length, 65536, &got, NULL)) {
      DWORD error = GetLastError();
      if (error == ERROR_HANDLE_EOF || error == ERROR_BROKEN_PIPE)
        return 1;
      fail_win32(make, error, "read", NULL, NULL);
      return 0;
    }
    if (got == 0)
      return 1;
    out->length += got;
  }
}

static int write_all(File f, const uint8_t *data, size_t n, TzMaker make) {
  while (n > 0) {
    DWORD chunk = n > 0x40000000 ? 0x40000000 : (DWORD)n, written = 0;
    if (!WriteFile(f, data, chunk, &written, NULL)) {
      fail_win32(make, GetLastError(), "write", NULL, NULL);
      return 0;
    }
    data += written;
    n -= written;
  }
  return 1;
}

static File fd_file(int32_t fd) {
  if (fd == 0)
    return GetStdHandle(STD_INPUT_HANDLE);
  intptr_t h = _get_osfhandle(fd);
  return h == -1 || h == -2 ? NO_FILE : (HANDLE)h;
}

static void fail_bad_fd(TzMaker make, const char *syscall) {
  fail_index(make, E_EBADF, syscall, NULL);
}
#else
typedef int File;
#define NO_FILE (-1)

static File open_raw(const Path *p, OpenFlags f, int32_t mode, Failure *failure) {
  int flags = f.read && f.write ? O_RDWR : f.write ? O_WRONLY : O_RDONLY;
  if (f.append)
    flags |= O_APPEND;
  if (f.create)
    flags |= O_CREAT;
  if (f.truncate)
    flags |= O_TRUNC;
  if (f.exclusive)
    flags |= O_EXCL;
  int fd = open(native(p), flags | O_CLOEXEC, mode);
  if (fd < 0)
    *failure = errno_failure(errno);
  return fd;
}

static void close_file(File f) { close(f); }

static int read_all(File f, Bytes *out, TzMaker make) {
  for (;;) {
    bytes_reserve(out, 65536);
    ssize_t got = read(f, out->data + out->length, 65536);
    if (got < 0) {
      if (errno == EINTR)
        continue;
      fail_errno(make, errno, "read", NULL, NULL);
      return 0;
    }
    if (got == 0)
      return 1;
    out->length += (size_t)got;
  }
}

static int write_all(File f, const uint8_t *data, size_t n, TzMaker make) {
  while (n > 0) {
    ssize_t written = write(f, data, n);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      fail_errno(make, errno, "write", NULL, NULL);
      return 0;
    }
    data += written;
    n -= (size_t)written;
  }
  return 1;
}

static File fd_file(int32_t fd) { return fd; }

static void fail_bad_fd(TzMaker make, const char *syscall) {
  throw_failure(make, errno_failure(EBADF), syscall, NULL, NULL);
}
#endif

static File open_file(const Path *p, OpenFlags f, int32_t mode, TzMaker make) {
  Failure failure;
  File file = open_raw(p, f, mode, &failure);
  if (file == NO_FILE)
    throw_failure(make, failure, "open", &p->shown, NULL);
  return file;
}

//===----------------------------------------------------------------------===//
// stdin
//===----------------------------------------------------------------------===//

// What stdin gave that nobody took yet, as UTF-8, and whether it ended.
// Not static: see the rules in tzrt.h.
Bytes tz_in;
int tz_in_eof;
#ifdef _WIN32
uint16_t tz_in_surrogate; // the first half of a pair the console split
#endif

// Reads more of stdin; false at its end.
static int in_fill(void) {
  if (tz_in_eof)
    return 0;
#ifdef _WIN32
  HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode;
  if (h == NULL || h == INVALID_HANDLE_VALUE) {
    tz_in_eof = 1;
    return 0;
  }
  if (GetConsoleMode(h, &mode)) {
    // The console gives UTF-16: letters of any language, whatever its code
    // page. A line that starts with Ctrl+Z ends the input.
    wchar_t text[4096];
    DWORD got = 0;
    if (!ReadConsoleW(h, text, 4096, &got, NULL) || got == 0 || text[0] == 0x1A) {
      tz_in_eof = 1;
      return 0;
    }
    uint16_t units[4097];
    size_t n = 0;
    if (tz_in_surrogate)
      units[n++] = tz_in_surrogate;
    tz_in_surrogate = 0;
    for (DWORD i = 0; i < got; ++i)
      units[n++] = text[i];
    if (n > 0 && units[n - 1] >= 0xD800 && units[n - 1] <= 0xDBFF)
      tz_in_surrogate = units[--n];
    bytes_utf8(&tz_in, units, n);
    return 1;
  }
  bytes_reserve(&tz_in, 65536);
  DWORD got = 0;
  if (!ReadFile(h, tz_in.data + tz_in.length, 65536, &got, NULL) || got == 0) {
    tz_in_eof = 1;
    return 0;
  }
  tz_in.length += got;
  return 1;
#else
  bytes_reserve(&tz_in, 65536);
  for (;;) {
    ssize_t got = read(0, tz_in.data + tz_in.length, 65536);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0) {
      tz_in_eof = 1;
      return 0;
    }
    tz_in.length += (size_t)got;
    return 1;
  }
#endif
}

// readLine(question?) of tz:io: the question as it is, then a line of stdin
// without its "\n" or "\r\n". At the end of the input, false.
int32_t tz_io_read_line(TzStr *out, void *question_obj, uint64_t question_meta,
                        int32_t has_question) {
  if (has_question)
    tz_stdout_write(question_obj, question_meta);
  tz_out_flush(1);
  size_t scanned = 0;
  for (;;) {
    for (size_t i = scanned; i < tz_in.length; ++i) {
      if (tz_in.data[i] != '\n')
        continue;
      size_t end = i > 0 && tz_in.data[i - 1] == '\r' ? i - 1 : i;
      *out = str_of_utf8(tz_in.data, end);
      bytes_consume(&tz_in, i + 1);
      return 1;
    }
    scanned = tz_in.length;
    if (!in_fill()) {
      if (tz_in.length == 0) {
        out->obj = NULL;
        out->meta = 0;
        return 0;
      }
      *out = str_of_utf8(tz_in.data, tz_in.length);
      tz_in.length = 0;
      return 1;
    }
  }
}

// Everything left in stdin, into `data`.
static void read_stdin(Bytes *data) {
  tz_out_flush(1);
  while (in_fill()) {
  }
  bytes_append(data, tz_in.data, tz_in.length);
  tz_in.length = 0;
}

//===----------------------------------------------------------------------===//
// readFileSync, writeFileSync, appendFileSync
//===----------------------------------------------------------------------===//

// The bytes of a file (or of stdin, fd 0, whatever it was read with before),
// into `data`; 0 after the error is thrown.
static int read_path(Bytes *data, void *path_obj, uint64_t path_meta, void *flag_obj,
                     uint64_t flag_meta, TzMaker make) {
  OpenFlags flags;
  if (!flags_of(flag_obj, flag_meta, &flags))
    return 0;
  Path p;
  if (!path_init(&p, path_obj, path_meta))
    return 0;
  int ok = 0;
  File f = open_file(&p, flags, 0666, make);
  if (f != NO_FILE) {
    ok = read_all(f, data, make);
    close_file(f);
  }
  path_free(&p);
  return ok;
}

static int read_fd(Bytes *data, int32_t fd, TzMaker make) {
  if (fd == 0) {
    read_stdin(data);
    return 1;
  }
  File f = fd_file(fd);
  if (f == NO_FILE || fd < 0) {
    fail_bad_fd(make, "read");
    return 0;
  }
  return read_all(f, data, make);
}

// The text of what was read: utf8 as it is, any other encoding of Buffer
// (numbered as in buffer.c) decoded.
static TzStr text_of(const Bytes *data, int32_t encoding) {
  if (encoding == 0)
    return str_of_utf8(data->data, data->length);
  return tz_buffer_decode(data->data, (int64_t)data->length, encoding);
}

void tz_fs_read_file(TzStr *out, void *path_obj, uint64_t path_meta, void *flag_obj,
                     uint64_t flag_meta, int32_t encoding, TzMaker make) {
  out->obj = NULL;
  out->meta = 0;
  Bytes data;
  bytes_init(&data);
  if (read_path(&data, path_obj, path_meta, flag_obj, flag_meta, make))
    *out = text_of(&data, encoding);
  bytes_free(&data);
}

void tz_fs_read_fd(TzStr *out, int32_t fd, int32_t encoding, TzMaker make) {
  out->obj = NULL;
  out->meta = 0;
  Bytes data;
  bytes_init(&data);
  if (read_fd(&data, fd, make))
    *out = text_of(&data, encoding);
  bytes_free(&data);
}

// readFileSync without an encoding: a Buffer of the bytes, or NULL after the
// error is thrown.
TzBytes *tz_fs_read_file_bytes(void *path_obj, uint64_t path_meta, void *flag_obj,
                               uint64_t flag_meta, TzMaker make) {
  Bytes data;
  bytes_init(&data);
  TzBytes *b = NULL;
  if (read_path(&data, path_obj, path_meta, flag_obj, flag_meta, make))
    b = tz_buffer_copy_of(data.data, (int64_t)data.length);
  bytes_free(&data);
  return b;
}

TzBytes *tz_fs_read_fd_bytes(int32_t fd, TzMaker make) {
  Bytes data;
  bytes_init(&data);
  TzBytes *b = NULL;
  if (read_fd(&data, fd, make))
    b = tz_buffer_copy_of(data.data, (int64_t)data.length);
  bytes_free(&data);
  return b;
}

// Writes `data` to the file at the path, opened with the flag and the mode.
static void write_path(void *path_obj, uint64_t path_meta, const uint8_t *data, size_t length,
                       void *flag_obj, uint64_t flag_meta, int32_t mode, TzMaker make) {
  OpenFlags flags;
  if (!flags_of(flag_obj, flag_meta, &flags))
    return;
  Path p;
  if (!path_init(&p, path_obj, path_meta))
    return;
  File f = open_file(&p, flags, mode, make);
  if (f != NO_FILE) {
    write_all(f, data, length, make);
    close_file(f);
  }
  path_free(&p);
}

// writeFileSync(fd, data): stdout and stderr through the output buffer, in
// order with console.log.
static void write_fd(int32_t fd, const uint8_t *data, size_t length, TzMaker make) {
  if (fd == 1 || fd == 2) {
    tz_std_write_bytes(fd, data, length);
    return;
  }
  File f = fd < 0 ? NO_FILE : fd_file(fd);
  if (f == NO_FILE) {
    fail_bad_fd(make, "write");
    return;
  }
  write_all(f, data, length, make);
}

void tz_fs_write_file(void *path_obj, uint64_t path_meta, void *data_obj, uint64_t data_meta,
                      void *flag_obj, uint64_t flag_meta, int32_t mode, int32_t encoding,
                      TzMaker make) {
  if (encoding != 0) {
    TzBytes *b = tz_buffer_from_string(data_obj, data_meta, encoding);
    write_path(path_obj, path_meta, b->data, (size_t)b->length, flag_obj, flag_meta, mode, make);
    tz_instance_release((TzInstance *)b);
    return;
  }
  Bytes data;
  bytes_init(&data);
  bytes_str(&data, (TzStr){data_obj, data_meta});
  write_path(path_obj, path_meta, data.data, data.length, flag_obj, flag_meta, mode, make);
  bytes_free(&data);
}

void tz_fs_write_fd(int32_t fd, void *data_obj, uint64_t data_meta, int32_t encoding,
                    TzMaker make) {
  if (encoding != 0) {
    TzBytes *b = tz_buffer_from_string(data_obj, data_meta, encoding);
    write_fd(fd, b->data, (size_t)b->length, make);
    tz_instance_release((TzInstance *)b);
    return;
  }
  if (fd == 1) {
    tz_stdout_write(data_obj, data_meta);
    return;
  }
  if (fd == 2) {
    tz_stderr_write(data_obj, data_meta);
    return;
  }
  Bytes data;
  bytes_init(&data);
  bytes_str(&data, (TzStr){data_obj, data_meta});
  write_fd(fd, data.data, data.length, make);
  bytes_free(&data);
}

// writeFileSync and appendFileSync of a typed array, a Buffer or a DataView:
// its bytes.
void tz_fs_write_file_bytes(void *path_obj, uint64_t path_meta, const TzBytes *b,
                            void *flag_obj, uint64_t flag_meta, int32_t mode, TzMaker make) {
  write_path(path_obj, path_meta, b->data, (size_t)tz_bytes_byte_length(b), flag_obj, flag_meta,
             mode, make);
}

void tz_fs_write_fd_bytes(int32_t fd, const TzBytes *b, TzMaker make) {
  write_fd(fd, b->data, (size_t)tz_bytes_byte_length(b), make);
}

//===----------------------------------------------------------------------===//
// existsSync, accessSync, chmodSync, truncateSync
//===----------------------------------------------------------------------===//

int32_t tz_fs_exists(void *path_obj, uint64_t path_meta) {
  TzBuf given;
  tz_buf_init(&given);
  tz_buf_str(&given, (TzStr){path_obj, path_meta});
  int ok = given.length > 0;
  for (size_t i = 0; i < given.length && ok; ++i)
    ok = given.data[i] != 0;
  if (ok) {
    Path p;
    path_of_units(&p, given.data, given.length);
#ifdef _WIN32
    ok = GetFileAttributesW(wide(&p)) != INVALID_FILE_ATTRIBUTES;
#else
    ok = access(native(&p), F_OK) == 0;
#endif
    path_free(&p);
  }
  tz_buf_free(&given);
  return ok;
}

void tz_fs_access(void *path_obj, uint64_t path_meta, int32_t mode, TzMaker make) {
  Path p;
  if (!path_init(&p, path_obj, path_meta))
    return;
#ifdef _WIN32
  // As libuv: only writing can be denied, to a read-only file.
  DWORD attributes = GetFileAttributesW(wide(&p));
  if (attributes == INVALID_FILE_ATTRIBUTES)
    fail_win32(make, GetLastError(), "access", &p, NULL);
  else if ((mode & 2) && (attributes & FILE_ATTRIBUTE_READONLY) &&
           !(attributes & FILE_ATTRIBUTE_DIRECTORY))
    fail_index(make, E_EPERM, "access", &p);
#else
  if (access(native(&p), mode) != 0)
    fail_errno(make, errno, "access", &p, NULL);
#endif
  path_free(&p);
}

void tz_fs_chmod(void *path_obj, uint64_t path_meta, int32_t mode, TzMaker make) {
  Path p;
  if (!path_init(&p, path_obj, path_meta))
    return;
#ifdef _WIN32
  // Windows keeps only whether a file is read-only: the owner's write bit.
  DWORD attributes = GetFileAttributesW(wide(&p));
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    fail_win32(make, GetLastError(), "chmod", &p, NULL);
  } else {
    DWORD changed = mode & 0200 ? attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY
                                : attributes | FILE_ATTRIBUTE_READONLY;
    if (changed != attributes && !SetFileAttributesW(wide(&p), changed))
      fail_win32(make, GetLastError(), "chmod", &p, NULL);
  }
#else
  if (chmod(native(&p), (mode_t)mode) != 0)
    fail_errno(make, errno, "chmod", &p, NULL);
#endif
  path_free(&p);
}

void tz_fs_truncate(void *path_obj, uint64_t path_meta, int64_t length, TzMaker make) {
  Path p;
  if (!path_init(&p, path_obj, path_meta))
    return;
  OpenFlags flags = {1, 1, 0, 0, 0, 0}; // "r+"
  File f = open_file(&p, flags, 0666, make);
  if (f != NO_FILE) {
#ifdef _WIN32
    LARGE_INTEGER at;
    at.QuadPart = length < 0 ? 0 : length;
    if (!SetFilePointerEx(f, at, NULL, FILE_BEGIN) || !SetEndOfFile(f))
      fail_win32(make, GetLastError(), "ftruncate", NULL, NULL);
#else
    if (ftruncate(f, length < 0 ? 0 : length) != 0)
      fail_errno(make, errno, "ftruncate", NULL, NULL);
#endif
    close_file(f);
  }
  path_free(&p);
}

//===----------------------------------------------------------------------===//
// statSync and lstatSync
//===----------------------------------------------------------------------===//

enum {
  S_KIND = 0170000,
  S_REG = 0100000,
  S_DIR = 0040000,
  S_LNK = 0120000,
  S_CHR = 0020000,
  S_BLK = 0060000,
  S_FIFO = 0010000,
  S_SOCK = 0140000,
};

#ifdef _WIN32
// Milliseconds since 1970 from 100-nanosecond intervals since 1601, as Node
// computes them from libuv's seconds and nanoseconds.
static double ms_of(LARGE_INTEGER time) {
  int64_t ticks = time.QuadPart - 116444736000000000LL;
  int64_t seconds = ticks / 10000000, rest = ticks % 10000000;
  if (rest < 0) {
    rest += 10000000;
    --seconds;
  }
  return (double)seconds * 1000 + (double)(rest * 100) / 1e6;
}

static int is_link_tag(HANDLE h) {
  FILE_ATTRIBUTE_TAG_INFO tag;
  if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof tag))
    return 0;
  return tag.ReparseTag == IO_REPARSE_TAG_SYMLINK || tag.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT;
}

// The fields of Stats as libuv fills them on Windows; 0 or the error.
static DWORD stat_of(const wchar_t *path, int follow, TzStatsParts *st) {
  HANDLE h =
      CreateFileW(path, FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                  FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT), NULL);
  if (h == INVALID_HANDLE_VALUE)
    return GetLastError();
  BY_HANDLE_FILE_INFORMATION info;
  FILE_BASIC_INFO basic;
  FILE_STANDARD_INFO standard;
  if (!GetFileInformationByHandle(h, &info) ||
      !GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof basic) ||
      !GetFileInformationByHandleEx(h, FileStandardInfo, &standard, sizeof standard)) {
    DWORD error = GetLastError();
    CloseHandle(h);
    return error;
  }
  int link = !follow && (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && is_link_tag(h);
  CloseHandle(h);
  uint32_t mode = info.dwFileAttributes & FILE_ATTRIBUTE_READONLY ? 0444 : 0666;
  mode |= link ? S_LNK : info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? S_DIR : S_REG;
  st->dev = (double)info.dwVolumeSerialNumber;
  st->mode = (double)mode;
  st->nlink = (double)standard.NumberOfLinks;
  st->uid = st->gid = st->rdev = 0;
  st->blksize = 4096;
  st->ino = (double)(((uint64_t)info.nFileIndexHigh << 32) | info.nFileIndexLow);
  st->size = (double)standard.EndOfFile.QuadPart;
  st->blocks = (double)((uint64_t)standard.AllocationSize.QuadPart >> 9);
  st->atime_ms = ms_of(basic.LastAccessTime);
  st->mtime_ms = ms_of(basic.LastWriteTime);
  st->ctime_ms = ms_of(basic.ChangeTime);
  st->birthtime_ms = ms_of(basic.CreationTime);
  return 0;
}
#else
static double ms_of(struct timespec t) { return (double)t.tv_sec * 1000 + (double)t.tv_nsec / 1e6; }

static int stat_of(const char *path, int follow, TzStatsParts *st) {
  struct stat s;
  if ((follow ? stat(path, &s) : lstat(path, &s)) != 0)
    return errno;
  st->dev = (double)s.st_dev;
  st->mode = (double)s.st_mode;
  st->nlink = (double)s.st_nlink;
  st->uid = (double)s.st_uid;
  st->gid = (double)s.st_gid;
  st->rdev = (double)s.st_rdev;
  st->blksize = (double)s.st_blksize;
  st->ino = (double)s.st_ino;
  st->size = (double)s.st_size;
  st->blocks = (double)s.st_blocks;
#ifdef __APPLE__
  st->atime_ms = ms_of(s.st_atimespec);
  st->mtime_ms = ms_of(s.st_mtimespec);
  st->ctime_ms = ms_of(s.st_ctimespec);
  st->birthtime_ms = ms_of(s.st_birthtimespec);
#else
  st->atime_ms = ms_of(s.st_atim);
  st->mtime_ms = ms_of(s.st_mtim);
  st->ctime_ms = ms_of(s.st_ctim);
  st->birthtime_ms = ms_of(s.st_ctim);
#endif
  return 0;
}
#endif

// statSync and lstatSync: a Stats from `make_stats`; NULL with the error
// thrown, or without one when the file is not there and `throw_if_missing`
// is unset ({ throwIfNoEntry: false }).
void *tz_fs_stat(void *path_obj, uint64_t path_meta, int32_t follow, int32_t throw_if_missing,
                 TzMaker make_stats, TzMaker make) {
  Path p;
  if (!path_init(&p, path_obj, path_meta))
    return NULL;
  TzStatsParts st;
  void *result = NULL;
  const char *syscall = follow ? "stat" : "lstat";
#ifdef _WIN32
  DWORD error = stat_of(wide(&p), follow, &st);
  if (error) {
    Failure f = win32_failure(error);
    if (throw_if_missing || (f.index != E_ENOENT && f.index != E_ENOTDIR))
      fail_win32(make, error, syscall, &p, NULL);
  } else {
    result = make_stats(&st);
  }
#else
  int error = stat_of(native(&p), follow, &st);
  if (error) {
    if (throw_if_missing || (error != ENOENT && error != ENOTDIR))
      fail_errno(make, error, syscall, &p, NULL);
  } else {
    result = make_stats(&st);
  }
#endif
  path_free(&p);
  return result;
}

//===----------------------------------------------------------------------===//
// Directories
//===----------------------------------------------------------------------===//

// One entry of a directory: its name and kind (S_REG...; 0 unknown).
typedef struct {
  TzBuf name;
  uint32_t kind;
} Entry;

typedef struct {
  Entry *items;
  size_t count, capacity;
} Entries;

static void entries_free(Entries *e) {
  for (size_t i = 0; i < e->count; ++i)
    tz_buf_free(&e->items[i].name);
  tz_free(e->items);
  e->items = NULL;
  e->count = e->capacity = 0;
}

static Entry *entries_add(Entries *e) {
  if (e->count == e->capacity) {
    size_t capacity = e->capacity * 2 + 16;
    Entry *items = (Entry *)tz_alloc(capacity * sizeof(Entry));
    if (e->count)
      memcpy(items, e->items, e->count * sizeof(Entry));
    tz_free(e->items);
    e->items = items;
    e->capacity = capacity;
  }
  Entry *entry = &e->items[e->count++];
  tz_buf_init(&entry->name);
  entry->kind = 0;
  return entry;
}

#ifdef _WIN32
// The entries of a directory, in the order the file system keeps them, as
// libuv's scandir gives them; false with the error thrown ("scandir").
static int list_dir(const Path *p, Entries *out, TzMaker make) {
  DWORD attributes = GetFileAttributesW(wide(p));
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    fail_win32(make, GetLastError(), "scandir", p, NULL);
    return 0;
  }
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    fail_index(make, E_ENOTDIR, "scandir", p);
    return 0;
  }
  TzBuf pattern;
  tz_buf_init(&pattern);
  tz_buf_units(&pattern, p->wide.data, p->wide.length - 1);
  if (pattern.length == 0 || pattern.data[pattern.length - 1] != '\\')
    tz_buf_push(&pattern, '\\');
  tz_buf_push(&pattern, '*');
  tz_buf_push(&pattern, 0);
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileExW((const wchar_t *)pattern.data, FindExInfoBasic, &data,
                                 FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
  tz_buf_free(&pattern);
  if (find == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_NO_MORE_FILES)
      return 1; // empty, as the root of an empty drive is
    fail_win32(make, error, "scandir", p, NULL);
    return 0;
  }
  do {
    const wchar_t *name = data.cFileName;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
      continue;
    Entry *entry = entries_add(out);
    tz_buf_units(&entry->name, (const uint16_t *)name, wcslen(name));
    DWORD a = data.dwFileAttributes;
    entry->kind = a & FILE_ATTRIBUTE_DEVICE          ? S_CHR
                  : a & FILE_ATTRIBUTE_REPARSE_POINT ? S_LNK
                  : a & FILE_ATTRIBUTE_DIRECTORY     ? S_DIR
                                                     : S_REG;
  } while (FindNextFileW(find, &data));
  FindClose(find);
  return 1;
}

// Whether the path is a directory, following links, as Node's internal
// stat does for readdirSync with `recursive`.
static int is_directory(const Path *p) {
  TzStatsParts st;
  return stat_of(wide(p), 1, &st) == 0 && ((uint32_t)st.mode & S_KIND) == S_DIR;
}
#else
static int compare_entries(const void *a, const void *b) {
  const Entry *x = (const Entry *)a, *y = (const Entry *)b;
  Bytes u, v;
  bytes_init(&u);
  bytes_init(&v);
  bytes_utf8(&u, x->name.data, x->name.length);
  bytes_utf8(&v, y->name.data, y->name.length);
  size_t n = u.length < v.length ? u.length : v.length;
  int c = n ? memcmp(u.data, v.data, n) : 0;
  if (c == 0)
    c = u.length < v.length ? -1 : u.length > v.length;
  bytes_free(&u);
  bytes_free(&v);
  return c;
}

// The entries sorted by their bytes, as libuv's scandir sorts them.
static int list_dir(const Path *p, Entries *out, TzMaker make) {
  DIR *dir = opendir(native(p));
  if (!dir) {
    fail_errno(make, errno, "scandir", p, NULL);
    return 0;
  }
  struct dirent *d;
  while ((d = readdir(dir)) != NULL) {
    if (d->d_name[0] == '.' && (d->d_name[1] == 0 || (d->d_name[1] == '.' && d->d_name[2] == 0)))
      continue;
    Entry *entry = entries_add(out);
    TzStr name = str_of_utf8((const uint8_t *)d->d_name, strlen(d->d_name));
    tz_buf_str(&entry->name, name);
    tz_str_release_value(name);
    switch (d->d_type) {
    case DT_REG:
      entry->kind = S_REG;
      break;
    case DT_DIR:
      entry->kind = S_DIR;
      break;
    case DT_LNK:
      entry->kind = S_LNK;
      break;
    case DT_CHR:
      entry->kind = S_CHR;
      break;
    case DT_BLK:
      entry->kind = S_BLK;
      break;
    case DT_FIFO:
      entry->kind = S_FIFO;
      break;
    case DT_SOCK:
      entry->kind = S_SOCK;
      break;
    default:
      entry->kind = 0;
      break;
    }
  }
  closedir(dir);
  qsort(out->items, out->count, sizeof(Entry), compare_entries);
  return 1;
}

static int is_directory(const Path *p) {
  struct stat s;
  return stat(native(p), &s) == 0 && S_ISDIR(s.st_mode);
}
#endif

#ifdef _WIN32
#define SEPARATOR '\\'
#else
#define SEPARATOR '/'
#endif

// readdirSync: the names (or with `make_dirent`, Dirents) of the entries of
// a directory, and with `recursive` those of the directories in it, level by
// level as Node lists them, by their paths relative to it.
TzArray *tz_fs_readdir(void *path_obj, uint64_t path_meta, int32_t recursive, const TzType *element,
                       TzMaker make_dirent, TzMaker make) {
  Path base;
  if (!path_init(&base, path_obj, path_meta))
    return NULL;
  TzArray *result = tz_arr_new(element, 0, 16);
  // The directories to list: as the program sees them (joined to the path
  // given), and relative to it.
  typedef struct {
    TzBuf path, relative;
  } Dir;
  Dir *queue = (Dir *)tz_alloc(sizeof(Dir) * 16);
  size_t count = 1, capacity = 16;
  tz_buf_init(&queue[0].path);
  tz_buf_init(&queue[0].relative);
  tz_buf_units(&queue[0].path, base.given.data, base.given.length);
  int ok = 1;
  for (size_t q = 0; q < count && ok; ++q) {
    Path dir;
    path_of_units(&dir, queue[q].path.data, queue[q].path.length);
    Entries entries = {NULL, 0, 0};
    ok = list_dir(q == 0 ? &base : &dir, &entries, make);
    for (size_t i = 0; ok && i < entries.count; ++i) {
      Entry *e = &entries.items[i];
      TzBuf child;
      tz_buf_init(&child);
      tz_path_join_units(&child, queue[q].path.data, queue[q].path.length, e->name.data,
                         e->name.length);
      int descend = 0;
      if (recursive) {
        if (make_dirent) {
          descend = e->kind == S_DIR;
        } else {
          Path c;
          path_of_units(&c, child.data, child.length);
          descend = is_directory(&c);
          path_free(&c);
        }
      }
      if (make_dirent) {
        TzDirentParts parts;
        parts.name = tz_str_from_utf16(e->name.data, e->name.length);
        parts.parent_path = tz_str_from_utf16(queue[q].path.data, queue[q].path.length);
        parts.kind = (int32_t)e->kind;
        void *dirent = make_dirent(&parts);
        tz_arr_push_value(result, &dirent);
      } else {
        TzBuf name;
        tz_buf_init(&name);
        if (queue[q].relative.length) {
          tz_buf_units(&name, queue[q].relative.data, queue[q].relative.length);
          tz_buf_push(&name, SEPARATOR);
        }
        tz_buf_units(&name, e->name.data, e->name.length);
        TzStr s = tz_buf_take(&name);
        tz_arr_push_value(result, &s);
      }
      if (descend) {
        if (count == capacity) {
          Dir *bigger = (Dir *)tz_alloc(sizeof(Dir) * capacity * 2);
          memcpy(bigger, queue, sizeof(Dir) * count);
          tz_free(queue);
          queue = bigger;
          capacity *= 2;
        }
        Dir *next = &queue[count++];
        next->path = child;
        tz_buf_init(&next->relative);
        if (queue[q].relative.length) {
          tz_buf_units(&next->relative, queue[q].relative.data, queue[q].relative.length);
          tz_buf_push(&next->relative, SEPARATOR);
        }
        tz_buf_units(&next->relative, e->name.data, e->name.length);
      } else {
        tz_buf_free(&child);
      }
    }
    entries_free(&entries);
    path_free(&dir);
  }
  for (size_t q = 0; q < count; ++q) {
    tz_buf_free(&queue[q].path);
    tz_buf_free(&queue[q].relative);
  }
  tz_free(queue);
  path_free(&base);
  if (!ok) {
    if (tz_rc_dec(&result->rc))
      tz_arr_free(result);
    return NULL;
  }
  return result;
}

#ifdef _WIN32
static DWORD make_dir(const uint16_t *path) {
  if (CreateDirectoryW((const wchar_t *)path, NULL))
    return 0;
  DWORD error = GetLastError();
  return error == ERROR_INVALID_NAME || error == ERROR_DIRECTORY ? ERROR_INVALID_PARAMETER : error;
}

// Where the last `\` of a path is, or -1.
static ptrdiff_t last_separator(const uint16_t *units, size_t n) {
  for (size_t i = n; i > 0; --i)
    if (units[i - 1] == '\\')
      return (ptrdiff_t)i - 1;
  return -1;
}
#endif

// mkdirSync and mkdir of node:fs/promises, the calls of the system only:
// makes the directory `path` names in the system's form (`n` units or bytes,
// without the zero), and with `recursive` the directories on the way too, as
// Node's MKDirpSync makes them. The first one made goes to `first` (UTF-16
// on Windows, UTF-8 elsewhere: as the system got it, which Node gives), and
// the result is 1 then, 0 for none; -1 with the failure of "mkdir", and in
// `at`, if given, the path that failed, which the async mkdir of Node shows.
#ifdef _WIN32
static int mkdir_raw(const uint16_t *path, size_t n, int32_t recursive, int32_t mode, Raw *first,
                     Failure *failure, Raw *at) {
  (void)mode;
  Raw target = {NULL, 0, 0};
  raw_units(&target, path, n);
  raw_unit(&target, 0);
  if (!recursive) {
    DWORD error = make_dir(raw_wide(&target));
    raw_free(&target);
    if (!error)
      return 0;
    *failure = win32_failure(error);
    return -1;
  }
  // A stack of the paths to make, the target first: a parent that is not
  // there goes on top of its child. Each ends with a zero.
  Raw *stack = (Raw *)malloc(sizeof(Raw) * 8);
  if (!stack)
    abort();
  size_t depth = 1, capacity = 8;
  stack[0] = target;
  int made = 0, failed = 0;
  while (depth > 0 && !failed) {
    Raw next = stack[--depth];
    const uint16_t *units = raw_wide(&next);
    size_t length = raw_count(&next) - 1;
    DWORD error = make_dir(units);
    Failure f = win32_failure(error);
    if (error == 0) {
      if (!made) {
        raw_units(first, units, length);
        made = 1;
      }
    } else if (f.index == E_EACCES || f.index == E_ENOSPC || f.index == E_ENOTDIR ||
               f.index == E_EPERM) {
      failed = 1;
      *failure = f;
    } else if (f.index == E_ENOENT && (depth > 0 || last_separator(units, length) >= 0)) {
      ptrdiff_t cut = last_separator(units, length);
      if (cut >= 0) {
        if (depth + 2 > capacity) {
          capacity *= 2;
          Raw *bigger = (Raw *)realloc(stack, sizeof(Raw) * capacity);
          if (!bigger)
            abort();
          stack = bigger;
        }
        Raw parent = {NULL, 0, 0};
        raw_units(&parent, units, (size_t)cut);
        raw_unit(&parent, 0);
        stack[depth++] = next;
        stack[depth++] = parent;
        continue;
      }
    } else {
      // There already (or with nowhere to go up, as if it were): fine if it
      // is a directory.
      if (f.index == E_ENOENT)
        f = (Failure){E_EEXIST, kErrors[E_EEXIST].windows};
      TzStatsParts st;
      DWORD stat_error = stat_of((const wchar_t *)units, 1, &st);
      if (stat_error == 0 && ((uint32_t)st.mode & S_KIND) != S_DIR) {
        failed = 1;
        *failure = f.index == E_EEXIST && depth > 0
                       ? (Failure){E_ENOTDIR, kErrors[E_ENOTDIR].windows}
                       : (Failure){E_EEXIST, kErrors[E_EEXIST].windows};
      } else if (stat_error) {
        failed = 1;
        *failure = win32_failure(stat_error);
      }
    }
    if (failed && at)
      raw_append(at, next.data, next.length - 2);
    raw_free(&next);
  }
  for (size_t i = 0; i < depth; ++i)
    raw_free(&stack[i]);
  free(stack);
  if (failed) {
    raw_free(first);
    return -1;
  }
  return made;
}
#else
static int mkdir_raw(const uint8_t *path, size_t n, int32_t recursive, int32_t mode, Raw *first,
                     Failure *failure, Raw *at) {
  Raw target = {NULL, 0, 0};
  raw_append(&target, path, n);
  raw_append(&target, "", 1);
  if (!recursive) {
    int error = mkdir((const char *)target.data, (mode_t)mode) == 0 ? 0 : errno;
    raw_free(&target);
    if (!error)
      return 0;
    *failure = errno_failure(error);
    return -1;
  }
  // The same on bytes, at '/'.
  Raw *stack = (Raw *)malloc(sizeof(Raw) * 8);
  if (!stack)
    abort();
  size_t depth = 1, capacity = 8;
  stack[0] = target;
  int made = 0, failed = 0, error_number = 0;
  while (depth > 0 && !failed) {
    Raw next = stack[--depth];
    int error = mkdir((const char *)next.data, (mode_t)mode) == 0 ? 0 : errno;
    if (error == 0) {
      if (!made) {
        raw_append(first, next.data, next.length - 1);
        made = 1;
      }
    } else if (error == EACCES || error == ENOSPC || error == ENOTDIR || error == EPERM) {
      failed = 1;
      error_number = error;
    } else if (error == ENOENT) {
      char *slash = strrchr((char *)next.data, '/');
      if (slash && slash != (char *)next.data) {
        if (depth + 2 > capacity) {
          capacity *= 2;
          Raw *bigger = (Raw *)realloc(stack, sizeof(Raw) * capacity);
          if (!bigger)
            abort();
          stack = bigger;
        }
        Raw parent = {NULL, 0, 0};
        raw_append(&parent, next.data, (size_t)(slash - (char *)next.data));
        raw_append(&parent, "", 1);
        stack[depth++] = next;
        stack[depth++] = parent;
        continue;
      }
      if (depth == 0) {
        failed = 1;
        error_number = EEXIST;
      }
    } else {
      struct stat st;
      if (stat((const char *)next.data, &st) == 0 && !S_ISDIR(st.st_mode)) {
        failed = 1;
        error_number = error == EEXIST && depth > 0 ? ENOTDIR : EEXIST;
      }
    }
    if (failed && at)
      raw_append(at, next.data, next.length - 1);
    raw_free(&next);
  }
  for (size_t i = 0; i < depth; ++i)
    raw_free(&stack[i]);
  free(stack);
  if (failed) {
    raw_free(first);
    *failure = errno_failure(error_number);
    return -1;
  }
  return made;
}
#endif

// The first directory mkdir_raw made, as a string.
static TzStr made_dir(const Raw *first) {
#ifdef _WIN32
  return tz_str_from_utf16(raw_wide(first), raw_count(first));
#else
  return str_of_utf8(first->data, first->length);
#endif
}

// mkdirSync. The first directory made is the result, with `recursive`;
// false for none.
int32_t tz_fs_mkdir(TzStr *out, void *path_obj, uint64_t path_meta, int32_t recursive, int32_t mode,
                    TzMaker make) {
  out->obj = NULL;
  out->meta = 0;
  Path p;
  if (!path_init(&p, path_obj, path_meta))
    return 0;
  Raw first = {NULL, 0, 0};
  Failure failure;
#ifdef _WIN32
  int made = mkdir_raw(p.wide.data, p.wide.length - 1, recursive, mode, &first, &failure, NULL);
#else
  int made = mkdir_raw(p.utf8.data, p.utf8.length - 1, recursive, mode, &first, &failure, NULL);
#endif
  if (made < 0) {
    throw_failure(make, failure, "mkdir", &p.shown, NULL);
    made = 0;
  } else if (made) {
    *out = made_dir(&first);
  }
  raw_free(&first);
  path_free(&p);
  return made;
}

// mkdtempSync(prefix): a new directory named the prefix and six random
// letters and digits, as given (not resolved).
void tz_fs_mkdtemp(TzStr *out, void *prefix_obj, uint64_t prefix_meta, TzMaker make) {
  static const char letters[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  out->obj = NULL;
  out->meta = 0;
  Path prefix;
  if (!path_init(&prefix, prefix_obj, prefix_meta))
    return;
  for (int attempt = 0;; ++attempt) {
    TzBuf name;
    tz_buf_init(&name);
    tz_buf_units(&name, prefix.given.data, prefix.given.length);
    for (int i = 0; i < 6; ++i) {
      unsigned int r;
#ifdef _WIN32
      if (rand_s(&r) != 0)
        r = (unsigned int)GetTickCount() * 2654435761u + (unsigned int)i;
#else
      r = (unsigned int)rand() ^ (unsigned int)time(NULL) ^ ((unsigned int)getpid() << 8);
#endif
      tz_buf_push(&name, (uint16_t)letters[r % 62]);
    }
    Path dir;
    path_of_units(&dir, name.data, name.length);
#ifdef _WIN32
    DWORD error = make_dir(dir.wide.data);
    int exists = error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS;
    Failure f = win32_failure(error);
#else
    int error = mkdir(native(&dir), 0700) == 0 ? 0 : errno;
    int exists = error == EEXIST;
    Failure f = error ? errno_failure(error) : (Failure){0, 0};
#endif
    path_free(&dir);
    if (!error) {
      *out = tz_buf_take(&name);
      break;
    }
    if (!exists || attempt >= 100) {
      // The template, as Node shows it.
      TzBuf shown;
      tz_buf_init(&shown);
      tz_buf_units(&shown, prefix.given.data, prefix.given.length);
      tz_buf_ascii(&shown, "XXXXXX");
      throw_failure(make, f, "mkdtemp", &shown, NULL);
      tz_buf_free(&shown);
      tz_buf_free(&name);
      break;
    }
    tz_buf_free(&name);
  }
  path_free(&prefix);
}

void tz_fs_rmdir(void *path_obj, uint64_t path_meta, TzMaker make) {
  Path p;
  if (!path_init(&p, path_obj, path_meta))
    return;
#ifdef _WIN32
  if (!RemoveDirectoryW(wide(&p)))
    fail_win32(make, GetLastError(), "rmdir", &p, NULL);
#else
  if (rmdir(native(&p)) != 0)
    fail_errno(make, errno, "rmdir", &p, NULL);
#endif
  path_free(&p);
}

#ifdef _WIN32
// Deletes a file, or the link a directory link is, as libuv's unlink does:
// a read-only file too, but not a directory (EPERM, as POSIX says). 0 or the
// error.
static DWORD unlink_path(const wchar_t *path) {
  DWORD attributes = GetFileAttributesW(path);
  if (attributes == INVALID_FILE_ATTRIBUTES)
    return GetLastError();
  if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
    if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
      return ERROR_ACCESS_DENIED;
    return RemoveDirectoryW(path) ? 0 : GetLastError();
  }
  if ((attributes & FILE_ATTRIBUTE_READONLY) &&
      !SetFileAttributesW(path, attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY))
    return GetLastError();
  if (DeleteFileW(path))
    return 0;
  DWORD error = GetLastError();
  if (attributes & FILE_ATTRIBUTE_READONLY)
    SetFileAttributesW(path, attributes);
  return error;
}

// Deletes a directory with everything in it; links are deleted, not
// followed. `path` holds the units of the path, without a zero; `depth`
// grows to the levels of directories it went down.
static DWORD remove_tree(Raw *path, int *depth) {
  size_t length = path->length;
  raw_unit(path, 0);
  DWORD attributes = GetFileAttributesW((const wchar_t *)path->data);
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    path->length = length;
    return GetLastError();
  }
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    DWORD error = unlink_path((const wchar_t *)path->data);
    path->length = length;
    return error;
  }
  path->length = length;
  static const uint16_t all[] = {'\\', '*', 0};
  raw_units(path, all, 3);
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileExW((const wchar_t *)path->data, FindExInfoBasic, &data,
                                 FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
  path->length = length;
  DWORD error = 0;
  int deepest = 0;
  if (find != INVALID_HANDLE_VALUE) {
    do {
      const wchar_t *name = data.cFileName;
      if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
        continue;
      raw_unit(path, '\\');
      raw_units(path, (const uint16_t *)name, wcslen(name));
      int below = 0;
      error = remove_tree(path, &below);
      if (below > deepest)
        deepest = below;
      path->length = length;
    } while (!error && FindNextFileW(find, &data));
    FindClose(find);
  }
  *depth = deepest + 1;
  if (error)
    return error;
  raw_unit(path, 0);
  if ((attributes & FILE_ATTRIBUTE_READONLY))
    SetFileAttributesW((const wchar_t *)path->data, attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY);
  if (!RemoveDirectoryW((const wchar_t *)path->data))
    error = GetLastError();
  path->length = length;
  return error;
}
#else
static int remove_tree(Raw *path, int *depth) {
  size_t length = path->length; // with the zero
  struct stat s;
  if (lstat((const char *)path->data, &s) != 0)
    return errno;
  if (!S_ISDIR(s.st_mode))
    return unlink((const char *)path->data) == 0 ? 0 : errno;
  DIR *dir = opendir((const char *)path->data);
  if (!dir)
    return errno;
  int error = 0, deepest = 0;
  struct dirent *d;
  while (!error && (d = readdir(dir)) != NULL) {
    if (d->d_name[0] == '.' && (d->d_name[1] == 0 || (d->d_name[1] == '.' && d->d_name[2] == 0)))
      continue;
    path->length = length - 1;
    raw_append(path, "/", 1);
    raw_append(path, d->d_name, strlen(d->d_name) + 1);
    int below = 0;
    error = remove_tree(path, &below);
    if (below > deepest)
      deepest = below;
  }
  closedir(dir);
  *depth = deepest + 1;
  path->length = length - 1;
  raw_append(path, "", 1);
  if (!error && rmdir((const char *)path->data) != 0)
    error = errno;
  return error;
}
#endif

void tz_fs_unlink(void *path_obj, uint64_t path_meta, TzMaker make) {
  Path p;
  if (!path_init(&p, path_obj, path_meta))
    return;
#ifdef _WIN32
  DWORD error = unlink_path(wide(&p));
  if (error)
    fail_win32(make, error, "unlink", &p, NULL);
#else
  if (unlink(native(&p)) != 0)
    fail_errno(make, errno, "unlink", &p, NULL);
#endif
  path_free(&p);
}

// What rm_raw found.
enum { RM_DONE, RM_FAILED, RM_DIRECTORY };

// rmSync and rm of node:fs/promises, the calls of the system only, as Node's:
// a path that is not there is an error ("lstat") unless `force`; a directory
// needs `recursive`, or it is RM_DIRECTORY (SystemError ERR_FS_EISDIR).
// `depth` gets the levels of directories removed.
static int rm_raw(const Path *p, int32_t recursive, int32_t force, Failure *failure,
                  const char **syscall, int *depth) {
  TzStatsParts st;
  *depth = 0;
#ifdef _WIN32
  DWORD error = stat_of(wide(p), 0, &st);
  Failure missing = win32_failure(error);
#else
  int error = stat_of(native(p), 0, &st);
  Failure missing = error ? errno_failure(error) : (Failure){0, 0};
#endif
  if (error) {
    if (force && (missing.index == E_ENOENT || missing.index == E_ENOTDIR))
      return RM_DONE;
    *failure = missing;
    *syscall = "lstat";
    return RM_FAILED;
  }
  if (((uint32_t)st.mode & S_KIND) == S_DIR && !recursive)
    return RM_DIRECTORY;
  Raw tree = {NULL, 0, 0};
#ifdef _WIN32
  raw_units(&tree, p->wide.data, p->wide.length - 1);
  error = remove_tree(&tree, depth);
  raw_free(&tree);
  if (error && !(force && win32_failure(error).index == E_ENOENT)) {
    *failure = win32_failure(error);
    *syscall = "rm";
    return RM_FAILED;
  }
#else
  raw_append(&tree, p->utf8.data, p->utf8.length);
  error = remove_tree(&tree, depth);
  raw_free(&tree);
  if (error && !(force && error == ENOENT)) {
    *failure = errno_failure(error);
    *syscall = "rm";
    return RM_FAILED;
  }
#endif
  return RM_DONE;
}

// SystemError ERR_FS_EISDIR of rm, whose message shows the path as given.
static void throw_rm_directory(const Path *p, TzMaker make) {
  TzBuf m;
  tz_buf_init(&m);
  tz_buf_ascii(&m, "Path is a directory: rm returned EISDIR (is a directory) ");
  tz_buf_units(&m, p->given.data, p->given.length);
  TzSystemErrorParts parts;
  memset(&parts, 0, sizeof parts);
  parts.message = tz_buf_take(&m);
  parts.name = ascii("SystemError");
  parts.errno_value = 21; // EISDIR of POSIX, on every system
  parts.code = ascii("ERR_FS_EISDIR");
  parts.syscall = ascii("rm");
  parts.path = opt_of(&p->given);
  throw_parts(make, &parts);
}

void tz_fs_rm(void *path_obj, uint64_t path_meta, int32_t recursive, int32_t force, TzMaker make) {
  Path p;
  if (!path_init(&p, path_obj, path_meta))
    return;
  Failure failure;
  const char *syscall = NULL;
  int depth;
  switch (rm_raw(&p, recursive, force, &failure, &syscall, &depth)) {
  case RM_FAILED:
    throw_failure(make, failure, syscall, &p.shown, NULL);
    break;
  case RM_DIRECTORY:
    throw_rm_directory(&p, make);
    break;
  default:
    break;
  }
  path_free(&p);
}

//===----------------------------------------------------------------------===//
// renameSync, copyFileSync, realpathSync
//===----------------------------------------------------------------------===//

void tz_fs_rename(void *from_obj, uint64_t from_meta, void *to_obj, uint64_t to_meta,
                  TzMaker make) {
  Path from, to;
  if (!path_init(&from, from_obj, from_meta))
    return;
  if (!path_init(&to, to_obj, to_meta)) {
    path_free(&from);
    return;
  }
#ifdef _WIN32
  if (!MoveFileExW(wide(&from), wide(&to), MOVEFILE_REPLACE_EXISTING))
    fail_win32(make, GetLastError(), "rename", &from, &to);
#else
  if (rename(native(&from), native(&to)) != 0)
    fail_errno(make, errno, "rename", &from, &to);
#endif
  path_free(&from);
  path_free(&to);
}

// copyFileSync with the mode of fs.constants: COPYFILE_EXCL (1) fails if the
// destination is there; COPYFILE_FICLONE_FORCE (4) is not possible here.
void tz_fs_copy_file(void *from_obj, uint64_t from_meta, void *to_obj, uint64_t to_meta,
                     int32_t mode, TzMaker make) {
  Path from, to;
  if (!path_init(&from, from_obj, from_meta))
    return;
  if (!path_init(&to, to_obj, to_meta)) {
    path_free(&from);
    return;
  }
#ifdef _WIN32
  if (mode & 4) {
    throw_failure(make, (Failure){E_ENOSYS, kErrors[E_ENOSYS].windows}, "copyfile", &from.shown,
                  &to.shown);
  } else if (!CopyFileW(wide(&from), wide(&to), (mode & 1) != 0)) {
    DWORD error = GetLastError();
    // The same file: there is nothing to copy (libuv checks for EBUSY).
    TzStatsParts a, b;
    int same = win32_failure(error).index == E_EBUSY && stat_of(wide(&from), 1, &a) == 0 &&
               stat_of(wide(&to), 1, &b) == 0 && a.dev == b.dev && a.ino == b.ino;
    if (!same)
      fail_win32(make, error, "copyfile", &from, &to);
  }
#else
  int in = -1, out = -1;
  if (mode & 4) {
    fail_errno(make, ENOSYS, "copyfile", &from, &to);
  } else if ((in = open(native(&from), O_RDONLY | O_CLOEXEC)) < 0) {
    fail_errno(make, errno, "copyfile", &from, &to);
  } else {
    struct stat s;
    fstat(in, &s);
    out = open(native(&to), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | ((mode & 1) ? O_EXCL : 0),
               s.st_mode & 0777);
    if (out < 0) {
      fail_errno(make, errno, "copyfile", &from, &to);
    } else {
      char chunk[65536];
      ssize_t got;
      while ((got = read(in, chunk, sizeof chunk)) > 0)
        if (write(out, chunk, (size_t)got) != got) {
          fail_errno(make, errno, "copyfile", &from, &to);
          break;
        }
      close(out);
    }
    close(in);
  }
#endif
  path_free(&from);
  path_free(&to);
}

#ifdef _WIN32
// Where a link leads, its whole real path; false if it cannot be found.
static int final_path(const Path *p, TzBuf *out) {
  HANDLE h = CreateFileW(wide(p), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (h == INVALID_HANDLE_VALUE)
    return 0;
  wchar_t small[MAX_PATH + 8];
  wchar_t *buffer = small;
  DWORD capacity = MAX_PATH + 8;
  DWORD n = GetFinalPathNameByHandleW(h, buffer, capacity, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (n >= capacity) {
    capacity = n + 1;
    buffer = (wchar_t *)tz_alloc(capacity * sizeof(wchar_t));
    n = GetFinalPathNameByHandleW(h, buffer, capacity, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  }
  CloseHandle(h);
  int ok = n > 0 && n < capacity;
  if (ok) {
    const uint16_t *s = (const uint16_t *)buffer;
    if (n >= 8 && memcmp(s, L"\\\\?\\UNC\\", 16) == 0) {
      tz_buf_ascii(out, "\\\\");
      tz_buf_units(out, s + 8, n - 8);
    } else if (n >= 4 && memcmp(s, L"\\\\?\\", 8) == 0) {
      tz_buf_units(out, s + 4, n - 4);
    } else {
      tz_buf_units(out, s, n);
    }
  }
  if (buffer != small)
    tz_free(buffer);
  return ok;
}

// The root of a resolved path: `C:\` or `\\server\share\`.
static size_t root_length(const TzBuf *p) {
  const uint16_t *s = p->data;
  size_t n = p->length, i = 0;
  if (n >= 2 && s[1] == ':')
    i = 2;
  else if (n >= 2 && (s[0] == '\\' || s[0] == '/') && (s[1] == '\\' || s[1] == '/')) {
    size_t j = 2;
    while (j < n && s[j] != '\\' && s[j] != '/')
      ++j;
    if (j > 2 && j < n) {
      size_t k = j + 1;
      while (k < n && s[k] != '\\' && s[k] != '/')
        ++k;
      if (k > j + 1)
        i = k;
    }
  }
  while (i < n && (s[i] == '\\' || s[i] == '/'))
    ++i;
  return i;
}
#endif

// realpathSync, as Node's (written in JavaScript): the path resolved, with
// each part that is a link replaced by where it leads; the letters stay as
// written. A part that is not there is ENOENT of "lstat" for the path up to
// it.
void tz_fs_realpath(TzStr *out, void *path_obj, uint64_t path_meta, TzMaker make) {
  out->obj = NULL;
  out->meta = 0;
  Path given;
  if (!path_init(&given, path_obj, path_meta))
    return;
#ifdef _WIN32
  TzBuf p;
  tz_buf_init(&p);
  tz_path_resolve_units(&p, given.given.data, given.given.length);
  int ok = 1;
  for (int round = 0; ok; ++round) {
    if (round > 64) {
      Path at;
      path_of_units(&at, p.data, p.length);
      fail_index(make, E_ELOOP, "lstat", &at);
      path_free(&at);
      ok = 0;
      break;
    }
    size_t pos = root_length(&p);
    int restarted = 0;
    // The root first, then each part.
    for (size_t end = pos;; ++end) {
      if (end < p.length && p.data[end] != '\\' && p.data[end] != '/')
        continue;
      Path at;
      path_of_units(&at, p.data, end);
      DWORD attributes = GetFileAttributesW(wide(&at));
      if (attributes == INVALID_FILE_ATTRIBUTES) {
        fail_win32(make, GetLastError(), "lstat", &at, NULL);
        path_free(&at);
        ok = 0;
        break;
      }
      if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        TzBuf target;
        tz_buf_init(&target);
        if (final_path(&at, &target)) {
          // resolve(target, rest)
          TzBuf next;
          tz_buf_init(&next);
          tz_buf_units(&next, target.data, target.length);
          if (end < p.length) {
            tz_buf_push(&next, '\\');
            tz_buf_units(&next, p.data + end + 1, p.length - end - 1);
          }
          tz_buf_free(&p);
          tz_buf_init(&p);
          tz_path_resolve_units(&p, next.data, next.length);
          tz_buf_free(&next);
          restarted = 1;
        }
        tz_buf_free(&target);
      }
      path_free(&at);
      if (restarted || end >= p.length)
        break;
    }
    if (!restarted)
      break;
  }
  if (ok)
    *out = tz_buf_take(&p);
  else
    tz_buf_free(&p);
#else
  char *real = realpath(native(&given), NULL);
  if (!real) {
    fail_errno(make, errno, "lstat", &given, NULL);
  } else {
    *out = str_of_utf8((const uint8_t *)real, strlen(real));
    free(real);
  }
#endif
  path_free(&given);
}

//===----------------------------------------------------------------------===//
// node:fs/promises
//===----------------------------------------------------------------------===//

// The functions of node:fs/promises (docs/io.md, step 4) are those above
// with the waiting moved to a thread of the pool (io.c). The main thread
// checks the arguments and prepares the paths; an error there (a zero in a
// path, a bad flag) rejects the promise at once, as an async function of
// Node does. The thread makes the calls of the system and keeps what they
// give in memory of its own (Raw), and the main thread makes the result of
// it, or the SystemError, which rejects the promise. Each request says how
// many turns of the loop Node takes for the same (TzWork::turns): its round
// trips to its pool.

// An entry of a directory that a thread listed: its name (UTF-16 on
// Windows, the bytes elsewhere), its kind (S_REG...; 0 unknown) and the
// directory it is in, by number.
typedef struct {
  Raw name;
  uint32_t kind;
  uint32_t dir;
} Listed;

typedef struct {
  Listed *items;
  size_t count, capacity;
} ListedList;

static Listed *listed_add(ListedList *list) {
  if (list->count == list->capacity) {
    size_t capacity = list->capacity * 2 + 16;
    Listed *items = (Listed *)realloc(list->items, capacity * sizeof(Listed));
    if (!items)
      abort();
    list->items = items;
    list->capacity = capacity;
  }
  Listed *entry = &list->items[list->count++];
  memset(entry, 0, sizeof *entry);
  return entry;
}

static void listed_free(ListedList *list) {
  for (size_t i = 0; i < list->count; ++i)
    raw_free(&list->items[i].name);
  free(list->items);
  list->items = NULL;
  list->count = list->capacity = 0;
}

typedef struct FsRequest {
  TzWork work;
  TzPromise *promise; // held until settled
  TzMaker make;       // of SystemError
  TzMaker make_item;  // of Stats, or of each Dirent
  const TzType *element;
  void *signal; // an AbortSignal, held; or NULL
  Path path, dest;
  int paths; // how many of path and dest are made
  OpenFlags flags;
  int32_t mode, recursive, force, follow, attempts;
  // readFile: the encoding of the text (as numbered in buffer.c), or -1 for
  // a Buffer of the bytes.
  int32_t encoding;
  int64_t length;
  Raw data; // what writeFile writes, or what readFile read
  // What the thread found.
  int failed;
  Failure failure;
  const char *syscall;
  int shows;       // the paths the error shows: 0, 1 (path) or 2 (and dest)
  int directory;   // rm of a directory without `recursive`
  Raw name;        // mkdir: the first made; mkdtemp: the name; realpath: the
                   // path
  Raw at;          // the path that failed, of mkdir and readdir: the system's form
  int made;
  TzStatsParts st;
  ListedList entries; // readdir, in the order Node gives them
  // readdir: each directory listed, by number, as the entry that names it in
  // its parent (UINT32_MAX for the one given).
  uint32_t *dirs;
  size_t dir_count, dir_capacity;
} FsRequest;

static FsRequest *fsp_new(TzPromise *p, TzMaker make) {
  FsRequest *r = (FsRequest *)tz_alloc(sizeof(FsRequest));
  memset(r, 0, sizeof *r);
  r->promise = p;
  tz_ref_retain(p); // it may be a member of a group (async.c)
  r->make = make;
  return r;
}

static void fsp_free(FsRequest *r) {
  if (r->paths > 0)
    path_free(&r->path);
  if (r->paths > 1)
    path_free(&r->dest);
  raw_free(&r->data);
  raw_free(&r->name);
  raw_free(&r->at);
  listed_free(&r->entries);
  free(r->dirs);
  if (r->signal)
    tz_instance_release((TzInstance *)r->signal);
  tz_obj_release((TzObject *)r->promise);
  tz_free(r);
}

// Rejects the promise with the error in flight (of the arguments, or that
// throw_failure made), and frees the request.
static void fsp_reject_thrown(FsRequest *r) {
  void *error = tz_thrown;
  tz_thrown = NULL;
  tz_promise_reject(r->promise, error);
  fsp_free(r);
}

// The path at `obj` as the first (or second) path of the request; false with
// the promise rejected.
static int fsp_path(FsRequest *r, void *obj, uint64_t meta) {
  Path *p = r->paths == 0 ? &r->path : &r->dest;
  if (!path_init(p, obj, meta)) {
    fsp_reject_thrown(r);
    return 0;
  }
  ++r->paths;
  return 1;
}

// A request with a signal that was aborted already is rejected with an
// AbortError, as Node checks it first.
static int fsp_signal(FsRequest *r, void *signal) {
  if (!signal)
    return 1;
  tz_ref_retain(signal); // a member of a group (signal.c)
  r->signal = signal;
  if (!tz_signal_aborted(signal))
    return 1;
  tz_promise_reject(r->promise, tz_abort_error(signal));
  fsp_free(r);
  return 0;
}

static void fsp_queue(FsRequest *r, void (*work)(TzWork *), void (*done)(TzWork *)) {
  r->work.work = work;
  r->work.done = done;
  r->work.turns = 1;
  // Done settles the promise and frees the request, nothing else.
  tz_work_queue_settles(&r->work, r->promise);
}

// The failure the thread found, of `syscall`, showing `shows` paths.
static void fsp_fail(FsRequest *r, Failure failure, const char *syscall, int shows) {
  r->failed = 1;
  r->failure = failure;
  r->syscall = syscall;
  r->shows = shows;
}

#ifdef _WIN32
static void fsp_fail_system(FsRequest *r, DWORD error, const char *syscall, int shows) {
  fsp_fail(r, win32_failure(error), syscall, shows);
}
#else
static void fsp_fail_system(FsRequest *r, int error, const char *syscall, int shows) {
  fsp_fail(r, errno_failure(error), syscall, shows);
}
#endif

// What the request gives once it is back: false after it settled the
// promise with its failure, or with an AbortError if its signal was
// aborted meanwhile, and freed it.
static int fsp_succeeded(FsRequest *r) {
  if (r->signal && tz_signal_aborted(r->signal)) {
    tz_promise_reject(r->promise, tz_abort_error(r->signal));
    fsp_free(r);
    return 0;
  }
  if (!r->failed)
    return 1;
  if (r->at.length) {
    // A path inside the one given failed: the error shows it, as the
    // program would name it, without the prefix of the system's form.
    TzBuf units;
    tz_buf_init(&units);
#ifdef _WIN32
    tz_buf_units(&units, raw_wide(&r->at), raw_count(&r->at));
    size_t skip = units.length >= 4 && units.data[0] == '\\' && units.data[1] == '\\' &&
                          units.data[2] == '?' && units.data[3] == '\\'
                      ? 4
                      : 0;
#else
    TzStr text = str_of_utf8(r->at.data, r->at.length);
    tz_buf_str(&units, text);
    tz_str_release_value(text);
    size_t skip = 0;
#endif
    Path at;
    path_of_units(&at, units.data + skip, units.length - skip);
    tz_buf_free(&units);
    throw_failure(r->make, r->failure, r->syscall, &at.shown, NULL);
    path_free(&at);
  } else {
    throw_failure(r->make, r->failure, r->syscall, r->shows > 0 ? &r->path.shown : NULL,
                  r->shows > 1 ? &r->dest.shown : NULL);
  }
  fsp_reject_thrown(r);
  return 0;
}

// Fulfills the promise with the `size` bytes at `value` (which it takes
// over), or none, and frees the request.
static void fsp_fulfill(FsRequest *r, const void *value, size_t size) {
  if (size)
    memcpy((char *)r->promise + TZ_PROMISE_VALUE, value, size);
  tz_promise_fulfill(r->promise);
  fsp_free(r);
}

static void fsp_done_void(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  if (fsp_succeeded(r))
    fsp_fulfill(r, NULL, 0);
}

// The number of pieces of `length` bytes Node reads or writes a file in.
static uint32_t chunks_of(size_t length) {
  const size_t chunk = 512 * 1024; // kReadFileBufferLength, kWriteFileMaxChunkSize
  return (uint32_t)((length + chunk - 1) / chunk);
}

//===----------------------------------------------------------------------===//
// readFile, writeFile and appendFile
//===----------------------------------------------------------------------===//

#ifdef _WIN32
// Reads to the end into `out`; 0 or the error.
static DWORD read_raw(File f, Raw *out) {
  // As much as the file has, and one byte more to find its end: a block of
  // its size, which the heap of the system gives the threads at once, where
  // big blocks would make them wait for each other.
  LARGE_INTEGER size;
  if (GetFileSizeEx(f, &size) && size.QuadPart >= 0 && size.QuadPart < ((int64_t)1 << 31))
    raw_reserve(out, (size_t)size.QuadPart + 1);
  for (;;) {
    if (out->length == out->capacity)
      raw_reserve(out, 65536);
    DWORD room = (DWORD)(out->capacity - out->length < 0x40000000 ? out->capacity - out->length
                                                                  : 0x40000000);
    DWORD got = 0;
    if (!ReadFile(f, out->data + out->length, room, &got, NULL)) {
      DWORD error = GetLastError();
      return error == ERROR_HANDLE_EOF || error == ERROR_BROKEN_PIPE ? 0 : error;
    }
    if (got == 0)
      return 0;
    out->length += got;
  }
}

static DWORD write_raw(File f, const uint8_t *data, size_t n) {
  while (n > 0) {
    DWORD chunk = n > 0x40000000 ? 0x40000000 : (DWORD)n, written = 0;
    if (!WriteFile(f, data, chunk, &written, NULL))
      return GetLastError();
    data += written;
    n -= written;
  }
  return 0;
}
#else
static int read_raw(File f, Raw *out) {
  struct stat st;
  if (fstat(f, &st) == 0 && S_ISREG(st.st_mode) && st.st_size < ((off_t)1 << 31))
    raw_reserve(out, (size_t)st.st_size + 1);
  for (;;) {
    if (out->length == out->capacity)
      raw_reserve(out, 65536);
    ssize_t got = read(f, out->data + out->length, out->capacity - out->length);
    if (got < 0) {
      if (errno == EINTR)
        continue;
      return errno;
    }
    if (got == 0)
      return 0;
    out->length += (size_t)got;
  }
}

static int write_raw(File f, const uint8_t *data, size_t n) {
  while (n > 0) {
    ssize_t written = write(f, data, n);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      return errno;
    }
    data += written;
    n -= (size_t)written;
  }
  return 0;
}
#endif

static void read_file_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  Failure failure;
  File f = open_raw(&r->path, r->flags, 0666, &failure);
  if (f == NO_FILE) {
    fsp_fail(r, failure, "open", 1);
    return;
  }
  // open, fstat, the reads and close.
#ifdef _WIN32
  DWORD error = read_raw(f, &r->data);
#else
  int error = read_raw(f, &r->data);
#endif
  if (error)
    fsp_fail_system(r, error, "read", 0);
  close_file(f);
  r->work.turns = 3 + (r->data.length ? chunks_of(r->data.length) : 1);
}

static void read_file_done(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  if (!fsp_succeeded(r))
    return;
  // The main thread makes the value: a Buffer, or the text.
  if (r->encoding < 0) {
    TzBytes *b = tz_buffer_copy_of(r->data.data, (int64_t)r->data.length);
    fsp_fulfill(r, &b, sizeof b);
    return;
  }
  TzStr text = r->encoding == 0 ? str_of_utf8(r->data.data, r->data.length)
                                : tz_buffer_decode(r->data.data, (int64_t)r->data.length,
                                                   r->encoding);
  fsp_fulfill(r, &text, sizeof text);
}

void tz_fsp_read_file(TzPromise *p, void *path_obj, uint64_t path_meta, void *flag_obj,
                      uint64_t flag_meta, void *signal, int32_t encoding, TzMaker make) {
  FsRequest *r = fsp_new(p, make);
  r->encoding = encoding;
  if (!fsp_signal(r, signal))
    return;
  if (!flags_of(flag_obj, flag_meta, &r->flags)) {
    fsp_reject_thrown(r);
    return;
  }
  if (!fsp_path(r, path_obj, path_meta))
    return;
  fsp_queue(r, read_file_work, read_file_done);
}

static void write_file_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  Failure failure;
  File f = open_raw(&r->path, r->flags, r->mode, &failure);
  if (f == NO_FILE) {
    fsp_fail(r, failure, "open", 1);
    return;
  }
#ifdef _WIN32
  DWORD error = write_raw(f, r->data.data, r->data.length);
#else
  int error = write_raw(f, r->data.data, r->data.length);
#endif
  if (error)
    fsp_fail_system(r, error, "write", 0);
  close_file(f);
  // open, the writes and close.
  r->work.turns = 2 + chunks_of(r->data.length);
}

// What writeFile and appendFile write: a request for the path, the flag and
// the mode, or NULL once it is settled with an error.
static FsRequest *write_request(TzPromise *p, void *path_obj, uint64_t path_meta,
                                void *flag_obj, uint64_t flag_meta, int32_t mode, void *signal,
                                TzMaker make) {
  FsRequest *r = fsp_new(p, make);
  if (!fsp_signal(r, signal))
    return NULL;
  if (!flags_of(flag_obj, flag_meta, &r->flags)) {
    fsp_reject_thrown(r);
    return NULL;
  }
  if (!fsp_path(r, path_obj, path_meta))
    return NULL;
  r->mode = mode;
  return r;
}

// writeFile and appendFile of text, whose flag is "a", in an encoding of
// Buffer (0 is utf8).
void tz_fsp_write_file(TzPromise *p, void *path_obj, uint64_t path_meta, void *data_obj,
                       uint64_t data_meta, void *flag_obj, uint64_t flag_meta, int32_t mode,
                       void *signal, int32_t encoding, TzMaker make) {
  FsRequest *r =
      write_request(p, path_obj, path_meta, flag_obj, flag_meta, mode, signal, make);
  if (!r)
    return;
  if (encoding != 0) {
    // The bytes of the text in that encoding, a copy the thread writes.
    TzBytes *b = tz_buffer_from_string(data_obj, data_meta, encoding);
    raw_reserve(&r->data, (size_t)b->length + 1);
    memcpy(r->data.data, b->data, (size_t)b->length);
    r->data.length = (size_t)b->length;
    tz_instance_release((TzInstance *)b);
    fsp_queue(r, write_file_work, fsp_done_void);
    return;
  }
  // The text as UTF-8, which the thread writes.
  TzBuf units;
  tz_buf_init(&units);
  tz_buf_str(&units, (TzStr){data_obj, data_meta});
  raw_reserve(&r->data, units.length * 3);
  r->data.length = (size_t)(utf8_encode(r->data.data, units.data, units.length) - r->data.data);
  tz_buf_free(&units);
  fsp_queue(r, write_file_work, fsp_done_void);
}

// writeFile and appendFile of a typed array, a Buffer or a DataView: a copy
// of its bytes, which the thread writes while the program goes on.
void tz_fsp_write_file_bytes(TzPromise *p, void *path_obj, uint64_t path_meta, const TzBytes *b,
                             void *flag_obj, uint64_t flag_meta, int32_t mode, void *signal,
                             TzMaker make) {
  FsRequest *r =
      write_request(p, path_obj, path_meta, flag_obj, flag_meta, mode, signal, make);
  if (!r)
    return;
  size_t length = (size_t)tz_bytes_byte_length(b);
  raw_reserve(&r->data, length + 1);
  memcpy(r->data.data, b->data, length);
  r->data.length = length;
  fsp_queue(r, write_file_work, fsp_done_void);
}

//===----------------------------------------------------------------------===//
// access, chmod, truncate, stat and lstat
//===----------------------------------------------------------------------===//

static void access_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
#ifdef _WIN32
  // As libuv: only writing can be denied, to a read-only file.
  DWORD attributes = GetFileAttributesW(wide(&r->path));
  if (attributes == INVALID_FILE_ATTRIBUTES)
    fsp_fail_system(r, GetLastError(), "access", 1);
  else if ((r->mode & 2) && (attributes & FILE_ATTRIBUTE_READONLY) &&
           !(attributes & FILE_ATTRIBUTE_DIRECTORY))
    fsp_fail(r, (Failure){E_EPERM, kErrors[E_EPERM].windows}, "access", 1);
#else
  if (access(native(&r->path), r->mode) != 0)
    fsp_fail_system(r, errno, "access", 1);
#endif
}

void tz_fsp_access(TzPromise *p, void *path_obj, uint64_t path_meta, int32_t mode, TzMaker make) {
  FsRequest *r = fsp_new(p, make);
  if (!fsp_path(r, path_obj, path_meta))
    return;
  r->mode = mode;
  fsp_queue(r, access_work, fsp_done_void);
}

static void chmod_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
#ifdef _WIN32
  // Windows keeps only whether a file is read-only: the owner's write bit.
  DWORD attributes = GetFileAttributesW(wide(&r->path));
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    fsp_fail_system(r, GetLastError(), "chmod", 1);
    return;
  }
  DWORD changed = r->mode & 0200 ? attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY
                                 : attributes | FILE_ATTRIBUTE_READONLY;
  if (changed != attributes && !SetFileAttributesW(wide(&r->path), changed))
    fsp_fail_system(r, GetLastError(), "chmod", 1);
#else
  if (chmod(native(&r->path), (mode_t)r->mode) != 0)
    fsp_fail_system(r, errno, "chmod", 1);
#endif
}

void tz_fsp_chmod(TzPromise *p, void *path_obj, uint64_t path_meta, int32_t mode, TzMaker make) {
  FsRequest *r = fsp_new(p, make);
  if (!fsp_path(r, path_obj, path_meta))
    return;
  r->mode = mode;
  fsp_queue(r, chmod_work, fsp_done_void);
}

static void truncate_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  OpenFlags flags = {1, 1, 0, 0, 0, 0}; // "r+"
  Failure failure;
  File f = open_raw(&r->path, flags, 0666, &failure);
  r->work.turns = 3; // open, ftruncate and close
  if (f == NO_FILE) {
    fsp_fail(r, failure, "open", 1);
    return;
  }
#ifdef _WIN32
  LARGE_INTEGER at;
  at.QuadPart = r->length < 0 ? 0 : r->length;
  if (!SetFilePointerEx(f, at, NULL, FILE_BEGIN) || !SetEndOfFile(f))
    fsp_fail_system(r, GetLastError(), "ftruncate", 0);
#else
  if (ftruncate(f, r->length < 0 ? 0 : r->length) != 0)
    fsp_fail_system(r, errno, "ftruncate", 0);
#endif
  close_file(f);
}

void tz_fsp_truncate(TzPromise *p, void *path_obj, uint64_t path_meta, int64_t length,
                     TzMaker make) {
  FsRequest *r = fsp_new(p, make);
  if (!fsp_path(r, path_obj, path_meta))
    return;
  r->length = length;
  fsp_queue(r, truncate_work, fsp_done_void);
}

static void stat_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  const char *syscall = r->follow ? "stat" : "lstat";
#ifdef _WIN32
  DWORD error = stat_of(wide(&r->path), r->follow, &r->st);
#else
  int error = stat_of(native(&r->path), r->follow, &r->st);
#endif
  if (error)
    fsp_fail_system(r, error, syscall, 1);
}

static void stat_done(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  if (!fsp_succeeded(r))
    return;
  void *stats = r->make_item(&r->st);
  fsp_fulfill(r, &stats, sizeof stats);
}

void tz_fsp_stat(TzPromise *p, void *path_obj, uint64_t path_meta, int32_t follow,
                 TzMaker make_stats, TzMaker make) {
  FsRequest *r = fsp_new(p, make);
  if (!fsp_path(r, path_obj, path_meta))
    return;
  r->follow = follow;
  r->make_item = make_stats;
  fsp_queue(r, stat_work, stat_done);
}

//===----------------------------------------------------------------------===//
// readdir
//===----------------------------------------------------------------------===//

// The entries of the directory at `path` (its system form, without a zero),
// each named in the directory numbered `dir`, as list_dir gives them; false
// with the failure of "scandir".
#ifdef _WIN32
static int list_raw(const uint16_t *path, size_t n, uint32_t dir, ListedList *out,
                    Failure *failure) {
  Raw pattern = {NULL, 0, 0};
  raw_units(&pattern, path, n);
  raw_unit(&pattern, 0);
  DWORD attributes = GetFileAttributesW((const wchar_t *)pattern.data);
  if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    *failure = attributes == INVALID_FILE_ATTRIBUTES
                   ? win32_failure(GetLastError())
                   : (Failure){E_ENOTDIR, kErrors[E_ENOTDIR].windows};
    raw_free(&pattern);
    return 0;
  }
  pattern.length -= 2;
  if (n == 0 || path[n - 1] != '\\')
    raw_unit(&pattern, '\\');
  raw_unit(&pattern, '*');
  raw_unit(&pattern, 0);
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileExW((const wchar_t *)pattern.data, FindExInfoBasic, &data,
                                 FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
  raw_free(&pattern);
  if (find == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_NO_MORE_FILES)
      return 1; // empty, as the root of an empty drive is
    *failure = win32_failure(error);
    return 0;
  }
  do {
    const wchar_t *name = data.cFileName;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
      continue;
    Listed *entry = listed_add(out);
    raw_units(&entry->name, (const uint16_t *)name, wcslen(name));
    DWORD a = data.dwFileAttributes;
    entry->kind = a & FILE_ATTRIBUTE_DEVICE          ? S_CHR
                  : a & FILE_ATTRIBUTE_REPARSE_POINT ? S_LNK
                  : a & FILE_ATTRIBUTE_DIRECTORY     ? S_DIR
                                                     : S_REG;
    entry->dir = dir;
  } while (FindNextFileW(find, &data));
  FindClose(find);
  return 1;
}
#else
static int compare_listed(const void *a, const void *b) {
  const Raw *x = &((const Listed *)a)->name, *y = &((const Listed *)b)->name;
  size_t n = x->length < y->length ? x->length : y->length;
  int c = n ? memcmp(x->data, y->data, n) : 0;
  return c ? c : x->length < y->length ? -1 : x->length > y->length;
}

static int list_raw(const uint8_t *path, size_t n, uint32_t dir, ListedList *out,
                    Failure *failure) {
  Raw native_path = {NULL, 0, 0};
  raw_append(&native_path, path, n);
  raw_append(&native_path, "", 1);
  DIR *d = opendir((const char *)native_path.data);
  raw_free(&native_path);
  if (!d) {
    *failure = errno_failure(errno);
    return 0;
  }
  size_t first = out->count;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0)))
      continue;
    Listed *entry = listed_add(out);
    raw_append(&entry->name, e->d_name, strlen(e->d_name));
    entry->dir = dir;
    switch (e->d_type) {
    case DT_REG: entry->kind = S_REG; break;
    case DT_DIR: entry->kind = S_DIR; break;
    case DT_LNK: entry->kind = S_LNK; break;
    case DT_CHR: entry->kind = S_CHR; break;
    case DT_BLK: entry->kind = S_BLK; break;
    case DT_FIFO: entry->kind = S_FIFO; break;
    case DT_SOCK: entry->kind = S_SOCK; break;
    default: entry->kind = 0; break;
    }
  }
  closedir(d);
  // Sorted by their bytes, as libuv's scandir sorts them.
  qsort(out->items + first, out->count - first, sizeof(Listed), compare_listed);
  return 1;
}
#endif

#ifdef _WIN32
#define SYSTEM_UNIT 2 // the bytes of a unit of a path of the system
#else
#define SYSTEM_UNIT 1
#endif

// A directory waiting to be visited: its number, its system form and its
// entries.
typedef struct {
  uint32_t dir;
  Raw path;
  ListedList entries;
} Pending;

// readdir of node:fs/promises: with `recursive`, as Node's readdirRecursive
// goes, which differs from readdirSync's: the entries of a directory, each
// directory among them listed when it is met, and then the directory listed
// last is visited (a stack, where readdirSync takes them level by level).
static void readdir_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  Raw root = {NULL, 0, 0};
#ifdef _WIN32
  raw_append(&root, r->path.wide.data, (r->path.wide.length - 1) * 2);
#else
  raw_append(&root, r->path.utf8.data, r->path.utf8.length - 1);
#endif
  r->dirs = (uint32_t *)malloc(sizeof(uint32_t) * 16);
  if (!r->dirs)
    abort();
  r->dir_capacity = 16;
  r->dirs[r->dir_count++] = UINT32_MAX;
  Pending *stack = (Pending *)malloc(sizeof(Pending) * 8);
  if (!stack)
    abort();
  size_t depth = 0, capacity = 8;
  Pending first = {0, root, {NULL, 0, 0}};
  Failure failure;
  if (!list_raw((const void *)root.data, root.length / SYSTEM_UNIT, 0, &first.entries, &failure)) {
    fsp_fail(r, failure, "scandir", 1);
    raw_free(&first.path);
    free(stack);
    return;
  }
  stack[depth++] = first;
  int failed = 0;
  while (depth > 0) {
    Pending at = stack[--depth];
    for (size_t i = 0; i < at.entries.count && !failed; ++i) {
      // The entry moves to the result.
      Listed *result = listed_add(&r->entries);
      *result = at.entries.items[i];
      memset(&at.entries.items[i], 0, sizeof(Listed));
      if (!r->recursive)
        continue;
      Raw child = {NULL, 0, 0};
      raw_append(&child, at.path.data, at.path.length);
#ifdef _WIN32
      if (raw_count(&child) == 0 || raw_wide(&child)[raw_count(&child) - 1] != '\\')
        raw_unit(&child, '\\');
#else
      if (child.length == 0 || child.data[child.length - 1] != '/')
        raw_append(&child, "/", 1);
#endif
      raw_append(&child, result->name.data, result->name.length);
      int descend;
      if (r->make_item) {
        descend = result->kind == S_DIR;
      } else {
        // A link to a directory too, as Node's internalModuleStat follows it.
        TzStatsParts st;
#ifdef _WIN32
        raw_unit(&child, 0);
        descend = stat_of((const wchar_t *)child.data, 1, &st) == 0 &&
                  ((uint32_t)st.mode & S_KIND) == S_DIR;
        child.length -= 2;
#else
        raw_append(&child, "", 1);
        descend = stat_of((const char *)child.data, 1, &st) == 0 &&
                  ((uint32_t)st.mode & S_KIND) == S_DIR;
        child.length -= 1;
#endif
      }
      if (!descend) {
        raw_free(&child);
        continue;
      }
      if (r->dir_count == r->dir_capacity) {
        r->dir_capacity *= 2;
        uint32_t *dirs = (uint32_t *)realloc(r->dirs, sizeof(uint32_t) * r->dir_capacity);
        if (!dirs)
          abort();
        r->dirs = dirs;
      }
      uint32_t number = (uint32_t)r->dir_count;
      r->dirs[r->dir_count++] = (uint32_t)(r->entries.count - 1);
      ++r->work.turns; // Node awaits a readdir for each
      Pending next = {number, child, {NULL, 0, 0}};
      if (!list_raw((const void *)child.data, child.length / SYSTEM_UNIT, number, &next.entries,
                    &failure)) {
        fsp_fail(r, failure, "scandir", 1);
        raw_append(&r->at, child.data, child.length);
        raw_free(&next.path);
        failed = 1;
        break;
      }
      if (depth == capacity) {
        capacity *= 2;
        Pending *bigger = (Pending *)realloc(stack, sizeof(Pending) * capacity);
        if (!bigger)
          abort();
        stack = bigger;
      }
      stack[depth++] = next;
    }
    listed_free(&at.entries);
    raw_free(&at.path);
    if (failed)
      break;
  }
  for (size_t i = 0; i < depth; ++i) {
    listed_free(&stack[i].entries);
    raw_free(&stack[i].path);
  }
  free(stack);
}

static TzStr str_of_system(const Raw *name) {
#ifdef _WIN32
  return tz_str_from_utf16(raw_wide(name), raw_count(name));
#else
  return str_of_utf8(name->data, name->length);
#endif
}

static void buf_system(TzBuf *out, const Raw *name) {
#ifdef _WIN32
  tz_buf_units(out, raw_wide(name), raw_count(name));
#else
  TzStr s = str_of_utf8(name->data, name->length);
  tz_buf_str(out, s);
  tz_str_release_value(s);
#endif
}

static void readdir_done(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  if (!fsp_succeeded(r))
    return;
  size_t count = r->entries.count;
  TzArray *result = tz_arr_new(r->element, 0, (int64_t)count);
  // Each directory as the program sees it (joined to the path given) for
  // Dirents, and relative to the one given for names.
  TzBuf *seen = (TzBuf *)tz_alloc(sizeof(TzBuf) * r->dir_count);
  TzBuf *relative = (TzBuf *)tz_alloc(sizeof(TzBuf) * r->dir_count);
  for (size_t d = 0; d < r->dir_count; ++d) {
    tz_buf_init(&seen[d]);
    tz_buf_init(&relative[d]);
    if (d == 0) {
      tz_buf_units(&seen[0], r->path.given.data, r->path.given.length);
      continue;
    }
    // Its parent was numbered before it.
    const Listed *entry = &r->entries.items[r->dirs[d]];
    TzBuf name;
    tz_buf_init(&name);
    buf_system(&name, &entry->name);
    const TzBuf *parent = &seen[entry->dir];
    tz_path_join_units(&seen[d], parent->data, parent->length, name.data, name.length);
    if (relative[entry->dir].length) {
      tz_buf_units(&relative[d], relative[entry->dir].data, relative[entry->dir].length);
      tz_buf_push(&relative[d], SEPARATOR);
    }
    tz_buf_units(&relative[d], name.data, name.length);
    tz_buf_free(&name);
  }
  for (size_t i = 0; i < count; ++i) {
    const Listed *e = &r->entries.items[i];
    if (r->make_item) {
      TzDirentParts parts;
      parts.name = str_of_system(&e->name);
      parts.parent_path = tz_str_from_utf16(seen[e->dir].data, seen[e->dir].length);
      parts.kind = (int32_t)e->kind;
      void *dirent = r->make_item(&parts);
      tz_arr_push_value(result, &dirent);
    } else {
      TzBuf name;
      tz_buf_init(&name);
      if (relative[e->dir].length) {
        tz_buf_units(&name, relative[e->dir].data, relative[e->dir].length);
        tz_buf_push(&name, SEPARATOR);
      }
      buf_system(&name, &e->name);
      TzStr s = tz_buf_take(&name);
      tz_arr_push_value(result, &s);
    }
  }
  for (size_t d = 0; d < r->dir_count; ++d) {
    tz_buf_free(&seen[d]);
    tz_buf_free(&relative[d]);
  }
  tz_free(seen);
  tz_free(relative);
  fsp_fulfill(r, &result, sizeof result);
}

void tz_fsp_readdir(TzPromise *p, void *path_obj, uint64_t path_meta, int32_t recursive,
                    const TzType *element, TzMaker make_dirent, TzMaker make) {
  FsRequest *r = fsp_new(p, make);
  if (!fsp_path(r, path_obj, path_meta))
    return;
  r->recursive = recursive;
  r->element = element;
  r->make_item = make_dirent;
  fsp_queue(r, readdir_work, readdir_done);
}

//===----------------------------------------------------------------------===//
// mkdir, mkdtemp, rmdir, unlink and rm
//===----------------------------------------------------------------------===//

static void mkdir_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  Failure failure;
#ifdef _WIN32
  r->made = mkdir_raw(r->path.wide.data, r->path.wide.length - 1, r->recursive, r->mode, &r->name,
                      &failure, &r->at);
#else
  r->made = mkdir_raw(r->path.utf8.data, r->path.utf8.length - 1, r->recursive, r->mode, &r->name,
                      &failure, &r->at);
#endif
  if (r->made < 0)
    fsp_fail(r, failure, "mkdir", 1);
  if (!r->recursive)
    return;
  // Node makes the directories a call at a time: one that fails for each
  // parent missing, then one for each made; or one and a stat when the
  // directory is there.
  size_t levels = 0;
  if (r->made > 0) {
#ifdef _WIN32
    const uint16_t *target = r->path.wide.data;
    size_t n = r->path.wide.length - 1, from = raw_count(&r->name);
    for (size_t i = from; i < n; ++i)
      levels += target[i] == '\\';
#else
    const uint8_t *target = r->path.utf8.data;
    size_t n = r->path.utf8.length - 1, from = r->name.length;
    for (size_t i = from; i < n; ++i)
      levels += target[i] == '/';
#endif
    ++levels;
  }
  r->work.turns = levels ? 2 * (uint32_t)levels - 1 : 2;
}

static void mkdir_done(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  if (!fsp_succeeded(r))
    return;
  // `string | undefined`: the first directory made, with `recursive`.
  TzOptStr first;
  memset(&first, 0, sizeof first);
  if (r->made) {
    first.value = made_dir(&r->name);
    first.has = 1;
  }
  fsp_fulfill(r, &first, sizeof first);
}

void tz_fsp_mkdir(TzPromise *p, void *path_obj, uint64_t path_meta, int32_t recursive,
                  int32_t mode, TzMaker make) {
  FsRequest *r = fsp_new(p, make);
  if (!fsp_path(r, path_obj, path_meta))
    return;
  r->recursive = recursive;
  r->mode = mode;
  fsp_queue(r, mkdir_work, mkdir_done);
}

// mkdtemp: the prefix and six random letters and digits, as given (not
// resolved). The name is made on the main thread, where paths are, and the
// thread tries it; one that is taken comes back for another.
static void mkdtemp_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
#ifdef _WIN32
  DWORD error = make_dir(r->dest.wide.data);
  r->made = error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS;
  if (error)
    fsp_fail_system(r, error, "mkdtemp", 1);
#else
  int error = mkdir(native(&r->dest), 0700) == 0 ? 0 : errno;
  r->made = error == EEXIST;
  if (error)
    fsp_fail_system(r, error, "mkdtemp", 1);
#endif
}

static void mkdtemp_try(FsRequest *r);

static void mkdtemp_done(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  if (r->failed && r->made && r->attempts < 100) {
    // Taken: another name.
    r->failed = 0;
    path_free(&r->dest);
    --r->paths;
    mkdtemp_try(r);
    return;
  }
  if (r->failed) {
    // The template, as Node shows it.
    TzBuf shown;
    tz_buf_init(&shown);
    tz_buf_units(&shown, r->path.given.data, r->path.given.length);
    tz_buf_ascii(&shown, "XXXXXX");
    throw_failure(r->make, r->failure, "mkdtemp", &shown, NULL);
    tz_buf_free(&shown);
    fsp_reject_thrown(r);
    return;
  }
  TzStr name = tz_str_from_utf16(r->dest.given.data, r->dest.given.length);
  fsp_fulfill(r, &name, sizeof name);
}

static void mkdtemp_try(FsRequest *r) {
  static const char letters[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  ++r->attempts;
  TzBuf name;
  tz_buf_init(&name);
  tz_buf_units(&name, r->path.given.data, r->path.given.length);
  for (int i = 0; i < 6; ++i) {
    unsigned int n;
#ifdef _WIN32
    if (rand_s(&n) != 0)
      n = (unsigned int)GetTickCount() * 2654435761u + (unsigned int)i;
#else
    n = (unsigned int)rand() ^ (unsigned int)time(NULL) ^ ((unsigned int)getpid() << 8);
#endif
    tz_buf_push(&name, (uint16_t)letters[n % 62]);
  }
  path_of_units(&r->dest, name.data, name.length);
  ++r->paths;
  tz_buf_free(&name);
  fsp_queue(r, mkdtemp_work, mkdtemp_done);
}

void tz_fsp_mkdtemp(TzPromise *p, void *prefix_obj, uint64_t prefix_meta, TzMaker make) {
  FsRequest *r = fsp_new(p, make);
  if (!fsp_path(r, prefix_obj, prefix_meta))
    return;
  mkdtemp_try(r);
}

static void rmdir_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
#ifdef _WIN32
  if (!RemoveDirectoryW(wide(&r->path)))
    fsp_fail_system(r, GetLastError(), "rmdir", 1);
#else
  if (rmdir(native(&r->path)) != 0)
    fsp_fail_system(r, errno, "rmdir", 1);
#endif
}

static void unlink_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
#ifdef _WIN32
  DWORD error = unlink_path(wide(&r->path));
  if (error)
    fsp_fail_system(r, error, "unlink", 1);
#else
  if (unlink(native(&r->path)) != 0)
    fsp_fail_system(r, errno, "unlink", 1);
#endif
}

// rmdir and unlink: `which` is 0 for rmdir.
void tz_fsp_remove(TzPromise *p, void *path_obj, uint64_t path_meta, int32_t which,
                   TzMaker make) {
  FsRequest *r = fsp_new(p, make);
  if (!fsp_path(r, path_obj, path_meta))
    return;
  fsp_queue(r, which ? unlink_work : rmdir_work, fsp_done_void);
}

static void rm_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  Failure failure;
  const char *syscall = NULL;
  int depth = 0;
  int found = rm_raw(&r->path, r->recursive, r->force, &failure, &syscall, &depth);
  if (found == RM_FAILED)
    fsp_fail(r, failure, syscall, 1);
  r->directory = found == RM_DIRECTORY;
  // Node checks with lstat, then its rimraf goes down the tree: lstat and
  // unlink for a file, lstat, rmdir, readdir and rmdir again for a
  // directory, each level after the one above it.
  r->work.turns = 3 + 4 * (uint32_t)depth;
}

static void rm_done(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  if (r->directory) {
    throw_rm_directory(&r->path, r->make);
    fsp_reject_thrown(r);
    return;
  }
  fsp_done_void(w);
}

void tz_fsp_rm(TzPromise *p, void *path_obj, uint64_t path_meta, int32_t recursive, int32_t force,
               TzMaker make) {
  FsRequest *r = fsp_new(p, make);
  if (!fsp_path(r, path_obj, path_meta))
    return;
  r->recursive = recursive;
  r->force = force;
  fsp_queue(r, rm_work, rm_done);
}

//===----------------------------------------------------------------------===//
// rename, copyFile and realpath
//===----------------------------------------------------------------------===//

static void rename_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
#ifdef _WIN32
  if (!MoveFileExW(wide(&r->path), wide(&r->dest), MOVEFILE_REPLACE_EXISTING))
    fsp_fail_system(r, GetLastError(), "rename", 2);
#else
  if (rename(native(&r->path), native(&r->dest)) != 0)
    fsp_fail_system(r, errno, "rename", 2);
#endif
}

void tz_fsp_rename(TzPromise *p, void *from_obj, uint64_t from_meta, void *to_obj,
                   uint64_t to_meta, TzMaker make) {
  FsRequest *r = fsp_new(p, make);
  if (!fsp_path(r, from_obj, from_meta) || !fsp_path(r, to_obj, to_meta))
    return;
  fsp_queue(r, rename_work, fsp_done_void);
}

static void copy_file_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
#ifdef _WIN32
  if (r->mode & 4) {
    fsp_fail(r, (Failure){E_ENOSYS, kErrors[E_ENOSYS].windows}, "copyfile", 2);
  } else if (!CopyFileW(wide(&r->path), wide(&r->dest), (r->mode & 1) != 0)) {
    DWORD error = GetLastError();
    // The same file: there is nothing to copy (libuv checks for EBUSY).
    TzStatsParts a, b;
    int same = win32_failure(error).index == E_EBUSY && stat_of(wide(&r->path), 1, &a) == 0 &&
               stat_of(wide(&r->dest), 1, &b) == 0 && a.dev == b.dev && a.ino == b.ino;
    if (!same)
      fsp_fail_system(r, error, "copyfile", 2);
  }
#else
  int in = -1, out = -1;
  if (r->mode & 4) {
    fsp_fail_system(r, ENOSYS, "copyfile", 2);
  } else if ((in = open(native(&r->path), O_RDONLY | O_CLOEXEC)) < 0) {
    fsp_fail_system(r, errno, "copyfile", 2);
  } else {
    struct stat s;
    fstat(in, &s);
    out = open(native(&r->dest),
               O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | ((r->mode & 1) ? O_EXCL : 0),
               s.st_mode & 0777);
    if (out < 0) {
      fsp_fail_system(r, errno, "copyfile", 2);
    } else {
      char chunk[65536];
      ssize_t got;
      while ((got = read(in, chunk, sizeof chunk)) > 0)
        if (write(out, chunk, (size_t)got) != got) {
          fsp_fail_system(r, errno, "copyfile", 2);
          break;
        }
      close(out);
    }
    close(in);
  }
#endif
}

void tz_fsp_copy_file(TzPromise *p, void *from_obj, uint64_t from_meta, void *to_obj,
                      uint64_t to_meta, int32_t mode, TzMaker make) {
  FsRequest *r = fsp_new(p, make);
  if (!fsp_path(r, from_obj, from_meta) || !fsp_path(r, to_obj, to_meta))
    return;
  r->mode = mode;
  fsp_queue(r, copy_file_work, fsp_done_void);
}

// realpath of node:fs/promises is libuv's, unlike realpathSync: the path
// the system has for the file, with its letters as they are on the disk on
// Windows.
static void realpath_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
#ifdef _WIN32
  HANDLE h = CreateFileW(wide(&r->path), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    fsp_fail_system(r, GetLastError(), "realpath", 1);
    return;
  }
  DWORD n = GetFinalPathNameByHandleW(h, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  Raw buffer = {NULL, 0, 0};
  raw_reserve(&buffer, ((size_t)n + 1) * 2);
  DWORD got = n ? GetFinalPathNameByHandleW(h, (wchar_t *)buffer.data, n + 1,
                                            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)
                : 0;
  DWORD error = got == 0 || got > n ? GetLastError() : 0;
  CloseHandle(h);
  if (error || got == 0 || got > n) {
    fsp_fail_system(r, error ? error : ERROR_INVALID_FUNCTION, "realpath", 1);
    raw_free(&buffer);
    return;
  }
  const uint16_t *s = (const uint16_t *)buffer.data;
  static const uint16_t unc[] = {'\\', '\\', '?', '\\', 'U', 'N', 'C', '\\'};
  if (got >= 8 && memcmp(s, unc, sizeof unc) == 0) {
    static const uint16_t two[] = {'\\', '\\'};
    raw_units(&r->name, two, 2);
    raw_units(&r->name, s + 8, got - 8);
  } else if (got >= 4 && s[0] == '\\' && s[1] == '\\' && s[2] == '?' && s[3] == '\\') {
    raw_units(&r->name, s + 4, got - 4);
  } else {
    raw_units(&r->name, s, got);
  }
  raw_free(&buffer);
#else
  char *real = realpath(native(&r->path), NULL);
  if (!real) {
    fsp_fail_system(r, errno, "realpath", 1);
    return;
  }
  raw_append(&r->name, real, strlen(real));
  free(real);
#endif
}

static void realpath_done(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  if (!fsp_succeeded(r))
    return;
  TzStr path = str_of_system(&r->name);
  fsp_fulfill(r, &path, sizeof path);
}

void tz_fsp_realpath(TzPromise *p, void *path_obj, uint64_t path_meta, TzMaker make) {
  FsRequest *r = fsp_new(p, make);
  if (!fsp_path(r, path_obj, path_meta))
    return;
  fsp_queue(r, realpath_work, realpath_done);
}

//===----------------------------------------------------------------------===//
// node:readline
//===----------------------------------------------------------------------===//

// An Interface of node:readline over stdin (docs/io.md, step 4). Its lines
// come from the buffer that readLine of tz:io reads (tz_in), as they are
// asked for, by a question or by a step of `for await`, in the order they
// are asked. What stdin does not have yet is read on a thread of the pool,
// a chunk at a time, while someone waits for a line; that read keeps the
// program alive. The lines end as Node's do: at "\n", "\r\n", a lone "\r",
// U+2028 or U+2029; the last line may end with the input.
//
// Unlike Node, which gives each line to whoever listens when its chunk
// comes and drops it if nobody does, a line waits here until it is asked
// for: `await rl.question(a)` after another question gets the next line of
// a file, as it does from a terminal. And an Interface nobody asks for
// lines reads nothing, so it does not keep the program waiting for the end
// of the input.

// A read of stdin on a thread of the pool: what it gave, in memory of its
// own. On Windows a console gives UTF-16, which the main thread puts
// together as readLine does.
typedef struct StdinRead {
  TzWork work;
  Raw data;
  int32_t ended;   // nothing more: the end of the input, or an error
  int32_t console; // `data` holds UTF-16 units of a console
} StdinRead;

// Not static: see the rules in tzrt.h. The read in flight, if any.
StdinRead *tz_stdin_read;

extern TzErrorInfo tz_error_info;

// Someone waiting for a line: a step of `for await` (a promise of the line,
// or done at the end) or a question (a promise of the line).
typedef struct Asker {
  struct Asker *next;
  TzPromise *promise; // held
  int32_t step;
  TzAbortListener listener; // of the question's signal
  TzSignal *signal;         // held
  struct Readline *readline;
} Asker;

typedef struct Readline {
  TzInstance base;
  int32_t output; // where questions go: 1 stdout, 2 stderr, 0 nowhere
  int32_t closed;
  int32_t skip_lf; // a line ended with "\r": a "\n" right after it goes with it
  TzSignal *signal; // of createInterface: an abort closes it
  TzAbortListener listener;
  Asker *head, *tail;
} Readline;

// All the interfaces there are, so that the reads find them.
Readline **tz_readlines;
size_t tz_readline_count, tz_readline_capacity;

static void readline_drop(void *object);
static void readline_step(void *object, TzPromise *p);
static void readline_close_step(void *object, TzPromise *p);

const TzAsyncIterLayout tz_readline_layout = {
    {readline_drop, NULL, 0, TZ_LAYOUT_ASYNC_ITERATOR, "Interface", 9, 0},
    readline_step,
    readline_close_step};

static void stdin_read_work(TzWork *w) {
  StdinRead *r = (StdinRead *)w;
#ifdef _WIN32
  HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode;
  if (h == NULL || h == INVALID_HANDLE_VALUE) {
    r->ended = 1;
    return;
  }
  if (GetConsoleMode(h, &mode)) {
    r->console = 1;
    raw_reserve(&r->data, 4096 * 2);
    DWORD got = 0;
    if (!ReadConsoleW(h, (wchar_t *)r->data.data, 4096, &got, NULL) || got == 0 ||
        ((const uint16_t *)r->data.data)[0] == 0x1A)
      r->ended = 1; // a line that starts with Ctrl+Z ends the input
    else
      r->data.length = (size_t)got * 2;
    return;
  }
  raw_reserve(&r->data, 65536);
  DWORD got = 0;
  if (!ReadFile(h, r->data.data, 65536, &got, NULL) || got == 0)
    r->ended = 1;
  else
    r->data.length = got;
#else
  raw_reserve(&r->data, 65536);
  for (;;) {
    ssize_t got = read(0, r->data.data, 65536);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0)
      r->ended = 1;
    else
      r->data.length = (size_t)got;
    return;
  }
#endif
}

static void readline_serve(Readline *rl);
static void stdin_serve_chunks(void);

static void stdin_read_done(TzWork *w) {
  StdinRead *r = (StdinRead *)w;
  tz_stdin_read = NULL;
  if (r->ended) {
    tz_in_eof = 1;
  } else if (r->console) {
#ifdef _WIN32
    // As in_fill: a pair of surrogates the console split waits for its half.
    const uint16_t *units = (const uint16_t *)r->data.data;
    size_t n = r->data.length / 2;
    uint16_t joined[4097];
    size_t k = 0;
    if (tz_in_surrogate)
      joined[k++] = tz_in_surrogate;
    tz_in_surrogate = 0;
    for (size_t i = 0; i < n; ++i)
      joined[k++] = units[i];
    if (k > 0 && joined[k - 1] >= 0xD800 && joined[k - 1] <= 0xDBFF)
      tz_in_surrogate = joined[--k];
    bytes_utf8(&tz_in, joined, k);
#endif
  } else {
    bytes_reserve(&tz_in, r->data.length);
    memcpy(tz_in.data + tz_in.length, r->data.data, r->data.length);
    tz_in.length += r->data.length;
  }
  raw_free(&r->data);
  free(r);
  // The interfaces take their lines, in the order they were made; then
  // process.stdin as a stream takes what is left.
  for (size_t i = 0; i < tz_readline_count; ++i)
    readline_serve(tz_readlines[i]);
  stdin_serve_chunks();
}

// Reads more of stdin, unless a read is in flight; with `ref`, it keeps the
// program alive.
static void stdin_read(int ref) {
  if (tz_stdin_read) {
    if (ref)
      tz_work_ref(&tz_stdin_read->work);
    return;
  }
  // What waits to be written goes first: it may be the question.
  tz_out_flush(1);
  StdinRead *r = (StdinRead *)calloc(1, sizeof(StdinRead));
  if (!r)
    abort();
  r->work.work = stdin_read_work;
  r->work.done = stdin_read_done;
  r->work.unref = !ref;
  tz_stdin_read = r;
  tz_work_queue(&r->work);
}

// The end of the next line in tz_in, from `start`: where it ends and where
// the next starts; false if it has not come yet.
static int line_end(size_t start, size_t *end, size_t *next) {
  const uint8_t *s = tz_in.data;
  size_t n = tz_in.length;
  for (size_t i = start; i < n; ++i) {
    if (s[i] == '\n') {
      *end = i;
      *next = i + 1;
      return 1;
    }
    if (s[i] == '\r') {
      *end = i;
      *next = i + 1 < n && s[i + 1] == '\n' ? i + 2 : i + 1;
      return 1;
    }
    if (s[i] == 0xE2 && i + 2 < n && s[i + 1] == 0x80 && (s[i + 2] == 0xA8 || s[i + 2] == 0xA9)) {
      *end = i;
      *next = i + 3;
      return 1;
    }
  }
  return 0;
}

// The next line, if it is there (or the rest at the end of the input):
// false otherwise.
static int take_line(Readline *rl, TzStr *line) {
  size_t start = 0;
  if (rl->skip_lf && tz_in.length > 0) {
    if (tz_in.data[0] == '\n')
      start = 1;
    rl->skip_lf = 0;
  }
  size_t end, next;
  if (line_end(start, &end, &next)) {
    *line = str_of_utf8(tz_in.data + start, end - start);
    rl->skip_lf = tz_in.data[end] == '\r' && next == end + 1;
    bytes_consume(&tz_in, next);
    return 1;
  }
  if (!tz_in_eof) {
    if (start)
      bytes_consume(&tz_in, start);
    return 0;
  }
  if (tz_in.length == start) {
    tz_in.length = 0;
    return 0;
  }
  *line = str_of_utf8(tz_in.data + start, tz_in.length - start);
  tz_in.length = 0;
  return 1;
}

static void asker_free(Asker *a) {
  tz_signal_unlisten(&a->listener);
  if (a->signal)
    tz_instance_release((TzInstance *)a->signal);
  tz_obj_release((TzObject *)a->promise);
  tz_free(a);
}

static void readline_unlink(Readline *rl, Asker *a) {
  Asker **link = &rl->head;
  Asker *prev = NULL;
  while (*link && *link != a) {
    prev = *link;
    link = &(*link)->next;
  }
  if (!*link)
    return;
  *link = a->next;
  if (rl->tail == a)
    rl->tail = prev;
  a->next = NULL;
}

// close(), the end of the input or an abort: the steps of `for await` are
// done; questions wait for nothing more, as in Node, where they never
// settle then.
static void readline_shut(Readline *rl) {
  if (rl->closed)
    return;
  rl->closed = 1;
  tz_signal_unlisten(&rl->listener);
  Asker **link = &rl->head;
  while (*link) {
    Asker *a = *link;
    if (!a->step) {
      link = &a->next;
      continue;
    }
    *link = a->next;
    if (rl->tail == a)
      rl->tail = NULL;
    a->promise->flags |= TZ_PROMISE_DONE;
    tz_promise_fulfill(a->promise);
    asker_free(a);
  }
  rl->tail = NULL;
  for (Asker *a = rl->head; a; a = a->next)
    rl->tail = a;
  // A read in flight no longer keeps the program alive for it.
  if (tz_stdin_read && !tz_stdin_read->work.unref) {
    int needed = 0;
    for (size_t i = 0; i < tz_readline_count && !needed; ++i)
      needed = tz_readlines[i] != rl && !tz_readlines[i]->closed && tz_readlines[i]->head;
    if (!needed)
      tz_work_unref(&tz_stdin_read->work);
  }
}

// Gives the lines there are to those who wait, then reads more if someone
// still does.
static void readline_serve(Readline *rl) {
  ++rl->base.rc;
  while (rl->head && !rl->closed) {
    TzStr line;
    if (!take_line(rl, &line)) {
      if (tz_in_eof)
        readline_shut(rl);
      else
        stdin_read(1);
      break;
    }
    Asker *a = rl->head;
    rl->head = a->next;
    if (!rl->head)
      rl->tail = NULL;
    *(TzStr *)((char *)a->promise + TZ_PROMISE_VALUE) = line;
    tz_promise_fulfill(a->promise);
    asker_free(a);
  }
  tz_instance_release(&rl->base);
}

static Asker *readline_ask(Readline *rl, TzPromise *p, int32_t step) {
  Asker *a = (Asker *)tz_alloc(sizeof(Asker));
  memset(a, 0, sizeof *a);
  a->promise = p;
  tz_ref_retain(p); // it may be a member of a group (async.c)
  a->step = step;
  a->readline = rl;
  if (rl->tail)
    rl->tail->next = a;
  else
    rl->head = a;
  rl->tail = a;
  return a;
}

static void readline_aborted(TzAbortListener *l) {
  readline_shut((Readline *)((char *)l - offsetof(Readline, listener)));
}

void *tz_readline_new(int32_t output, TzSignal *signal) {
  Readline *rl = (Readline *)tz_obj_alloc(sizeof(Readline));
  memset(rl, 0, sizeof *rl);
  rl->base.rc = 1;
  rl->base.layout = &tz_readline_layout.base;
  rl->output = output;
  if (tz_readline_count == tz_readline_capacity) {
    size_t capacity = tz_readline_capacity * 2 + 4;
    Readline **grown = (Readline **)tz_alloc(sizeof(Readline *) * capacity);
    if (tz_readline_count)
      memcpy(grown, tz_readlines, sizeof(Readline *) * tz_readline_count);
    tz_free(tz_readlines);
    tz_readlines = grown;
    tz_readline_capacity = capacity;
  }
  tz_readlines[tz_readline_count++] = rl;
  if (signal) {
    if (signal->aborted) {
      rl->closed = 1;
    } else {
      rl->signal = signal;
      tz_ref_retain(signal); // a member of a group (signal.c)
      rl->listener.run = readline_aborted;
      tz_signal_listen(signal, &rl->listener);
    }
  }
  return rl;
}

static void readline_drop(void *object) {
  // It leaves lists of the runtime: in node's order (parallel.c).
  TZ_ORDERED();
  Readline *rl = (Readline *)object;
  for (size_t i = 0; i < tz_readline_count; ++i)
    if (tz_readlines[i] == rl) {
      tz_readlines[i] = tz_readlines[--tz_readline_count];
      break;
    }
  if (tz_readline_count == 0) {
    tz_free(tz_readlines);
    tz_readlines = NULL;
    tz_readline_capacity = 0;
  }
  readline_shut(rl);
  while (rl->head) {
    Asker *a = rl->head;
    rl->head = a->next;
    asker_free(a);
  }
  if (rl->signal)
    tz_instance_release((TzInstance *)rl->signal);
  tz_obj_free(rl);
}

// A step of `for await (const line of rl)`.
static void readline_step(void *object, TzPromise *p) {
  Readline *rl = (Readline *)object;
  if (rl->closed) {
    p->flags |= TZ_PROMISE_DONE;
    tz_promise_fulfill(p);
    return;
  }
  readline_ask(rl, p, 1);
  readline_serve(rl);
}

// `for await` left early closes the interface, as in Node.
static void readline_close_step(void *object, TzPromise *p) {
  readline_shut((Readline *)object);
  tz_promise_fulfill(p);
}

void tz_readline_close(void *object) { readline_shut((Readline *)object); }

static void question_aborted(TzAbortListener *l) {
  Asker *a = (Asker *)((char *)l - offsetof(Asker, listener));
  Readline *rl = a->readline;
  readline_unlink(rl, a);
  tz_promise_reject(a->promise, tz_abort_error(a->signal));
  asker_free(a);
}

// rl.question(query, { signal }): the query is written to the output, then
// the next line answers it.
void tz_readline_question(void *object, TzPromise *p, void *query_obj, uint64_t query_meta,
                          TzSignal *signal) {
  Readline *rl = (Readline *)object;
  if (rl->closed) {
    TzStr message = ascii("readline was closed");
    void *error = tz_error_info.make(TZ_ERROR, message.obj, message.meta);
    tz_promise_reject(p, error);
    return;
  }
  if (signal && signal->aborted) {
    tz_promise_reject(p, tz_abort_error(signal));
    return;
  }
  if (rl->output == 1)
    tz_stdout_write(query_obj, query_meta);
  else if (rl->output == 2)
    tz_stderr_write(query_obj, query_meta);
  Asker *a = readline_ask(rl, p, 0);
  if (signal) {
    a->signal = signal;
    tz_ref_retain(signal); // a member of a group (signal.c)
    a->listener.run = question_aborted;
    tz_signal_listen(signal, &a->listener);
  }
  readline_serve(rl);
}

// Releases what the runtime holds, before the leak check.
void tz_fs_shutdown(void) { bytes_free(&tz_in); }

//===----------------------------------------------------------------------===//
// Descriptors for the streams of node:fs
//===----------------------------------------------------------------------===//

// fs.ReadStream and fs.WriteStream (src/lib/fs_stream.ts, docs/streams.md,
// step 3) open, read, write and close a file as Node's do: each call a work
// for the pool, as a call with a callback of node:fs is, the descriptor a
// CRT one, as libuv gives it on Windows. A read or a write holds its Buffer
// until it is back.

typedef struct FdRequest {
  TzWork work;
  TzPromise *promise; // held until settled
  TzMaker make;       // of SystemError
  Path path;
  int has_path;
  OpenFlags flags;
  int32_t mode;
  int32_t fd;
  TzBytes *bytes; // held
  int32_t offset, length;
  int64_t position; // -1: where the descriptor is
  int64_t done;     // what the call gave: a descriptor, or bytes moved
  int failed;
  Failure failure;
  const char *syscall;
} FdRequest;

static FdRequest *fd_new(TzPromise *p, TzMaker make) {
  FdRequest *r = (FdRequest *)tz_alloc(sizeof(FdRequest));
  memset(r, 0, sizeof *r);
  r->promise = p;
  tz_rc_inc(&p->rc);
  r->make = make;
  r->position = -1;
  return r;
}

static void fd_free(FdRequest *r) {
  if (r->has_path)
    path_free(&r->path);
  if (r->bytes)
    tz_instance_release((TzInstance *)r->bytes);
  tz_obj_release((TzObject *)r->promise);
  tz_free(r);
}

static void fd_queue(FdRequest *r, void (*work)(TzWork *), void (*done)(TzWork *)) {
  r->work.work = work;
  r->work.done = done;
  r->work.turns = 1;
  tz_work_queue(&r->work);
}

#ifdef _WIN32
static void fd_fail(FdRequest *r, DWORD error, const char *syscall) {
  r->failed = 1;
  r->failure = win32_failure(error);
  r->syscall = syscall;
}
#else
static void fd_fail(FdRequest *r, int error, const char *syscall) {
  r->failed = 1;
  r->failure = errno_failure(error);
  r->syscall = syscall;
}
#endif

// Settles the promise with what the call gave (an int), or rejects it with
// the SystemError, and frees the request.
static void fd_done_int(TzWork *w) {
  FdRequest *r = (FdRequest *)w;
  if (r->failed) {
    throw_failure(r->make, r->failure, r->syscall, r->has_path ? &r->path.shown : NULL, NULL);
    void *error = tz_thrown;
    tz_thrown = NULL;
    tz_promise_reject(r->promise, error);
  } else {
    int32_t value = (int32_t)r->done;
    memcpy((char *)r->promise + TZ_PROMISE_VALUE, &value, sizeof value);
    tz_promise_fulfill(r->promise);
  }
  fd_free(r);
}

static void fd_open_work(TzWork *w) {
  FdRequest *r = (FdRequest *)w;
  Failure failure;
  File f = open_raw(&r->path, r->flags, r->mode, &failure);
  if (f == NO_FILE) {
    r->failed = 1;
    r->failure = failure;
    r->syscall = "open";
    return;
  }
#ifdef _WIN32
  int crt = r->flags.read && r->flags.write ? _O_RDWR : r->flags.write ? _O_WRONLY : _O_RDONLY;
  if (r->flags.append)
    crt |= _O_APPEND;
  int fd = _open_osfhandle((intptr_t)f, crt | _O_BINARY);
  if (fd < 0) {
    CloseHandle(f);
    fd_fail(r, ERROR_TOO_MANY_OPEN_FILES, "open");
    return;
  }
  r->done = fd;
#else
  r->done = f;
#endif
}

// fs.open(path, flags, mode): a Promise of the descriptor.
void tz_fss_open(TzPromise *p, void *path_obj, uint64_t path_meta, void *flag_obj,
                 uint64_t flag_meta, int32_t mode, TzMaker make) {
  FdRequest *r = fd_new(p, make);
  if (!flags_of(flag_obj, flag_meta, &r->flags) || !path_init(&r->path, path_obj, path_meta)) {
    void *error = tz_thrown;
    tz_thrown = NULL;
    tz_promise_reject(r->promise, error);
    fd_free(r);
    return;
  }
  r->has_path = 1;
  r->mode = mode;
  fd_queue(r, fd_open_work, fd_done_int);
}

static void fd_read_work(TzWork *w) {
  FdRequest *r = (FdRequest *)w;
  uint8_t *at = r->bytes->data + r->offset;
#ifdef _WIN32
  HANDLE h = fd_file(r->fd);
  if (h == NO_FILE) {
    fd_fail(r, ERROR_INVALID_HANDLE, "read");
    return;
  }
  DWORD got = 0;
  OVERLAPPED at_position;
  memset(&at_position, 0, sizeof at_position);
  at_position.Offset = (DWORD)(r->position & 0xFFFFFFFF);
  at_position.OffsetHigh = (DWORD)(r->position >> 32);
  if (!ReadFile(h, at, (DWORD)r->length, &got, r->position >= 0 ? &at_position : NULL)) {
    DWORD error = GetLastError();
    if (error != ERROR_HANDLE_EOF && error != ERROR_BROKEN_PIPE)
      fd_fail(r, error, "read");
    got = 0;
  }
  r->done = got;
#else
  for (;;) {
    ssize_t got = r->position >= 0 ? pread(r->fd, at, (size_t)r->length, (off_t)r->position)
                                   : read(r->fd, at, (size_t)r->length);
    if (got < 0 && errno == EINTR)
      continue;
    if (got < 0)
      fd_fail(r, errno, "read");
    r->done = got < 0 ? 0 : got;
    return;
  }
#endif
}

static void fd_write_work(TzWork *w) {
  FdRequest *r = (FdRequest *)w;
  const uint8_t *at = r->bytes->data + r->offset;
#ifdef _WIN32
  HANDLE h = fd_file(r->fd);
  if (h == NO_FILE) {
    fd_fail(r, ERROR_INVALID_HANDLE, "write");
    return;
  }
  DWORD written = 0;
  OVERLAPPED at_position;
  memset(&at_position, 0, sizeof at_position);
  at_position.Offset = (DWORD)(r->position & 0xFFFFFFFF);
  at_position.OffsetHigh = (DWORD)(r->position >> 32);
  if (!WriteFile(h, at, (DWORD)r->length, &written, r->position >= 0 ? &at_position : NULL))
    fd_fail(r, GetLastError(), "write");
  r->done = written;
#else
  for (;;) {
    ssize_t written = r->position >= 0
                          ? pwrite(r->fd, at, (size_t)r->length, (off_t)r->position)
                          : write(r->fd, at, (size_t)r->length);
    if (written < 0 && errno == EINTR)
      continue;
    if (written < 0)
      fd_fail(r, errno, "write");
    r->done = written < 0 ? 0 : written;
    return;
  }
#endif
}

// fs.read(fd, buffer, offset, length, position) and fs.write(...): a
// Promise of the bytes moved; a position below 0 is where the descriptor is.
static void fd_transfer(TzPromise *p, int32_t fd, TzBytes *b, int32_t offset, int32_t length,
                        double position, TzMaker make, int writing) {
  FdRequest *r = fd_new(p, make);
  r->fd = fd;
  r->bytes = b;
  tz_rc_inc(&b->rc);
  if (offset < 0 || length < 0 || (int64_t)offset + length > b->length) {
    offset = 0;
    length = 0;
  }
  r->offset = offset;
  r->length = length;
  r->position = position >= 0 ? (int64_t)position : -1;
  fd_queue(r, writing ? fd_write_work : fd_read_work, fd_done_int);
}

void tz_fss_read(TzPromise *p, int32_t fd, TzBytes *b, int32_t offset, int32_t length,
                 double position, TzMaker make) {
  fd_transfer(p, fd, b, offset, length, position, make, 0);
}

void tz_fss_write(TzPromise *p, int32_t fd, TzBytes *b, int32_t offset, int32_t length,
                  double position, TzMaker make) {
  fd_transfer(p, fd, b, offset, length, position, make, 1);
}

static void fd_close_work(TzWork *w) {
  FdRequest *r = (FdRequest *)w;
#ifdef _WIN32
  if (fd_file(r->fd) == NO_FILE || _close(r->fd) != 0)
    fd_fail(r, ERROR_INVALID_HANDLE, "close");
#else
  if (close(r->fd) != 0)
    fd_fail(r, errno, "close");
#endif
}

static void fd_fsync_work(TzWork *w) {
  FdRequest *r = (FdRequest *)w;
#ifdef _WIN32
  HANDLE h = fd_file(r->fd);
  if (h == NO_FILE || !FlushFileBuffers(h))
    fd_fail(r, h == NO_FILE ? ERROR_INVALID_HANDLE : GetLastError(), "fsync");
#else
  if (fsync(r->fd) != 0)
    fd_fail(r, errno, "fsync");
#endif
}

// fs.close(fd) and fs.fsync(fd): a Promise of 0.
void tz_fss_close(TzPromise *p, int32_t fd, int32_t sync, TzMaker make) {
  FdRequest *r = fd_new(p, make);
  r->fd = fd;
  fd_queue(r, sync ? fd_fsync_work : fd_close_work, fd_done_int);
}

//===----------------------------------------------------------------------===//
// process.stdin as a stream
//===----------------------------------------------------------------------===//

// The reads of process.stdin as a Readable (src/lib/fs_stream.ts): each
// waits for the next chunk, at most `max` bytes, of the buffer that readLine
// and readline read too, or undefined at its end. Not static: see the rules
// in tzrt.h.
typedef struct ChunkWaiter {
  struct ChunkWaiter *next;
  TzPromise *promise; // held
  int32_t max;
} ChunkWaiter;

ChunkWaiter *tz_chunk_head, *tz_chunk_tail;

static void stdin_serve_chunks(void) {
  while (tz_chunk_head && (tz_in.length > 0 || tz_in_eof)) {
    ChunkWaiter *w = tz_chunk_head;
    tz_chunk_head = w->next;
    if (!tz_chunk_head)
      tz_chunk_tail = NULL;
    TzBytes *chunk = NULL; // undefined: the end
    if (tz_in.length > 0) {
      size_t n = tz_in.length < (size_t)w->max ? tz_in.length : (size_t)w->max;
      chunk = tz_buffer_copy_of(tz_in.data, (int64_t)n);
      bytes_consume(&tz_in, n);
    }
    memcpy((char *)w->promise + TZ_PROMISE_VALUE, &chunk, sizeof chunk);
    tz_promise_fulfill(w->promise);
    tz_obj_release((TzObject *)w->promise);
    free(w);
  }
  if (tz_chunk_head && !tz_in_eof)
    stdin_read(1);
}

// A Promise of the next chunk of stdin, a Buffer, or undefined at its end.
void tz_stdin_chunk(TzPromise *p, int32_t max) {
  ChunkWaiter *w = (ChunkWaiter *)calloc(1, sizeof(ChunkWaiter));
  if (!w)
    abort();
  w->promise = p;
  tz_rc_inc(&p->rc);
  w->max = max > 0 ? max : 65536;
  if (tz_chunk_tail)
    tz_chunk_tail->next = w;
  else
    tz_chunk_head = w;
  tz_chunk_tail = w;
  stdin_serve_chunks();
}
