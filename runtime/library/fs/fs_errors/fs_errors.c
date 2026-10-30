#ifdef _WIN32
#define _CRT_RAND_S
#else
#define _DEFAULT_SOURCE
#endif

#include "runtime/library/fs/fs_errors/fs_errors.h"
#include "runtime/core/error/error.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/fs/fs/fs.h"
#include <stdio.h>
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

const SysError tz_fs_errors[] = {
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
    {"ERR_INVALID_PATH", "the path has a zero character in it", -4071},
};

#ifdef _WIN32
Failure tz_fs_win32_failure(DWORD error) {
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
  case ERROR_INVALID_NAME:
    index = E_EINVAL;
    break;
  case ERROR_DIRECTORY:
    index = E_ENOTDIR;
    break;
  case ERROR_BAD_PATHNAME:
  case ERROR_ENVVAR_NOT_FOUND:
  case ERROR_FILE_NOT_FOUND:
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

  return (Failure){index, tz_fs_errors[index].windows, error};
}
#else
Failure tz_fs_errno_failure(int e) {
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

  return (Failure){index, -e, (uint32_t)e};
}
#endif

TzOptStr tz_fs_opt_of(const TzBuf *text) {
  TzOptStr o;
  memset(&o, 0, sizeof o);
  if (text) {
    o.value = tz_str_from_utf16(text->data, text->length);
    o.has = 1;
  }

  return o;
}

TzStr tz_fs_ascii(const char *text) { return tz_str_from_ascii(text, strlen(text)); }

void tz_fs_throw_parts(TzMaker make, TzSystemErrorParts *parts) {
  void *error = make(parts);
  tz_failed = error;
}

void tz_fs_throw_failure(TzMaker make, Failure f, const char *syscall, const TzBuf *path,
                         const TzBuf *dest) {
  TzFsErrorParts parts;
  memset(&parts, 0, sizeof parts);
  parts.code = tz_fs_errors[f.index].code;
  parts.syscall = tz_fs_ascii(syscall);
  parts.path = path ? tz_str_from_utf16(path->data, path->length) : tz_fs_ascii("");
  if (dest) {
    parts.dest = tz_str_from_utf16(dest->data, dest->length);
  }
  parts.index = f.index;
  parts.system = f.system;
  tz_failed = make(&parts);
}

void tz_fs_fd_name(TzBuf *out, int64_t fd) {
  if (fd == 0) {
    tz_buf_ascii(out, "<stdin>");
    return;
  }
  if (fd == 1) {
    tz_buf_ascii(out, "<stdout>");
    return;
  }
  if (fd == 2) {
    tz_buf_ascii(out, "<stderr>");
    return;
  }
  char number[32];
  snprintf(number, sizeof number, "<fd %lld>", (long long)fd);
  tz_buf_ascii(out, number);
}

enum { KIND_MISSING, KIND_FILE, KIND_FOLDER };

static int kind_of(const uint16_t *units, size_t n) {
  if (n == 0) {
    return KIND_MISSING;
  }
#ifdef _WIN32
  TzBuf wide;
  tz_buf_init(&wide);
  tz_buf_units(&wide, units, n);
  tz_buf_push(&wide, 0);
  DWORD attributes = GetFileAttributesW((const wchar_t *)wide.data);
  tz_buf_free(&wide);
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return KIND_MISSING;
  }

  return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? KIND_FOLDER : KIND_FILE;
#else
  Bytes utf8;
  tz_fs_bytes_init(&utf8);
  tz_fs_bytes_utf8(&utf8, units, n);
  tz_fs_bytes_append(&utf8, "", 1);
  struct stat st;
  int found = stat((const char *)utf8.data, &st) == 0;
  tz_fs_bytes_free(&utf8);
  if (!found) {
    return KIND_MISSING;
  }

  return S_ISDIR(st.st_mode) ? KIND_FOLDER : KIND_FILE;
#endif
}

static size_t parent_length(const uint16_t *units, size_t n) {
  while (n > 0 && units[n - 1] != '\\' && units[n - 1] != '/') {
    --n;
  }
  while (n > 1 && (units[n - 1] == '\\' || units[n - 1] == '/') && units[n - 2] != ':') {
    --n;
  }

  return n;
}

static void put_path(TzBuf *m, const TzBuf *path) {
  int plain = path->length > 0;
  for (size_t i = 0; i < path->length; ++i) {
    plain = plain && path->data[i] != 0;
  }
  if (plain) {
    tz_buf_units(m, path->data, path->length);
    return;
  }
  tz_buf_push(m, '"');
  for (size_t i = 0; i < path->length; ++i) {
    if (path->data[i] == 0) {
      tz_buf_ascii(m, "\\0");
    } else {
      tz_buf_push(m, path->data[i]);
    }
  }
  tz_buf_push(m, '"');
}

static void put_name(TzBuf *m, const TzBuf *path) {
  size_t start = parent_length(path->data, path->length);
  if (start > 0 && start < path->length &&
      (path->data[start] == '\\' || path->data[start] == '/')) {
    ++start;
  }
  if (start >= path->length) {
    start = 0;
  }
  tz_buf_units(m, path->data + start, path->length - start);
}

static int same(const char *a, const char *b) { return strcmp(a, b) == 0; }

static int missing_parent(TzBuf *m, const TzBuf *path, const char *advice) {
  size_t parent = parent_length(path->data, path->length);
  if (parent == 0 || kind_of(path->data, parent) != KIND_MISSING) {
    return 0;
  }
  tz_buf_ascii(m, "the folder ");
  tz_buf_units(m, path->data, parent);
  tz_buf_ascii(m, " does not exist");
  tz_buf_ascii(m, advice);

  return 1;
}

static void reason(TzBuf *m, const char *code, const char *verb, const TzBuf *path,
                   const TzBuf *dest) {
  int moves = same(verb, "move") || same(verb, "copy");
  int writes = same(verb, "write") || same(verb, "append to");
  if (same(code, "ENOENT")) {
    if (moves && dest->length > 0 && kind_of(path->data, path->length) != KIND_MISSING &&
        missing_parent(m, dest, "; create it with mkdirSync(path, { recursive: true })")) {
      return;
    }
    if (writes &&
        missing_parent(m, path, "; create it with mkdirSync(path, { recursive: true })")) {
      return;
    }
    if (same(verb, "create the folder") &&
        missing_parent(m, path, "; pass { recursive: true } to create it too")) {
      return;
    }
    if (same(verb, "create a temporary folder at") && missing_parent(m, path, "")) {
      return;
    }
    if (same(verb, "list") || same(verb, "remove the folder")) {
      tz_buf_ascii(m, "there is no such folder");
    } else if (same(verb, "read") || writes || moves || same(verb, "truncate")) {
      tz_buf_ascii(m, "there is no such file");
    } else {
      tz_buf_ascii(m, "there is no such file or folder");
    }
    return;
  }
  if (same(code, "EEXIST")) {
    if (same(verb, "move") && dest->length > 0) {
      put_name(m, dest);
      tz_buf_ascii(m, " already exists; to replace it on purpose, use replaceSync from \"tz:fs\"");
      return;
    }
    if (same(verb, "copy") && dest->length > 0) {
      put_name(m, dest);
      tz_buf_ascii(m, " already exists; to replace it on purpose, copy to a temporary name and "
                      "move that over it with replaceSync from \"tz:fs\"");
      return;
    }
    tz_buf_ascii(m, "it already exists");
    return;
  }
  if (same(code, "ENOTEMPTY")) {
    if (same(verb, "move")) {
      tz_buf_ascii(m, "the folder ");
      tz_buf_units(m, dest->data, dest->length);
      tz_buf_ascii(m, " is not empty");
      return;
    }
    if (same(verb, "remove")) {
      tz_buf_ascii(m, "a file appeared in the folder while it was being removed; try again");
      return;
    }
    tz_buf_ascii(m, "it is not empty; pass { recursive: true } to rmSync to remove it with its "
                    "contents");
    return;
  }
  if (same(code, "EISDIR")) {
    if (same(verb, "remove")) {
      tz_buf_ascii(m, "it is a folder; pass { recursive: true } to rmSync to remove it with its "
                      "contents");
      return;
    }
    if (same(verb, "move") && dest->length > 0) {
      tz_buf_units(m, dest->data, dest->length);
      tz_buf_ascii(m, " is a folder");
      return;
    }
    tz_buf_ascii(m, "it is a folder, not a file");
    return;
  }
  if (same(code, "ENOTDIR")) {
    if (kind_of(path->data, path->length) == KIND_FILE) {
      tz_buf_ascii(m, "it is a file, not a folder");
    } else {
      tz_buf_ascii(m, "a part of the path is a file, not a folder");
    }
    return;
  }
  if (same(code, "EACCES") || same(code, "EPERM")) {
    if (same(verb, "remove") && kind_of(path->data, path->length) == KIND_FOLDER) {
      tz_buf_ascii(m, "it is a folder; remove it with rmdirSync, or with its contents with "
                      "rmSync(path, { recursive: true })");
      return;
    }
    if (moves && dest->length > 0 && kind_of(dest->data, dest->length) == KIND_FOLDER) {
      tz_buf_units(m, dest->data, dest->length);
      tz_buf_ascii(m, " is a folder");
      return;
    }
    if (moves && kind_of(path->data, path->length) == KIND_FOLDER && same(verb, "copy")) {
      tz_buf_ascii(m, "it is a folder, not a file");
      return;
    }
    tz_buf_ascii(m, "no permission");
    return;
  }
  static const struct {
    const char *code;
    const char *text;
  } texts[] = {
      {"EROFS", "the disk is read-only"},
      {"EBUSY", "another program holds it open; close it and try again"},
      {"ETXTBSY", "the program is running; stop it and try again"},
      {"EAGAIN", "it has no data ready yet; try again"},
      {"EMFILE", "too many files are open; close some and try again"},
      {"ENFILE", "too many files are open; close some and try again"},
      {"ENOSPC", "the disk is full"},
      {"EFBIG", "the file would be larger than the disk allows"},
      {"EMLINK", "the folder has too many links"},
      {"ERR_INVALID_PATH", "the path has a zero character in it"},
      {"ENAMETOOLONG", "the path is too long"},
      {"ELOOP", "the links in the path go round in a loop"},
      {"EINVAL", "the name has characters the system does not allow, such as ? or *"},
      {"ECHARSET", "a name on the disk is not valid text"},
      {"EXDEV", "they are on different disks; copy it with copyFileSync and remove the original"},
      {"EIO", "the disk failed"},
      {"ENOMEM", "there is not enough memory"},
      {"ETIMEDOUT", "the disk did not answer in time"},
      {"EBADF", "it is not an open file"},
      {"EPIPE", "the reader is gone"},
      {"EOF", "the reader is gone"},
  };
  for (size_t i = 0; i < sizeof texts / sizeof texts[0]; ++i) {
    if (same(code, texts[i].code)) {
      tz_buf_ascii(m, texts[i].text);
      return;
    }
  }
  tz_buf_ascii(m, "the system failed");
}

void tz_fs_compose(TzFsErrorParts *parts, const char *verb, int32_t known) {
  TzBuf path, dest, m;
  tz_buf_init(&path);
  tz_buf_init(&dest);
  tz_buf_init(&m);
  tz_buf_str(&path, parts->path);
  if (parts->dest.obj) {
    tz_buf_str(&dest, parts->dest);
  }
  tz_buf_ascii(&m, "could not ");
  tz_buf_ascii(&m, verb);
  tz_buf_push(&m, ' ');
  put_path(&m, &path);
  if (dest.length > 0 && (same(verb, "move") || same(verb, "copy"))) {
    tz_buf_ascii(&m, " to ");
    put_path(&m, &dest);
  }
  tz_buf_ascii(&m, ": ");
  if (known) {
    reason(&m, parts->code, verb, &path, &dest);
  } else {
    const SysError *e = &tz_fs_errors[parts->index];
    char number[64];
    if (parts->index == E_UNKNOWN) {
#ifdef _WIN32
      snprintf(number, sizeof number, "the system failed with Windows error %lu",
               (unsigned long)parts->system);
#else
      snprintf(number, sizeof number, "the system failed with error %lu",
               (unsigned long)parts->system);
#endif
      tz_buf_ascii(&m, number);
    } else {
      tz_buf_ascii(&m, "the system failed with ");
      tz_buf_ascii(&m, e->code);
      tz_buf_ascii(&m, " (");
      tz_buf_ascii(&m, e->message);
      tz_buf_push(&m, ')');
    }
    parts->code = "UNKNOWN";
  }
  parts->message = tz_buf_take(&m);
  tz_buf_free(&path);
  tz_buf_free(&dest);
}
