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

  return (Failure){index, tz_fs_errors[index].windows};
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

  return (Failure){index, -e};
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

void tz_fs_abort_failure(TzMaker make) {
  TzSystemErrorParts parts;
  memset(&parts, 0, sizeof parts);
  parts.message = tz_fs_ascii("The operation was aborted");
  parts.name = tz_fs_ascii("AbortError");
  parts.code = tz_fs_ascii("ABORT_ERR");
  parts.syscall = tz_fs_ascii("");
  tz_fs_throw_parts(make, &parts);
}

void tz_fs_throw_failure(TzMaker make, Failure f, const char *syscall, const TzBuf *path,
                         const TzBuf *dest) {
  const SysError *e = &tz_fs_errors[f.index];
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
  parts.name = tz_fs_ascii("Error");
  parts.errno_value = f.number;
  parts.code = tz_fs_ascii(e->code);
  parts.syscall = tz_fs_ascii(syscall);
  parts.path = tz_fs_opt_of(path);
  parts.dest = tz_fs_opt_of(dest);
  tz_fs_throw_parts(make, &parts);
}
