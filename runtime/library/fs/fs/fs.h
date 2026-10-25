// node:fs (docs/io.md): what its functions share: byte buffers, paths as the
// program gave them and as the system gets them, the flags of open, and files.

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

// Stats of statSync, in the order Node prints them.
typedef struct {
  double dev, mode, nlink, uid, gid, rdev, blksize, ino, size, blocks;
  double atime_ms, mtime_ms, ctime_ms, birthtime_ms;
} TzStatsParts;

// Dirent of readdirSync: the kind is the file type bits of a mode (S_IFREG).
typedef struct {
  TzStr name, parent_path;
  int32_t kind;
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

// Drops the first `n` bytes.
void tz_fs_bytes_consume(Bytes *b, size_t n);

// UTF-16 as UTF-8 into `p`, which has room for 3 bytes a unit; a lone
// surrogate becomes U+FFFD, as Buffer.from does. Returns the end.
uint8_t *tz_fs_utf8_encode(uint8_t *p, const uint16_t *s, size_t n);

void tz_fs_bytes_utf8(Bytes *out, const uint16_t *s, size_t n);

void tz_fs_bytes_str(Bytes *out, TzStr s);

// What the threads of the pool (io.c) write into: memory of malloc, which is
// thread-safe, as the allocator of the runtime is not. UTF-16 lies there as
// pairs of bytes.
typedef struct {
  uint8_t *data;
  size_t length, capacity;
} Raw;

void tz_fs_raw_reserve(Raw *r, size_t extra);

void tz_fs_raw_append(Raw *r, const void *data, size_t n);

void tz_fs_raw_free(Raw *r);

#ifdef _WIN32
// UTF-16 units of a Raw.
void tz_fs_raw_units(Raw *r, const uint16_t *units, size_t n);

void tz_fs_raw_unit(Raw *r, uint16_t unit);

const uint16_t *tz_fs_raw_wide(const Raw *r);

size_t tz_fs_raw_count(const Raw *r);
#endif

// UTF-8 as a string, as Node decodes it: each maximal part of a sequence
// that is not UTF-8 becomes one U+FFFD (the WHATWG decoder). A byte order
// mark stays, as in Node.
TzStr tz_fs_str_of_utf8(const uint8_t *s, size_t n);

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

void tz_fs_path_free(Path *p);

// A path of the program; false, with a TypeError thrown, if it holds a zero
// character, which would name another file.
int tz_fs_path_init(Path *p, void *obj, uint64_t meta);

// A path made by the runtime, in the form the program sees.
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

// The flags of `flag`, or a TypeError as Node throws it.
int tz_fs_flags_of(void *obj, uint64_t meta, OpenFlags *f);

#ifdef _WIN32
typedef HANDLE File;

#define NO_FILE INVALID_HANDLE_VALUE

// As libuv opens files: a directory opens too (and then fails to read), and
// a file created without write permission in `mode` is read-only. NO_FILE
// with the failure of "open"; the system calls only, for the pool too.
File tz_fs_open_raw(const Path *p, OpenFlags f, int32_t mode, Failure *failure);

void tz_fs_close_file(File f);

// Reads to the end; false with the error thrown ("read", without a path).
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
