#ifdef _WIN32
#define _CRT_RAND_S
#else
#define _DEFAULT_SOURCE
#endif

#include "runtime/library/fs/fs_sync/fs_sync.h"
#include "runtime/library/fs/fs/fs.h"
#include "runtime/library/fs/fs_errors/fs_errors.h"
#include "runtime/library/fs/stdin/stdin.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/group/group.h"
#include "runtime/core/output/output.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/bytes/buffer/buffer.h"
#include "runtime/library/bytes/bytes/bytes.h"
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

static int read_path(Bytes *data, void *path_obj, uint64_t path_meta, void *flag_obj,
                     uint64_t flag_meta, TzMaker make) {
  OpenFlags flags;
  if (!tz_fs_flags_of(flag_obj, flag_meta, &flags)) {
    return 0;
  }
  Path p;
  if (!tz_fs_path_init(&p, path_obj, path_meta, make, "open")) {
    return 0;
  }
  int ok = 0;
  File f = tz_fs_open_file(&p, flags, 0666, make);
  if (f != NO_FILE) {
    ok = tz_fs_read_all(f, data, make);
    tz_fs_close_file(f);
  }
  tz_fs_path_free(&p);

  return ok;
}

static int read_fd(Bytes *data, int32_t fd, TzMaker make) {
  if (fd == 0) {
    tz_fs_read_stdin(data);
    return 1;
  }
  File f = tz_fs_fd_file(fd);
  if (f == NO_FILE || fd < 0) {
    tz_fs_fail_bad_fd(make, "read");
    return 0;
  }

  return tz_fs_read_all(f, data, make);
}

static TzStr text_of(const Bytes *data, int32_t encoding) {
  if (encoding == 0) {
    return tz_fs_str_of_utf8(data->data, data->length);
  }

  return tz_buffer_decode(data->data, (int64_t)data->length, encoding);
}

void tz_fs_read_file(TzStr *out, void *path_obj, uint64_t path_meta, void *flag_obj,
                     uint64_t flag_meta, int32_t encoding, TzMaker make) {
  out->obj = NULL;
  out->meta = 0;
  Bytes data;
  tz_fs_bytes_init(&data);
  if (read_path(&data, path_obj, path_meta, flag_obj, flag_meta, make)) {
    *out = text_of(&data, encoding);
  }
  tz_fs_bytes_free(&data);
}

void tz_fs_read_fd(TzStr *out, int32_t fd, int32_t encoding, TzMaker make) {
  out->obj = NULL;
  out->meta = 0;
  Bytes data;
  tz_fs_bytes_init(&data);
  if (read_fd(&data, fd, make)) {
    *out = text_of(&data, encoding);
  }
  tz_fs_bytes_free(&data);
}

TzBytes *tz_fs_read_file_bytes(void *path_obj, uint64_t path_meta, void *flag_obj,
                               uint64_t flag_meta, TzMaker make) {
  Bytes data;
  tz_fs_bytes_init(&data);
  TzBytes *b = NULL;
  if (read_path(&data, path_obj, path_meta, flag_obj, flag_meta, make)) {
    b = tz_buffer_copy_of(data.data, (int64_t)data.length);
  }
  tz_fs_bytes_free(&data);

  return b;
}

TzBytes *tz_fs_read_fd_bytes(int32_t fd, TzMaker make) {
  Bytes data;
  tz_fs_bytes_init(&data);
  TzBytes *b = NULL;
  if (read_fd(&data, fd, make)) {
    b = tz_buffer_copy_of(data.data, (int64_t)data.length);
  }
  tz_fs_bytes_free(&data);

  return b;
}

static void write_path(void *path_obj, uint64_t path_meta, const uint8_t *data, size_t length,
                       void *flag_obj, uint64_t flag_meta, int32_t mode, TzMaker make) {
  OpenFlags flags;
  if (!tz_fs_flags_of(flag_obj, flag_meta, &flags)) {
    return;
  }
  Path p;
  if (!tz_fs_path_init(&p, path_obj, path_meta, make, "open")) {
    return;
  }
  File f = tz_fs_open_file(&p, flags, mode, make);
  if (f != NO_FILE) {
    tz_fs_write_all(f, data, length, make);
    tz_fs_close_file(f);
  }
  tz_fs_path_free(&p);
}

static void write_fd(int32_t fd, const uint8_t *data, size_t length, TzMaker make) {
  if (fd == 1 || fd == 2) {
    tz_std_write_bytes(fd, data, length);
    return;
  }
  File f = fd < 0 ? NO_FILE : tz_fs_fd_file(fd);
  if (f == NO_FILE) {
    tz_fs_fail_bad_fd(make, "write");
    return;
  }
  tz_fs_write_all(f, data, length, make);
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
  tz_fs_bytes_init(&data);
  tz_fs_bytes_str(&data, (TzStr){data_obj, data_meta});
  write_path(path_obj, path_meta, data.data, data.length, flag_obj, flag_meta, mode, make);
  tz_fs_bytes_free(&data);
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
  tz_fs_bytes_init(&data);
  tz_fs_bytes_str(&data, (TzStr){data_obj, data_meta});
  write_fd(fd, data.data, data.length, make);
  tz_fs_bytes_free(&data);
}

void tz_fs_write_file_bytes(void *path_obj, uint64_t path_meta, const TzBytes *b, void *flag_obj,
                            uint64_t flag_meta, int32_t mode, TzMaker make) {
  write_path(path_obj, path_meta, b->data, (size_t)tz_bytes_byte_length(b), flag_obj, flag_meta,
             mode, make);
}

void tz_fs_write_fd_bytes(int32_t fd, const TzBytes *b, TzMaker make) {
  write_fd(fd, b->data, (size_t)tz_bytes_byte_length(b), make);
}

int32_t tz_fs_exists(void *path_obj, uint64_t path_meta) {
  TzBuf given;
  tz_buf_init(&given);
  tz_buf_str(&given, (TzStr){path_obj, path_meta});
  int ok = given.length > 0;
  for (size_t i = 0; i < given.length && ok; ++i) {
    ok = given.data[i] != 0;
  }
  if (ok) {
    Path p;
    tz_fs_path_of_units(&p, given.data, given.length);
#ifdef _WIN32
    ok = GetFileAttributesW(tz_fs_wide(&p)) != INVALID_FILE_ATTRIBUTES;
#else
    ok = access(tz_fs_native(&p), F_OK) == 0;
#endif
    tz_fs_path_free(&p);
  }
  tz_buf_free(&given);

  return ok;
}

void tz_fs_access(void *path_obj, uint64_t path_meta, int32_t mode, TzMaker make) {
  Path p;
  if (!tz_fs_path_init(&p, path_obj, path_meta, make, "access")) {
    return;
  }
#ifdef _WIN32
  DWORD attributes = GetFileAttributesW(tz_fs_wide(&p));
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    tz_fs_fail_win32(make, GetLastError(), "access", &p, NULL);
  } else if ((mode & 2) && (attributes & FILE_ATTRIBUTE_READONLY) &&
             !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    tz_fs_fail_index(make, E_EPERM, "access", &p);
  }
#else
  if (access(tz_fs_native(&p), mode) != 0) {
    tz_fs_fail_errno(make, errno, "access", &p, NULL);
  }
#endif
  tz_fs_path_free(&p);
}

void tz_fs_chmod(void *path_obj, uint64_t path_meta, int32_t mode, TzMaker make) {
  Path p;
  if (!tz_fs_path_init(&p, path_obj, path_meta, make, "chmod")) {
    return;
  }
#ifdef _WIN32
  DWORD attributes = GetFileAttributesW(tz_fs_wide(&p));
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    tz_fs_fail_win32(make, GetLastError(), "chmod", &p, NULL);
  } else {
    DWORD changed = mode & 0200 ? attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY
                                : attributes | FILE_ATTRIBUTE_READONLY;
    if (changed != attributes && !SetFileAttributesW(tz_fs_wide(&p), changed)) {
      tz_fs_fail_win32(make, GetLastError(), "chmod", &p, NULL);
    }
  }
#else
  if (chmod(tz_fs_native(&p), (mode_t)mode) != 0) {
    tz_fs_fail_errno(make, errno, "chmod", &p, NULL);
  }
#endif
  tz_fs_path_free(&p);
}

void tz_fs_truncate(void *path_obj, uint64_t path_meta, int64_t length, TzMaker make) {
  Path p;
  if (!tz_fs_path_init(&p, path_obj, path_meta, make, "open")) {
    return;
  }
  OpenFlags flags = {1, 1, 0, 0, 0, 0};
  File f = tz_fs_open_file(&p, flags, 0666, make);
  if (f != NO_FILE) {
#ifdef _WIN32
    LARGE_INTEGER at;
    at.QuadPart = length < 0 ? 0 : length;
    if (!SetFilePointerEx(f, at, NULL, FILE_BEGIN) || !SetEndOfFile(f)) {
      tz_fs_fail_win32(make, GetLastError(), "ftruncate", NULL, NULL);
    }
#else
    if (ftruncate(f, length < 0 ? 0 : length) != 0) {
      tz_fs_fail_errno(make, errno, "ftruncate", NULL, NULL);
    }
#endif
    tz_fs_close_file(f);
  }
  tz_fs_path_free(&p);
}

#ifdef _WIN32
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
  if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof tag)) {
    return 0;
  }

  return tag.ReparseTag == IO_REPARSE_TAG_SYMLINK || tag.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT;
}

DWORD tz_fs_stat_of(const wchar_t *path, int follow, TzStatsParts *st) {
  HANDLE h =
      CreateFileW(path, FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                  FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT), NULL);
  if (h == INVALID_HANDLE_VALUE) {
    return GetLastError();
  }
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

int tz_fs_stat_of(const char *path, int follow, TzStatsParts *st) {
  struct stat s;
  if ((follow ? stat(path, &s) : lstat(path, &s)) != 0) {
    return errno;
  }
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

void *tz_fs_stat(void *path_obj, uint64_t path_meta, int32_t follow, int32_t throw_if_missing,
                 TzMaker make_stats, TzMaker make) {
  Path p;
  if (!tz_fs_path_init(&p, path_obj, path_meta, make, follow ? "stat" : "lstat")) {
    return NULL;
  }
  TzStatsParts st;
  void *result = NULL;
  const char *syscall = follow ? "stat" : "lstat";
#ifdef _WIN32
  DWORD error = tz_fs_stat_of(tz_fs_wide(&p), follow, &st);
  if (error) {
    Failure f = tz_fs_win32_failure(error);
    if (throw_if_missing || (f.index != E_ENOENT && f.index != E_ENOTDIR)) {
      tz_fs_fail_win32(make, error, syscall, &p, NULL);
    }
  } else {
    result = make_stats(&st);
  }
#else
  int error = tz_fs_stat_of(tz_fs_native(&p), follow, &st);
  if (error) {
    if (throw_if_missing || (error != ENOENT && error != ENOTDIR)) {
      tz_fs_fail_errno(make, error, syscall, &p, NULL);
    }
  } else {
    result = make_stats(&st);
  }
#endif
  tz_fs_path_free(&p);

  return result;
}

void tz_fs_rename(void *from_obj, uint64_t from_meta, void *to_obj, uint64_t to_meta,
                  TzMaker make) {
  Path from, to;
  if (!tz_fs_path_init(&from, from_obj, from_meta, make, "rename")) {
    return;
  }
  if (!tz_fs_path_init(&to, to_obj, to_meta, make, "rename")) {
    tz_fs_path_free(&from);
    return;
  }
#ifdef _WIN32
  if (!MoveFileExW(tz_fs_wide(&from), tz_fs_wide(&to), MOVEFILE_REPLACE_EXISTING)) {
    tz_fs_fail_win32(make, GetLastError(), "rename", &from, &to);
  }
#else
  if (rename(tz_fs_native(&from), tz_fs_native(&to)) != 0) {
    tz_fs_fail_errno(make, errno, "rename", &from, &to);
  }
#endif
  tz_fs_path_free(&from);
  tz_fs_path_free(&to);
}

void tz_fs_copy_file(void *from_obj, uint64_t from_meta, void *to_obj, uint64_t to_meta,
                     int32_t mode, TzMaker make) {
  Path from, to;
  if (!tz_fs_path_init(&from, from_obj, from_meta, make, "copyfile")) {
    return;
  }
  if (!tz_fs_path_init(&to, to_obj, to_meta, make, "copyfile")) {
    tz_fs_path_free(&from);
    return;
  }
#ifdef _WIN32
  if (mode & 4) {
    tz_fs_throw_failure(make, (Failure){E_ENOSYS, tz_fs_errors[E_ENOSYS].windows}, "copyfile",
                        &from.shown, &to.shown);
  } else if (!CopyFileW(tz_fs_wide(&from), tz_fs_wide(&to), (mode & 1) != 0)) {
    DWORD error = GetLastError();
    TzStatsParts a, b;
    int same = tz_fs_win32_failure(error).index == E_EBUSY &&
               tz_fs_stat_of(tz_fs_wide(&from), 1, &a) == 0 &&
               tz_fs_stat_of(tz_fs_wide(&to), 1, &b) == 0 && a.dev == b.dev && a.ino == b.ino;
    if (!same) {
      tz_fs_fail_win32(make, error, "copyfile", &from, &to);
    }
  }
#else
  int in = -1, out = -1;
  if (mode & 4) {
    tz_fs_fail_errno(make, ENOSYS, "copyfile", &from, &to);
  } else if ((in = open(tz_fs_native(&from), O_RDONLY | O_CLOEXEC)) < 0) {
    tz_fs_fail_errno(make, errno, "copyfile", &from, &to);
  } else {
    struct stat s;
    fstat(in, &s);
    out = open(tz_fs_native(&to),
               O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | ((mode & 1) ? O_EXCL : 0),
               s.st_mode & 0777);
    if (out < 0) {
      tz_fs_fail_errno(make, errno, "copyfile", &from, &to);
    } else {
      char chunk[65536];
      ssize_t got;
      while ((got = read(in, chunk, sizeof chunk)) > 0) {
        if (write(out, chunk, (size_t)got) != got) {
          tz_fs_fail_errno(make, errno, "copyfile", &from, &to);
          break;
        }
      }
      close(out);
    }
    close(in);
  }
#endif
  tz_fs_path_free(&from);
  tz_fs_path_free(&to);
}

#ifdef _WIN32
static int final_path(const Path *p, TzBuf *out) {
  HANDLE h = CreateFileW(tz_fs_wide(p), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    return 0;
  }
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
  if (buffer != small) {
    tz_free(buffer);
  }

  return ok;
}

static size_t root_length(const TzBuf *p) {
  const uint16_t *s = p->data;
  size_t n = p->length, i = 0;
  if (n >= 2 && s[1] == ':') {
    i = 2;
  } else if (n >= 2 && (s[0] == '\\' || s[0] == '/') && (s[1] == '\\' || s[1] == '/')) {
    size_t j = 2;
    while (j < n && s[j] != '\\' && s[j] != '/') {
      ++j;
    }
    if (j > 2 && j < n) {
      size_t k = j + 1;
      while (k < n && s[k] != '\\' && s[k] != '/') {
        ++k;
      }
      if (k > j + 1) {
        i = k;
      }
    }
  }
  while (i < n && (s[i] == '\\' || s[i] == '/')) {
    ++i;
  }

  return i;
}
#endif

void tz_fs_realpath(TzStr *out, void *path_obj, uint64_t path_meta, TzMaker make) {
  out->obj = NULL;
  out->meta = 0;
  Path given;
  if (!tz_fs_path_init(&given, path_obj, path_meta, make, "realpath")) {
    return;
  }
#ifdef _WIN32
  TzBuf p;
  tz_buf_init(&p);
  tz_path_resolve_units(&p, given.given.data, given.given.length);
  int ok = 1;
  for (int round = 0; ok; ++round) {
    if (round > 64) {
      Path at;
      tz_fs_path_of_units(&at, p.data, p.length);
      tz_fs_fail_index(make, E_ELOOP, "lstat", &at);
      tz_fs_path_free(&at);
      ok = 0;
      break;
    }
    size_t pos = root_length(&p);
    int restarted = 0;
    for (size_t end = pos;; ++end) {
      if (end < p.length && p.data[end] != '\\' && p.data[end] != '/') {
        continue;
      }
      Path at;
      tz_fs_path_of_units(&at, p.data, end);
      DWORD attributes = GetFileAttributesW(tz_fs_wide(&at));
      if (attributes == INVALID_FILE_ATTRIBUTES) {
        tz_fs_fail_win32(make, GetLastError(), "lstat", &at, NULL);
        tz_fs_path_free(&at);
        ok = 0;
        break;
      }
      if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        TzBuf target;
        tz_buf_init(&target);
        if (final_path(&at, &target)) {
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
      tz_fs_path_free(&at);
      if (restarted || end >= p.length) {
        break;
      }
    }
    if (!restarted) {
      break;
    }
  }
  if (ok) {
    *out = tz_buf_take(&p);
  } else {
    tz_buf_free(&p);
  }
#else
  char *real = realpath(tz_fs_native(&given), NULL);
  if (!real) {
    tz_fs_fail_errno(make, errno, "lstat", &given, NULL);
  } else {
    *out = tz_fs_str_of_utf8((const uint8_t *)real, strlen(real));
    free(real);
  }
#endif
  tz_fs_path_free(&given);
}
