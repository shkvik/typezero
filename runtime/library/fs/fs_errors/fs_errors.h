#ifndef TZ_FS_ERRORS_H
#define TZ_FS_ERRORS_H

#include "runtime/core/error/error.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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

extern const SysError tz_fs_errors[];

typedef struct {
  int index;
  int32_t number;
} Failure;

#ifdef _WIN32
Failure tz_fs_win32_failure(DWORD error);
#else
Failure tz_fs_errno_failure(int e);
#endif

TzOptStr tz_fs_opt_of(const TzBuf *text);

TzStr tz_fs_ascii(const char *text);

void tz_fs_throw_parts(TzMaker make, TzSystemErrorParts *parts);

void tz_fs_throw_failure(TzMaker make, Failure f, const char *syscall, const TzBuf *path,
                         const TzBuf *dest);

#endif
