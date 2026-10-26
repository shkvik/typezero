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
  TzPromise *promise;
  TzMaker make;
  TzMaker make_item;
  const TzType *element;
  void *signal;
  Path path, dest;
  int paths;
  OpenFlags flags;
  int32_t mode, recursive, force, follow, attempts;
  int32_t encoding;
  int64_t length;
  Raw data;
  int failed;
  Failure failure;
  const char *syscall;
  int shows;
  int directory;
  Raw name;
  Raw at;
  int made;
  TzStatsParts st;
  ListedList entries;
  uint32_t *dirs;
  size_t dir_count, dir_capacity;
} FsRequest;

FsRequest *tz_fs_fsp_new(TzPromise *p, TzMaker make);

void tz_fs_fsp_reject_thrown(FsRequest *r);

int tz_fs_fsp_path(FsRequest *r, void *obj, uint64_t meta, const char *syscall);

void tz_fs_fsp_queue(FsRequest *r, void (*work)(TzWork *), void (*done)(TzWork *));

void tz_fs_fsp_fail(FsRequest *r, Failure failure, const char *syscall, int shows);

#ifdef _WIN32
void tz_fs_fsp_fail_system(FsRequest *r, DWORD error, const char *syscall, int shows);
#else
void tz_fs_fsp_fail_system(FsRequest *r, int error, const char *syscall, int shows);
#endif

int tz_fs_fsp_succeeded(FsRequest *r);

void tz_fs_fsp_fulfill(FsRequest *r, const void *value, size_t size);

void tz_fs_fsp_done_void(TzWork *w);

#endif
