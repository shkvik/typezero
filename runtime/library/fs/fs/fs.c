// node:fs (docs/io.md): what its functions share: byte buffers, paths as the
// program gave them and as the system gets them, the flags of open, and files.

#ifdef _WIN32
#define _CRT_RAND_S // rand_s
#else
#define _DEFAULT_SOURCE // lstat, realpath, d_type and the like, which -std=c17 hides
#endif

#include "runtime/library/fs/fs/fs.h"
#include "runtime/library/fs/fs_errors/fs_errors.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/error/error.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/path/path/path.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

//===----------------------------------------------------------------------===//
// Bytes
//===----------------------------------------------------------------------===//

void tz_fs_bytes_init(Bytes *b) {
  b->data = NULL;
  b->length = b->capacity = 0;
}

void tz_fs_bytes_free(Bytes *b) {
  tz_free(b->data);
  tz_fs_bytes_init(b);
}

void tz_fs_bytes_reserve(Bytes *b, size_t extra) {
  if (b->length + extra <= b->capacity) {
    return;
  }
  size_t capacity = b->capacity + b->capacity / 2 + 256;
  if (capacity < b->length + extra) {
    capacity = b->length + extra;
  }
  uint8_t *data = (uint8_t *)tz_alloc(capacity);
  if (b->length) {
    memcpy(data, b->data, b->length);
  }
  tz_free(b->data);
  b->data = data;
  b->capacity = capacity;
}

void tz_fs_bytes_append(Bytes *b, const void *data, size_t n) {
  tz_fs_bytes_reserve(b, n);
  memcpy(b->data + b->length, data, n);
  b->length += n;
}

void tz_fs_bytes_consume(Bytes *b, size_t n) {
  memmove(b->data, b->data + n, b->length - n);
  b->length -= n;
}

uint8_t *tz_fs_utf8_encode(uint8_t *p, const uint16_t *s, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    uint32_t c = s[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
    } else if (c >= 0xD800 && c <= 0xDFFF) {
      c = 0xFFFD;
    }
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

void tz_fs_bytes_utf8(Bytes *out, const uint16_t *s, size_t n) {
  tz_fs_bytes_reserve(out, n * 3);
  out->length = (size_t)(tz_fs_utf8_encode(out->data + out->length, s, n) - out->data);
}

void tz_fs_bytes_str(Bytes *out, TzStr s) {
  TzBuf units;
  tz_buf_init(&units);
  tz_buf_str(&units, s);
  tz_fs_bytes_utf8(out, units.data, units.length);
  tz_buf_free(&units);
}

void tz_fs_raw_reserve(Raw *r, size_t extra) {
  if (r->length + extra <= r->capacity) {
    return;
  }
  size_t capacity = r->capacity + r->capacity / 2 + 256;
  if (capacity < r->length + extra) {
    capacity = r->length + extra;
  }
  uint8_t *data = (uint8_t *)realloc(r->data, capacity);
  if (!data) {
    abort(); // on a thread of the pool, which cannot report it
  }
  r->data = data;
  r->capacity = capacity;
}

void tz_fs_raw_append(Raw *r, const void *data, size_t n) {
  tz_fs_raw_reserve(r, n);
  if (n) {
    memcpy(r->data + r->length, data, n);
  }
  r->length += n;
}

void tz_fs_raw_free(Raw *r) {
  free(r->data);
  r->data = NULL;
  r->length = r->capacity = 0;
}

#ifdef _WIN32
void tz_fs_raw_units(Raw *r, const uint16_t *units, size_t n) { tz_fs_raw_append(r, units, n * 2); }
void tz_fs_raw_unit(Raw *r, uint16_t unit) { tz_fs_raw_append(r, &unit, 2); }
const uint16_t *tz_fs_raw_wide(const Raw *r) { return (const uint16_t *)r->data; }
size_t tz_fs_raw_count(const Raw *r) { return r->length / 2; }
#endif

TzStr tz_fs_str_of_utf8(const uint8_t *s, size_t n) {
  size_t tz_fs_ascii = 0;
  while (tz_fs_ascii < n && s[tz_fs_ascii] < 0x80) {
    ++tz_fs_ascii;
  }
  if (tz_fs_ascii == n) {
    return tz_str_from_ascii((const char *)s, n);
  }
  TzBuf b;
  tz_buf_init(&b);
  tz_buf_bytes(&b, s, tz_fs_ascii);
  for (size_t i = tz_fs_ascii; i < n;) {
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
      if (lead == 0xE0) {
        lower = 0xA0;
      }
      if (lead == 0xED) {
        upper = 0x9F;
      }
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      need = 3;
      c = lead & 0x07;
      if (lead == 0xF0) {
        lower = 0x90;
      }
      if (lead == 0xF4) {
        upper = 0x8F;
      }
    } else {
      tz_buf_push(&b, 0xFFFD);
      ++i;
      continue;
    }
    size_t j = i + 1, k = 0;
    for (; k < need && j < n; ++k, ++j) {
      if (s[j] < lower || s[j] > upper) {
        break;
      }
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
// Paths
//===----------------------------------------------------------------------===//

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
  tz_fs_bytes_init(&p->utf8);
  tz_fs_bytes_utf8(&p->utf8, p->given.data, p->given.length);
  tz_fs_bytes_append(&p->utf8, "", 1);
#endif
}

void tz_fs_path_free(Path *p) {
  tz_buf_free(&p->given);
  tz_buf_free(&p->shown);
#ifdef _WIN32
  tz_buf_free(&p->wide);
#else
  tz_fs_bytes_free(&p->utf8);
#endif
}

int tz_fs_path_init(Path *p, void *obj, uint64_t meta) {
  tz_buf_init(&p->given);
  tz_buf_str(&p->given, (TzStr){obj, meta});
  for (size_t i = 0; i < p->given.length; ++i) {
    if (p->given.data[i] != 0) {
      continue;
    }
    TzBuf m;
    tz_buf_init(&m);
    tz_buf_ascii(&m, "The argument 'path' must be a string, Uint8Array, or URL without null bytes. "
                     "Received '");
    for (size_t k = 0; k < p->given.length; ++k) {
      if (p->given.data[k] == 0) {
        tz_buf_ascii(&m, "\\x00");
      } else {
        tz_buf_push(&m, p->given.data[k]);
      }
    }
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

void tz_fs_path_of_units(Path *p, const uint16_t *units, size_t n) {
  tz_buf_init(&p->given);
  tz_buf_units(&p->given, units, n);
  path_from_units(p);
}

#ifdef _WIN32
const wchar_t *tz_fs_wide(const Path *p) { return (const wchar_t *)p->wide.data; }

void tz_fs_fail_win32(TzMaker make, DWORD error, const char *syscall, const Path *path,
                      const Path *dest) {
  tz_fs_throw_failure(make, tz_fs_win32_failure(error), syscall, path ? &path->shown : NULL,
                      dest ? &dest->shown : NULL);
}

void tz_fs_fail_index(TzMaker make, int index, const char *syscall, const Path *path) {
  tz_fs_throw_failure(make, (Failure){index, tz_fs_errors[index].windows}, syscall,
                      path ? &path->shown : NULL, NULL);
}
#else
const char *tz_fs_native(const Path *p) { return (const char *)p->utf8.data; }

void tz_fs_fail_errno(TzMaker make, int e, const char *syscall, const Path *path,
                      const Path *dest) {
  tz_fs_throw_failure(make, tz_fs_errno_failure(e), syscall, path ? &path->shown : NULL,
                      dest ? &dest->shown : NULL);
}
#endif

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
    if (n != flag->length) {
      continue;
    }
    while (i < n && flag->data[i] == (uint8_t)table[k].text[i]) {
      ++i;
    }
    if (i == n) {
      *f = table[k].flags;
      return 1;
    }
  }

  return 0;
}

int tz_fs_flags_of(void *obj, uint64_t meta, OpenFlags *f) {
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

#ifdef _WIN32

File tz_fs_open_raw(const Path *p, OpenFlags f, int32_t mode, Failure *failure) {
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
  if (f.create && !(mode & 0200)) {
    attributes |= FILE_ATTRIBUTE_READONLY;
  }
  HANDLE h =
      CreateFileW(tz_fs_wide(p), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  NULL, disposition, attributes | FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    // To create a file where a directory is.
    if (error == ERROR_FILE_EXISTS && f.create && !f.exclusive) {
      *failure = (Failure){E_EISDIR, tz_fs_errors[E_EISDIR].windows};
    } else {
      *failure = tz_fs_win32_failure(error);
    }
  }

  return h;
}

void tz_fs_close_file(File f) { CloseHandle(f); }

int tz_fs_read_all(File f, Bytes *out, TzMaker make) {
  LARGE_INTEGER size;
  if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < ((int64_t)1 << 31)) {
    tz_fs_bytes_reserve(out, (size_t)size.QuadPart + 1);
  }
  for (;;) {
    tz_fs_bytes_reserve(out, 65536);
    DWORD got = 0;
    if (!ReadFile(f, out->data + out->length, 65536, &got, NULL)) {
      DWORD error = GetLastError();
      if (error == ERROR_HANDLE_EOF || error == ERROR_BROKEN_PIPE) {
        return 1;
      }
      tz_fs_fail_win32(make, error, "read", NULL, NULL);
      return 0;
    }
    if (got == 0) {
      return 1;
    }
    out->length += got;
  }
}

int tz_fs_write_all(File f, const uint8_t *data, size_t n, TzMaker make) {
  while (n > 0) {
    DWORD chunk = n > 0x40000000 ? 0x40000000 : (DWORD)n, written = 0;
    if (!WriteFile(f, data, chunk, &written, NULL)) {
      tz_fs_fail_win32(make, GetLastError(), "write", NULL, NULL);
      return 0;
    }
    data += written;
    n -= written;
  }

  return 1;
}

File tz_fs_fd_file(int32_t fd) {
  if (fd == 0) {
    return GetStdHandle(STD_INPUT_HANDLE);
  }
  intptr_t h = _get_osfhandle(fd);

  return h == -1 || h == -2 ? NO_FILE : (HANDLE)h;
}

void tz_fs_fail_bad_fd(TzMaker make, const char *syscall) {
  tz_fs_fail_index(make, E_EBADF, syscall, NULL);
}
#else

File tz_fs_open_raw(const Path *p, OpenFlags f, int32_t mode, Failure *failure) {
  int flags = f.read && f.write ? O_RDWR : f.write ? O_WRONLY : O_RDONLY;
  if (f.append) {
    flags |= O_APPEND;
  }
  if (f.create) {
    flags |= O_CREAT;
  }
  if (f.truncate) {
    flags |= O_TRUNC;
  }
  if (f.exclusive) {
    flags |= O_EXCL;
  }
  int fd = open(tz_fs_native(p), flags | O_CLOEXEC, mode);
  if (fd < 0) {
    *failure = tz_fs_errno_failure(errno);
  }

  return fd;
}

void tz_fs_close_file(File f) { close(f); }

int tz_fs_read_all(File f, Bytes *out, TzMaker make) {
  for (;;) {
    tz_fs_bytes_reserve(out, 65536);
    ssize_t got = read(f, out->data + out->length, 65536);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      tz_fs_fail_errno(make, errno, "read", NULL, NULL);
      return 0;
    }
    if (got == 0) {
      return 1;
    }
    out->length += (size_t)got;
  }
}

int tz_fs_write_all(File f, const uint8_t *data, size_t n, TzMaker make) {
  while (n > 0) {
    ssize_t written = write(f, data, n);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      tz_fs_fail_errno(make, errno, "write", NULL, NULL);
      return 0;
    }
    data += written;
    n -= (size_t)written;
  }

  return 1;
}

File tz_fs_fd_file(int32_t fd) { return fd; }

void tz_fs_fail_bad_fd(TzMaker make, const char *syscall) {
  tz_fs_throw_failure(make, tz_fs_errno_failure(EBADF), syscall, NULL, NULL);
}
#endif

File tz_fs_open_file(const Path *p, OpenFlags f, int32_t mode, TzMaker make) {
  Failure failure;
  File file = tz_fs_open_raw(p, f, mode, &failure);
  if (file == NO_FILE) {
    tz_fs_throw_failure(make, failure, "open", &p->shown, NULL);
  }

  return file;
}
