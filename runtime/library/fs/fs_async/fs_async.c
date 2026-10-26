#ifdef _WIN32
#define _CRT_RAND_S
#else
#define _DEFAULT_SOURCE
#endif

#include "runtime/library/fs/fs_async/fs_async.h"
#include "runtime/library/fs/fs/fs.h"
#include "runtime/library/fs/fs_errors/fs_errors.h"
#include "runtime/library/fs/fs_sync/fs_sync.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/async/async.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/group/group.h"
#include "runtime/core/io/io.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/bytes/buffer/buffer.h"
#include "runtime/library/bytes/bytes/bytes.h"
#include "runtime/library/events/signal/signal.h"
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

Listed *tz_fs_listed_add(ListedList *list) {
  if (list->count == list->capacity) {
    size_t capacity = list->capacity * 2 + 16;
    Listed *items = (Listed *)realloc(list->items, capacity * sizeof(Listed));
    if (!items) {
      abort();
    }
    list->items = items;
    list->capacity = capacity;
  }
  Listed *entry = &list->items[list->count++];
  memset(entry, 0, sizeof *entry);

  return entry;
}

void tz_fs_listed_free(ListedList *list) {
  for (size_t i = 0; i < list->count; ++i) {
    tz_fs_raw_free(&list->items[i].name);
  }
  free(list->items);
  list->items = NULL;
  list->count = list->capacity = 0;
}

FsRequest *tz_fs_fsp_new(TzPromise *p, TzMaker make) {
  FsRequest *r = (FsRequest *)tz_alloc(sizeof(FsRequest));
  memset(r, 0, sizeof *r);
  r->promise = p;
  tz_ref_retain(p);
  r->make = make;

  return r;
}

static void fsp_free(FsRequest *r) {
  if (r->paths > 0) {
    tz_fs_path_free(&r->path);
  }
  if (r->paths > 1) {
    tz_fs_path_free(&r->dest);
  }
  tz_fs_raw_free(&r->data);
  tz_fs_raw_free(&r->name);
  tz_fs_raw_free(&r->at);
  tz_fs_listed_free(&r->entries);
  free(r->dirs);
  if (r->signal) {
    tz_instance_release((TzInstance *)r->signal);
  }
  tz_obj_release((TzObject *)r->promise);
  tz_free(r);
}

void tz_fs_fsp_reject_thrown(FsRequest *r) {
  void *failure = tz_failed;
  if (failure) {
    tz_failed = NULL;
    tz_promise_fail(r->promise, failure);
  } else {
    void *error = tz_thrown;
    tz_thrown = NULL;
    tz_promise_reject(r->promise, error);
  }
  fsp_free(r);
}

int tz_fs_fsp_path(FsRequest *r, void *obj, uint64_t meta) {
  Path *p = r->paths == 0 ? &r->path : &r->dest;
  if (!tz_fs_path_init(p, obj, meta)) {
    tz_fs_fsp_reject_thrown(r);
    return 0;
  }
  ++r->paths;

  return 1;
}

static int fsp_signal(FsRequest *r, void *signal) {
  if (!signal) {
    return 1;
  }
  tz_ref_retain(signal);
  r->signal = signal;
  if (!tz_signal_aborted(signal)) {
    return 1;
  }
  tz_promise_reject(r->promise, tz_abort_error(signal));
  fsp_free(r);

  return 0;
}

void tz_fs_fsp_queue(FsRequest *r, void (*work)(TzWork *), void (*done)(TzWork *)) {
  r->work.work = work;
  r->work.done = done;
  r->work.turns = 1;
  tz_work_queue_settles(&r->work, r->promise);
}

void tz_fs_fsp_fail(FsRequest *r, Failure failure, const char *syscall, int shows) {
  r->failed = 1;
  r->failure = failure;
  r->syscall = syscall;
  r->shows = shows;
}

#ifdef _WIN32
void tz_fs_fsp_fail_system(FsRequest *r, DWORD error, const char *syscall, int shows) {
  tz_fs_fsp_fail(r, tz_fs_win32_failure(error), syscall, shows);
}
#else
void tz_fs_fsp_fail_system(FsRequest *r, int error, const char *syscall, int shows) {
  tz_fs_fsp_fail(r, tz_fs_errno_failure(error), syscall, shows);
}
#endif

int tz_fs_fsp_succeeded(FsRequest *r) {
  if (r->signal && tz_signal_aborted(r->signal)) {
    tz_promise_reject(r->promise, tz_abort_error(r->signal));
    fsp_free(r);
    return 0;
  }
  if (!r->failed) {
    return 1;
  }
  if (r->at.length) {
    TzBuf units;
    tz_buf_init(&units);
#ifdef _WIN32
    tz_buf_units(&units, tz_fs_raw_wide(&r->at), tz_fs_raw_count(&r->at));
    size_t skip = units.length >= 4 && units.data[0] == '\\' && units.data[1] == '\\' &&
                          units.data[2] == '?' && units.data[3] == '\\'
                      ? 4
                      : 0;
#else
    TzStr text = tz_fs_str_of_utf8(r->at.data, r->at.length);
    tz_buf_str(&units, text);
    tz_str_release_value(text);
    size_t skip = 0;
#endif
    Path at;
    tz_fs_path_of_units(&at, units.data + skip, units.length - skip);
    tz_buf_free(&units);
    tz_fs_throw_failure(r->make, r->failure, r->syscall, &at.shown, NULL);
    tz_fs_path_free(&at);
  } else {
    tz_fs_throw_failure(r->make, r->failure, r->syscall, r->shows > 0 ? &r->path.shown : NULL,
                        r->shows > 1 ? &r->dest.shown : NULL);
  }
  tz_fs_fsp_reject_thrown(r);

  return 0;
}

void tz_fs_fsp_fulfill(FsRequest *r, const void *value, size_t size) {
  tz_promise_succeed(r->promise, value, size);
  fsp_free(r);
}

void tz_fs_fsp_done_void(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  if (tz_fs_fsp_succeeded(r)) {
    tz_fs_fsp_fulfill(r, NULL, 0);
  }
}

static uint32_t chunks_of(size_t length) {
  const size_t chunk = 512 * 1024;

  return (uint32_t)((length + chunk - 1) / chunk);
}

#ifdef _WIN32
static DWORD read_raw(File f, Raw *out) {
  LARGE_INTEGER size;
  if (GetFileSizeEx(f, &size) && size.QuadPart >= 0 && size.QuadPart < ((int64_t)1 << 31)) {
    tz_fs_raw_reserve(out, (size_t)size.QuadPart + 1);
  }
  for (;;) {
    if (out->length == out->capacity) {
      tz_fs_raw_reserve(out, 65536);
    }
    DWORD room = (DWORD)(out->capacity - out->length < 0x40000000 ? out->capacity - out->length
                                                                  : 0x40000000);
    DWORD got = 0;
    if (!ReadFile(f, out->data + out->length, room, &got, NULL)) {
      DWORD error = GetLastError();
      return error == ERROR_HANDLE_EOF || error == ERROR_BROKEN_PIPE ? 0 : error;
    }
    if (got == 0) {
      return 0;
    }
    out->length += got;
  }
}

static DWORD write_raw(File f, const uint8_t *data, size_t n) {
  while (n > 0) {
    DWORD chunk = n > 0x40000000 ? 0x40000000 : (DWORD)n, written = 0;
    if (!WriteFile(f, data, chunk, &written, NULL)) {
      return GetLastError();
    }
    data += written;
    n -= written;
  }

  return 0;
}
#else
static int read_raw(File f, Raw *out) {
  struct stat st;
  if (fstat(f, &st) == 0 && S_ISREG(st.st_mode) && st.st_size < ((off_t)1 << 31)) {
    tz_fs_raw_reserve(out, (size_t)st.st_size + 1);
  }
  for (;;) {
    if (out->length == out->capacity) {
      tz_fs_raw_reserve(out, 65536);
    }
    ssize_t got = read(f, out->data + out->length, out->capacity - out->length);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      return errno;
    }
    if (got == 0) {
      return 0;
    }
    out->length += (size_t)got;
  }
}

static int write_raw(File f, const uint8_t *data, size_t n) {
  while (n > 0) {
    ssize_t written = write(f, data, n);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return errno;
    }
    data += written;
    n -= (size_t)written;
  }

  return 0;
}
#endif

static void read_file_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  Failure failure;
  File f = tz_fs_open_raw(&r->path, r->flags, 0666, &failure);
  if (f == NO_FILE) {
    tz_fs_fsp_fail(r, failure, "open", 1);
    return;
  }
#ifdef _WIN32
  DWORD error = read_raw(f, &r->data);
#else
  int error = read_raw(f, &r->data);
#endif
  if (error) {
    tz_fs_fsp_fail_system(r, error, "read", 0);
  }
  tz_fs_close_file(f);
  r->work.turns = 3 + (r->data.length ? chunks_of(r->data.length) : 1);
}

static void read_file_done(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  if (!tz_fs_fsp_succeeded(r)) {
    return;
  }
  if (r->encoding < 0) {
    TzBytes *b = tz_buffer_copy_of(r->data.data, (int64_t)r->data.length);
    tz_fs_fsp_fulfill(r, &b, sizeof b);
    return;
  }
  TzStr text = r->encoding == 0
                   ? tz_fs_str_of_utf8(r->data.data, r->data.length)
                   : tz_buffer_decode(r->data.data, (int64_t)r->data.length, r->encoding);
  tz_fs_fsp_fulfill(r, &text, sizeof text);
}

void tz_fsp_read_file(TzPromise *p, void *path_obj, uint64_t path_meta, void *flag_obj,
                      uint64_t flag_meta, void *signal, int32_t encoding, TzMaker make) {
  FsRequest *r = tz_fs_fsp_new(p, make);
  r->encoding = encoding;
  if (!fsp_signal(r, signal)) {
    return;
  }
  if (!tz_fs_flags_of(flag_obj, flag_meta, &r->flags)) {
    tz_fs_fsp_reject_thrown(r);
    return;
  }
  if (!tz_fs_fsp_path(r, path_obj, path_meta)) {
    return;
  }
  tz_fs_fsp_queue(r, read_file_work, read_file_done);
}

static void write_file_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  Failure failure;
  File f = tz_fs_open_raw(&r->path, r->flags, r->mode, &failure);
  if (f == NO_FILE) {
    tz_fs_fsp_fail(r, failure, "open", 1);
    return;
  }
#ifdef _WIN32
  DWORD error = write_raw(f, r->data.data, r->data.length);
#else
  int error = write_raw(f, r->data.data, r->data.length);
#endif
  if (error) {
    tz_fs_fsp_fail_system(r, error, "write", 0);
  }
  tz_fs_close_file(f);
  r->work.turns = 2 + chunks_of(r->data.length);
}

static FsRequest *write_request(TzPromise *p, void *path_obj, uint64_t path_meta, void *flag_obj,
                                uint64_t flag_meta, int32_t mode, void *signal, TzMaker make) {
  FsRequest *r = tz_fs_fsp_new(p, make);
  if (!fsp_signal(r, signal)) {
    return NULL;
  }
  if (!tz_fs_flags_of(flag_obj, flag_meta, &r->flags)) {
    tz_fs_fsp_reject_thrown(r);
    return NULL;
  }
  if (!tz_fs_fsp_path(r, path_obj, path_meta)) {
    return NULL;
  }
  r->mode = mode;

  return r;
}

void tz_fsp_write_file(TzPromise *p, void *path_obj, uint64_t path_meta, void *data_obj,
                       uint64_t data_meta, void *flag_obj, uint64_t flag_meta, int32_t mode,
                       void *signal, int32_t encoding, TzMaker make) {
  FsRequest *r = write_request(p, path_obj, path_meta, flag_obj, flag_meta, mode, signal, make);
  if (!r) {
    return;
  }
  if (encoding != 0) {
    TzBytes *b = tz_buffer_from_string(data_obj, data_meta, encoding);
    tz_fs_raw_reserve(&r->data, (size_t)b->length + 1);
    memcpy(r->data.data, b->data, (size_t)b->length);
    r->data.length = (size_t)b->length;
    tz_instance_release((TzInstance *)b);
    tz_fs_fsp_queue(r, write_file_work, tz_fs_fsp_done_void);
    return;
  }
  TzBuf units;
  tz_buf_init(&units);
  tz_buf_str(&units, (TzStr){data_obj, data_meta});
  tz_fs_raw_reserve(&r->data, units.length * 3);
  r->data.length =
      (size_t)(tz_fs_utf8_encode(r->data.data, units.data, units.length) - r->data.data);
  tz_buf_free(&units);
  tz_fs_fsp_queue(r, write_file_work, tz_fs_fsp_done_void);
}

void tz_fsp_write_file_bytes(TzPromise *p, void *path_obj, uint64_t path_meta, const TzBytes *b,
                             void *flag_obj, uint64_t flag_meta, int32_t mode, void *signal,
                             TzMaker make) {
  FsRequest *r = write_request(p, path_obj, path_meta, flag_obj, flag_meta, mode, signal, make);
  if (!r) {
    return;
  }
  size_t length = (size_t)tz_bytes_byte_length(b);
  tz_fs_raw_reserve(&r->data, length + 1);
  memcpy(r->data.data, b->data, length);
  r->data.length = length;
  tz_fs_fsp_queue(r, write_file_work, tz_fs_fsp_done_void);
}

static void access_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
#ifdef _WIN32
  DWORD attributes = GetFileAttributesW(tz_fs_wide(&r->path));
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    tz_fs_fsp_fail_system(r, GetLastError(), "access", 1);
  } else if ((r->mode & 2) && (attributes & FILE_ATTRIBUTE_READONLY) &&
             !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    tz_fs_fsp_fail(r, (Failure){E_EPERM, tz_fs_errors[E_EPERM].windows}, "access", 1);
  }
#else
  if (access(tz_fs_native(&r->path), r->mode) != 0) {
    tz_fs_fsp_fail_system(r, errno, "access", 1);
  }
#endif
}

void tz_fsp_access(TzPromise *p, void *path_obj, uint64_t path_meta, int32_t mode, TzMaker make) {
  FsRequest *r = tz_fs_fsp_new(p, make);
  if (!tz_fs_fsp_path(r, path_obj, path_meta)) {
    return;
  }
  r->mode = mode;
  tz_fs_fsp_queue(r, access_work, tz_fs_fsp_done_void);
}

static void chmod_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
#ifdef _WIN32
  DWORD attributes = GetFileAttributesW(tz_fs_wide(&r->path));
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    tz_fs_fsp_fail_system(r, GetLastError(), "chmod", 1);
    return;
  }
  DWORD changed = r->mode & 0200 ? attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY
                                 : attributes | FILE_ATTRIBUTE_READONLY;
  if (changed != attributes && !SetFileAttributesW(tz_fs_wide(&r->path), changed)) {
    tz_fs_fsp_fail_system(r, GetLastError(), "chmod", 1);
  }
#else
  if (chmod(tz_fs_native(&r->path), (mode_t)r->mode) != 0) {
    tz_fs_fsp_fail_system(r, errno, "chmod", 1);
  }
#endif
}

void tz_fsp_chmod(TzPromise *p, void *path_obj, uint64_t path_meta, int32_t mode, TzMaker make) {
  FsRequest *r = tz_fs_fsp_new(p, make);
  if (!tz_fs_fsp_path(r, path_obj, path_meta)) {
    return;
  }
  r->mode = mode;
  tz_fs_fsp_queue(r, chmod_work, tz_fs_fsp_done_void);
}

static void truncate_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  OpenFlags flags = {1, 1, 0, 0, 0, 0};
  Failure failure;
  File f = tz_fs_open_raw(&r->path, flags, 0666, &failure);
  r->work.turns = 3;
  if (f == NO_FILE) {
    tz_fs_fsp_fail(r, failure, "open", 1);
    return;
  }
#ifdef _WIN32
  LARGE_INTEGER at;
  at.QuadPart = r->length < 0 ? 0 : r->length;
  if (!SetFilePointerEx(f, at, NULL, FILE_BEGIN) || !SetEndOfFile(f)) {
    tz_fs_fsp_fail_system(r, GetLastError(), "ftruncate", 0);
  }
#else
  if (ftruncate(f, r->length < 0 ? 0 : r->length) != 0) {
    tz_fs_fsp_fail_system(r, errno, "ftruncate", 0);
  }
#endif
  tz_fs_close_file(f);
}

void tz_fsp_truncate(TzPromise *p, void *path_obj, uint64_t path_meta, int64_t length,
                     TzMaker make) {
  FsRequest *r = tz_fs_fsp_new(p, make);
  if (!tz_fs_fsp_path(r, path_obj, path_meta)) {
    return;
  }
  r->length = length;
  tz_fs_fsp_queue(r, truncate_work, tz_fs_fsp_done_void);
}

static void stat_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  const char *syscall = r->follow ? "stat" : "lstat";
#ifdef _WIN32
  DWORD error = tz_fs_stat_of(tz_fs_wide(&r->path), r->follow, &r->st);
#else
  int error = tz_fs_stat_of(tz_fs_native(&r->path), r->follow, &r->st);
#endif
  if (error) {
    tz_fs_fsp_fail_system(r, error, syscall, 1);
  }
}

static void stat_done(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  if (!tz_fs_fsp_succeeded(r)) {
    return;
  }
  void *stats = r->make_item(&r->st);
  tz_fs_fsp_fulfill(r, &stats, sizeof stats);
}

void tz_fsp_stat(TzPromise *p, void *path_obj, uint64_t path_meta, int32_t follow,
                 TzMaker make_stats, TzMaker make) {
  FsRequest *r = tz_fs_fsp_new(p, make);
  if (!tz_fs_fsp_path(r, path_obj, path_meta)) {
    return;
  }
  r->follow = follow;
  r->make_item = make_stats;
  tz_fs_fsp_queue(r, stat_work, stat_done);
}
