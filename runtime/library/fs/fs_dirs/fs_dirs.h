// The synchronous functions of node:fs for directories: readdir, mkdir, rm and the like.

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

// mkdirSync and mkdir of node:fs/promises, the calls of the system only:
// makes the directory `path` names in the system's form (`n` units or bytes,
// without the zero), and with `recursive` the directories on the way too, as
// Node's MKDirpSync makes them. The first one made goes to `first` (UTF-16
// on Windows, UTF-8 elsewhere: as the system got it, which Node gives), and
// the result is 1 then, 0 for none; -1 with the failure of "mkdir", and in
// `at`, if given, the path that failed, which the async mkdir of Node shows.
#ifdef _WIN32
int tz_fs_mkdir_raw(const uint16_t *path, size_t n, int32_t recursive, int32_t mode, Raw *first,
                    Failure *failure, Raw *at);
#else
int tz_fs_mkdir_raw(const uint8_t *path, size_t n, int32_t recursive, int32_t mode, Raw *first,
                    Failure *failure, Raw *at);
#endif

// The first directory tz_fs_mkdir_raw made, as a string.
TzStr tz_fs_made_dir(const Raw *first);

#ifdef _WIN32
// Deletes a file, or the link a directory link is, as libuv's unlink does:
// a read-only file too, but not a directory (EPERM, as POSIX says). 0 or the
// error.
DWORD tz_fs_unlink_path(const wchar_t *path);
#endif

// What tz_fs_rm_raw found.
enum { RM_DONE, RM_FAILED, RM_DIRECTORY };

// rmSync and rm of node:fs/promises, the calls of the system only, as Node's:
// a path that is not there is an error ("lstat") unless `force`; a directory
// needs `recursive`, or it is RM_DIRECTORY (SystemError ERR_FS_EISDIR).
// `depth` gets the levels of directories removed.
int tz_fs_rm_raw(const Path *p, int32_t recursive, int32_t force, Failure *failure,
                 const char **syscall, int *depth);

// SystemError ERR_FS_EISDIR of rm, whose message shows the path as given.
void tz_fs_throw_rm_directory(const Path *p, TzMaker make);

#endif
