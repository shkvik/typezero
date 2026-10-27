#ifndef TZ_FS_H
#define TZ_FS_H

#include "runtime/library/fs/fs_errors/fs_errors.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef struct {
  double dev, mode, nlink, uid, gid, rdev, blksize, ino, size, blocks;
  double atime_ms, mtime_ms, ctime_ms, birthtime_ms;
} TzStatsParts;

typedef struct {
  TzStr name, parent_path;
  int64_t kind;
} TzDirentParts;
#endif

typedef struct {
  uint8_t *data;
  size_t length, capacity;
} Bytes;

void tz_fs_bytes_init(Bytes *b);

void tz_fs_bytes_free(Bytes *b);

void tz_fs_bytes_reserve(Bytes *b, size_t extra);

void tz_fs_bytes_append(Bytes *b, const void *data, size_t n);

void tz_fs_bytes_consume(Bytes *b, size_t n);

uint8_t *tz_fs_utf8_encode(uint8_t *p, const uint16_t *s, size_t n);

void tz_fs_bytes_utf8(Bytes *out, const uint16_t *s, size_t n);

void tz_fs_bytes_str(Bytes *out, TzStr s);

typedef struct {
  uint8_t *data;
  size_t length, capacity;
} Raw;

void tz_fs_raw_reserve(Raw *r, size_t extra);

void tz_fs_raw_append(Raw *r, const void *data, size_t n);

void tz_fs_raw_free(Raw *r);

#ifdef _WIN32
void tz_fs_raw_units(Raw *r, const uint16_t *units, size_t n);

void tz_fs_raw_unit(Raw *r, uint16_t unit);

const uint16_t *tz_fs_raw_wide(const Raw *r);

size_t tz_fs_raw_count(const Raw *r);
#endif

TzStr tz_fs_str_of_utf8(const uint8_t *s, size_t n);

typedef struct {
  TzBuf given;
  TzBuf shown;
#ifdef _WIN32
  TzBuf wide;
#else
  Bytes utf8;
#endif
} Path;

void tz_fs_path_free(Path *p);

int tz_fs_path_init(Path *p, void *obj, uint64_t meta, TzMaker make, const char *syscall);

void tz_fs_path_of_units(Path *p, const uint16_t *units, size_t n);

#ifdef _WIN32
const wchar_t *tz_fs_wide(const Path *p);

void tz_fs_fail_win32(TzMaker make, DWORD error, const char *syscall, const Path *path,
                      const Path *dest);

void tz_fs_fail_index(TzMaker make, int index, const char *syscall, const Path *path);
#else
const char *tz_fs_native(const Path *p);

void tz_fs_fail_errno(TzMaker make, int e, const char *syscall, const Path *path, const Path *dest);
#endif

typedef struct {
  int read, write, append, create, truncate, exclusive;
} OpenFlags;

int tz_fs_flags_of(void *obj, uint64_t meta, OpenFlags *f, TzMaker make);

#ifdef _WIN32
typedef HANDLE File;

#define NO_FILE INVALID_HANDLE_VALUE

File tz_fs_open_raw(const Path *p, OpenFlags f, int32_t mode, Failure *failure);

void tz_fs_close_file(File f);

int tz_fs_read_all(File f, Bytes *out, TzMaker make);

int tz_fs_write_all(File f, const uint8_t *data, size_t n, TzMaker make);

File tz_fs_fd_file(int32_t fd);

void tz_fs_fail_bad_fd(TzMaker make, const char *syscall);
#else
typedef int File;

#define NO_FILE (-1)

File tz_fs_open_raw(const Path *p, OpenFlags f, int32_t mode, Failure *failure);

void tz_fs_close_file(File f);

int tz_fs_read_all(File f, Bytes *out, TzMaker make);

int tz_fs_write_all(File f, const uint8_t *data, size_t n, TzMaker make);

File tz_fs_fd_file(int32_t fd);

void tz_fs_fail_bad_fd(TzMaker make, const char *syscall);
#endif

File tz_fs_open_file(const Path *p, OpenFlags f, int32_t mode, TzMaker make);

#endif
