#ifndef TZ_FS_DIRS_H
#define TZ_FS_DIRS_H

#include "runtime/library/fs/fs/fs.h"
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
#endif

#ifdef _WIN32
#define SEPARATOR '\\'
#else
#define SEPARATOR '/'
#endif

#ifdef _WIN32
DWORD tz_fs_make_dir(const uint16_t *path);
#endif

#ifdef _WIN32
int tz_fs_mkdir_raw(const uint16_t *path, size_t n, int32_t recursive, int32_t mode, Raw *first,
                    Failure *failure, Raw *at);
#else
int tz_fs_mkdir_raw(const uint8_t *path, size_t n, int32_t recursive, int32_t mode, Raw *first,
                    Failure *failure, Raw *at);
#endif

TzStr tz_fs_made_dir(const Raw *first);

#ifdef _WIN32
DWORD tz_fs_unlink_path(const wchar_t *path);
#endif

enum { RM_DONE, RM_FAILED, RM_DIRECTORY };

int tz_fs_rm_raw(const Path *p, int32_t recursive, int32_t force, Failure *failure,
                 const char **syscall, int *depth);

void tz_fs_throw_rm_directory(const Path *p, TzMaker make);

#endif
