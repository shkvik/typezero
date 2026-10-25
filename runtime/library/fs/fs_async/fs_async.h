// node:fs/promises (docs/io.md, step 4): the requests to the thread pool (io.c),
// and the functions for files.

#ifndef TZ_FS_ASYNC_H
#define TZ_FS_ASYNC_H

#include "runtime/library/fs/fs/fs.h"
#include "runtime/library/fs/fs_errors/fs_errors.h"
#include "runtime/core/async/async.h"
#include "runtime/core/io/io.h"
#include "runtime/core/tzrt/tzrt.h"
#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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

Listed *tz_fs_listed_add(ListedList *list);

void tz_fs_listed_free(ListedList *list);

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
  int shows;     // the paths the error shows: 0, 1 (path) or 2 (and dest)
  int directory; // rm of a directory without `recursive`
  Raw name;      // mkdir: the first made; mkdtemp: the name; realpath: the
                 // path
  Raw at;        // the path that failed, of mkdir and readdir: the system's form
  int made;
  TzStatsParts st;
  ListedList entries; // readdir, in the order Node gives them
  // readdir: each directory listed, by number, as the entry that names it in
  // its parent (UINT32_MAX for the one given).
  uint32_t *dirs;
  size_t dir_count, dir_capacity;
} FsRequest;

FsRequest *tz_fs_fsp_new(TzPromise *p, TzMaker make);

// Rejects the promise with the error in flight (of the arguments, or that
// tz_fs_throw_failure made), and frees the request.
void tz_fs_fsp_reject_thrown(FsRequest *r);

// The path at `obj` as the first (or second) path of the request; false with
// the promise rejected.
int tz_fs_fsp_path(FsRequest *r, void *obj, uint64_t meta);

void tz_fs_fsp_queue(FsRequest *r, void (*work)(TzWork *), void (*done)(TzWork *));

// The failure the thread found, of `syscall`, showing `shows` paths.
void tz_fs_fsp_fail(FsRequest *r, Failure failure, const char *syscall, int shows);

#ifdef _WIN32
void tz_fs_fsp_fail_system(FsRequest *r, DWORD error, const char *syscall, int shows);
#else
void tz_fs_fsp_fail_system(FsRequest *r, int error, const char *syscall, int shows);
#endif

// What the request gives once it is back: false after it settled the
// promise with its failure, or with an AbortError if its signal was
// aborted meanwhile, and freed it.
int tz_fs_fsp_succeeded(FsRequest *r);

// Fulfills the promise with the `size` bytes at `value` (which it takes
// over), or none, and frees the request.
void tz_fs_fsp_fulfill(FsRequest *r, const void *value, size_t size);

void tz_fs_fsp_done_void(TzWork *w);

#endif
