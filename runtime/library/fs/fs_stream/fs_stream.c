#ifdef _WIN32
#define _CRT_RAND_S
#else
#define _DEFAULT_SOURCE
#endif

#include "runtime/library/fs/fs/fs.h"
#include "runtime/library/fs/fs_errors/fs_errors.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/async/async.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/group/group.h"
#include "runtime/core/io/io.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/bytes/bytes/bytes.h"
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

typedef struct FdRequest {
  TzWork work;
  TzPromise *promise;
  TzMaker make;
  Path path;
  int has_path;
  OpenFlags flags;
  int32_t mode;
  int32_t fd;
  TzBytes *bytes;
  int32_t offset, length;
  int64_t position;
  int64_t done;
  int failed;
  Failure failure;
  const char *syscall;
} FdRequest;

static FdRequest *fd_new(TzPromise *p, TzMaker make) {
  FdRequest *r = (FdRequest *)tz_alloc(sizeof(FdRequest));
  memset(r, 0, sizeof *r);
  r->promise = p;
  tz_rc_inc(&p->rc);
  r->make = make;
  r->position = -1;

  return r;
}

static void fd_free(FdRequest *r) {
  if (r->has_path) {
    tz_fs_path_free(&r->path);
  }
  if (r->bytes) {
    tz_instance_release((TzInstance *)r->bytes);
  }
  tz_obj_release((TzObject *)r->promise);
  tz_free(r);
}

static void fd_queue(FdRequest *r, void (*work)(TzWork *), void (*done)(TzWork *)) {
  r->work.work = work;
  r->work.done = done;
  r->work.turns = 1;
  tz_work_queue(&r->work);
}

#ifdef _WIN32
static void fd_fail(FdRequest *r, DWORD error, const char *syscall) {
  r->failed = 1;
  r->failure = tz_fs_win32_failure(error);
  r->syscall = syscall;
}
#else
static void fd_fail(FdRequest *r, int error, const char *syscall) {
  r->failed = 1;
  r->failure = tz_fs_errno_failure(error);
  r->syscall = syscall;
}
#endif

static void fd_done_int(TzWork *w) {
  FdRequest *r = (FdRequest *)w;
  if (r->failed) {
    tz_fs_throw_failure(r->make, r->failure, r->syscall, r->has_path ? &r->path.shown : NULL, NULL);
    void *error = tz_failed;
    tz_failed = NULL;
    tz_promise_reject(r->promise, error);
  } else {
    int32_t value = (int32_t)r->done;
    memcpy((char *)r->promise + TZ_PROMISE_VALUE, &value, sizeof value);
    tz_promise_fulfill(r->promise);
  }
  fd_free(r);
}

static void fd_open_work(TzWork *w) {
  FdRequest *r = (FdRequest *)w;
  Failure failure;
  File f = tz_fs_open_raw(&r->path, r->flags, r->mode, &failure);
  if (f == NO_FILE) {
    r->failed = 1;
    r->failure = failure;
    r->syscall = "open";
    return;
  }
#ifdef _WIN32
  int crt = r->flags.read && r->flags.write ? _O_RDWR : r->flags.write ? _O_WRONLY : _O_RDONLY;
  if (r->flags.append) {
    crt |= _O_APPEND;
  }
  int fd = _open_osfhandle((intptr_t)f, crt | _O_BINARY);
  if (fd < 0) {
    CloseHandle(f);
    fd_fail(r, ERROR_TOO_MANY_OPEN_FILES, "open");
    return;
  }
  r->done = fd;
#else
  r->done = f;
#endif
}

void tz_fss_open(TzPromise *p, void *path_obj, uint64_t path_meta, void *flag_obj,
                 uint64_t flag_meta, int32_t mode, TzMaker make) {
  FdRequest *r = fd_new(p, make);
  if (!tz_fs_flags_of(flag_obj, flag_meta, &r->flags, make) ||
      !tz_fs_path_init(&r->path, path_obj, path_meta, make, "open")) {
    void *error = tz_failed ? tz_failed : tz_thrown;
    tz_failed = NULL;
    tz_thrown = NULL;
    tz_promise_reject(r->promise, error);
    fd_free(r);
    return;
  }
  r->has_path = 1;
  r->mode = mode;
  fd_queue(r, fd_open_work, fd_done_int);
}

static void fd_read_work(TzWork *w) {
  FdRequest *r = (FdRequest *)w;
  uint8_t *at = r->bytes->data + r->offset;
#ifdef _WIN32
  HANDLE h = tz_fs_fd_file(r->fd);
  if (h == NO_FILE) {
    fd_fail(r, ERROR_INVALID_HANDLE, "read");
    return;
  }
  DWORD got = 0;
  OVERLAPPED at_position;
  memset(&at_position, 0, sizeof at_position);
  at_position.Offset = (DWORD)(r->position & 0xFFFFFFFF);
  at_position.OffsetHigh = (DWORD)(r->position >> 32);
  if (!ReadFile(h, at, (DWORD)r->length, &got, r->position >= 0 ? &at_position : NULL)) {
    DWORD error = GetLastError();
    if (error != ERROR_HANDLE_EOF && error != ERROR_BROKEN_PIPE) {
      fd_fail(r, error, "read");
    }
    got = 0;
  }
  r->done = got;
#else
  for (;;) {
    ssize_t got = r->position >= 0 ? pread(r->fd, at, (size_t)r->length, (off_t)r->position)
                                   : read(r->fd, at, (size_t)r->length);
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got < 0) {
      fd_fail(r, errno, "read");
    }
    r->done = got < 0 ? 0 : got;
    return;
  }
#endif
}

static void fd_write_work(TzWork *w) {
  FdRequest *r = (FdRequest *)w;
  const uint8_t *at = r->bytes->data + r->offset;
#ifdef _WIN32
  HANDLE h = tz_fs_fd_file(r->fd);
  if (h == NO_FILE) {
    fd_fail(r, ERROR_INVALID_HANDLE, "write");
    return;
  }
  DWORD written = 0;
  OVERLAPPED at_position;
  memset(&at_position, 0, sizeof at_position);
  at_position.Offset = (DWORD)(r->position & 0xFFFFFFFF);
  at_position.OffsetHigh = (DWORD)(r->position >> 32);
  if (!WriteFile(h, at, (DWORD)r->length, &written, r->position >= 0 ? &at_position : NULL)) {
    fd_fail(r, GetLastError(), "write");
  }
  r->done = written;
#else
  for (;;) {
    ssize_t written = r->position >= 0 ? pwrite(r->fd, at, (size_t)r->length, (off_t)r->position)
                                       : write(r->fd, at, (size_t)r->length);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0) {
      fd_fail(r, errno, "write");
    }
    r->done = written < 0 ? 0 : written;
    return;
  }
#endif
}

static void fd_transfer(TzPromise *p, int32_t fd, TzBytes *b, int32_t offset, int32_t length,
                        double position, TzMaker make, int writing) {
  FdRequest *r = fd_new(p, make);
  r->fd = fd;
  r->bytes = b;
  tz_rc_inc(&b->rc);
  if (offset < 0 || length < 0 || (int64_t)offset + length > b->length) {
    offset = 0;
    length = 0;
  }
  r->offset = offset;
  r->length = length;
  r->position = position >= 0 ? (int64_t)position : -1;
  fd_queue(r, writing ? fd_write_work : fd_read_work, fd_done_int);
}

void tz_fss_read(TzPromise *p, int32_t fd, TzBytes *b, int32_t offset, int32_t length,
                 double position, TzMaker make) {
  fd_transfer(p, fd, b, offset, length, position, make, 0);
}

void tz_fss_write(TzPromise *p, int32_t fd, TzBytes *b, int32_t offset, int32_t length,
                  double position, TzMaker make) {
  fd_transfer(p, fd, b, offset, length, position, make, 1);
}

static void fd_close_work(TzWork *w) {
  FdRequest *r = (FdRequest *)w;
#ifdef _WIN32
  if (tz_fs_fd_file(r->fd) == NO_FILE || _close(r->fd) != 0) {
    fd_fail(r, ERROR_INVALID_HANDLE, "close");
  }
#else
  if (close(r->fd) != 0) {
    fd_fail(r, errno, "close");
  }
#endif
}

static void fd_fsync_work(TzWork *w) {
  FdRequest *r = (FdRequest *)w;
#ifdef _WIN32
  HANDLE h = tz_fs_fd_file(r->fd);
  if (h == NO_FILE || !FlushFileBuffers(h)) {
    fd_fail(r, h == NO_FILE ? ERROR_INVALID_HANDLE : GetLastError(), "fsync");
  }
#else
  if (fsync(r->fd) != 0) {
    fd_fail(r, errno, "fsync");
  }
#endif
}

void tz_fss_close(TzPromise *p, int32_t fd, int32_t sync, TzMaker make) {
  FdRequest *r = fd_new(p, make);
  r->fd = fd;
  fd_queue(r, sync ? fd_fsync_work : fd_close_work, fd_done_int);
}
