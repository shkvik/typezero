#ifdef _WIN32
#define _CRT_RAND_S
#else
#define _DEFAULT_SOURCE
#endif

#include "runtime/library/fs/fs/fs.h"
#include "runtime/library/fs/fs_errors/fs_errors.h"
#include "runtime/library/fs/fs_sync/fs_sync.h"
#include "runtime/library/fs/fs_dirs/fs_dirs.h"
#include "runtime/library/fs/fs_async/fs_async.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/async/async.h"
#include "runtime/core/io/io.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
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

#ifdef _WIN32
static int list_raw(const uint16_t *path, size_t n, uint32_t dir, ListedList *out,
                    Failure *failure) {
  Raw pattern = {NULL, 0, 0};
  tz_fs_raw_units(&pattern, path, n);
  tz_fs_raw_unit(&pattern, 0);
  DWORD attributes = GetFileAttributesW((const wchar_t *)pattern.data);
  if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    *failure = attributes == INVALID_FILE_ATTRIBUTES
                   ? tz_fs_win32_failure(GetLastError())
                   : (Failure){E_ENOTDIR, tz_fs_errors[E_ENOTDIR].windows};
    tz_fs_raw_free(&pattern);
    return 0;
  }
  pattern.length -= 2;
  if (n == 0 || path[n - 1] != '\\') {
    tz_fs_raw_unit(&pattern, '\\');
  }
  tz_fs_raw_unit(&pattern, '*');
  tz_fs_raw_unit(&pattern, 0);
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileExW((const wchar_t *)pattern.data, FindExInfoBasic, &data,
                                 FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
  tz_fs_raw_free(&pattern);
  if (find == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_NO_MORE_FILES) {
      return 1;
    }
    *failure = tz_fs_win32_failure(error);
    return 0;
  }
  do {
    const wchar_t *name = data.cFileName;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) {
      continue;
    }
    Listed *entry = tz_fs_listed_add(out);
    tz_fs_raw_units(&entry->name, (const uint16_t *)name, wcslen(name));
    DWORD a = data.dwFileAttributes;
    entry->kind = a & FILE_ATTRIBUTE_DEVICE          ? S_CHR
                  : a & FILE_ATTRIBUTE_REPARSE_POINT ? S_LNK
                  : a & FILE_ATTRIBUTE_DIRECTORY     ? S_DIR
                                                     : S_REG;
    entry->dir = dir;
  } while (FindNextFileW(find, &data));
  FindClose(find);

  return 1;
}
#else
static int compare_listed(const void *a, const void *b) {
  const Raw *x = &((const Listed *)a)->name, *y = &((const Listed *)b)->name;
  size_t n = x->length < y->length ? x->length : y->length;
  int c = n ? memcmp(x->data, y->data, n) : 0;

  return c ? c : x->length < y->length ? -1 : x->length > y->length;
}

static int list_raw(const uint8_t *path, size_t n, uint32_t dir, ListedList *out,
                    Failure *failure) {
  Raw native_path = {NULL, 0, 0};
  tz_fs_raw_append(&native_path, path, n);
  tz_fs_raw_append(&native_path, "", 1);
  DIR *d = opendir((const char *)native_path.data);
  tz_fs_raw_free(&native_path);
  if (!d) {
    *failure = tz_fs_errno_failure(errno);
    return 0;
  }
  size_t first = out->count;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) {
      continue;
    }
    Listed *entry = tz_fs_listed_add(out);
    tz_fs_raw_append(&entry->name, e->d_name, strlen(e->d_name));
    entry->dir = dir;
    switch (e->d_type) {
    case DT_REG:
      entry->kind = S_REG;
      break;
    case DT_DIR:
      entry->kind = S_DIR;
      break;
    case DT_LNK:
      entry->kind = S_LNK;
      break;
    case DT_CHR:
      entry->kind = S_CHR;
      break;
    case DT_BLK:
      entry->kind = S_BLK;
      break;
    case DT_FIFO:
      entry->kind = S_FIFO;
      break;
    case DT_SOCK:
      entry->kind = S_SOCK;
      break;
    default:
      entry->kind = 0;
      break;
    }
  }
  closedir(d);
  qsort(out->items + first, out->count - first, sizeof(Listed), compare_listed);

  return 1;
}
#endif

#ifdef _WIN32
#define SYSTEM_UNIT 2
#else
#define SYSTEM_UNIT 1
#endif

typedef struct {
  uint32_t dir;
  Raw path;
  ListedList entries;
} Pending;

static void readdir_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  Raw root = {NULL, 0, 0};
#ifdef _WIN32
  tz_fs_raw_append(&root, r->path.wide.data, (r->path.wide.length - 1) * 2);
#else
  tz_fs_raw_append(&root, r->path.utf8.data, r->path.utf8.length - 1);
#endif
  r->dirs = (uint32_t *)malloc(sizeof(uint32_t) * 16);
  if (!r->dirs) {
    abort();
  }
  r->dir_capacity = 16;
  r->dirs[r->dir_count++] = UINT32_MAX;
  Pending *stack = (Pending *)malloc(sizeof(Pending) * 8);
  if (!stack) {
    abort();
  }
  size_t depth = 0, capacity = 8;
  Pending first = {0, root, {NULL, 0, 0}};
  Failure failure;
  if (!list_raw((const void *)root.data, root.length / SYSTEM_UNIT, 0, &first.entries, &failure)) {
    tz_fs_fsp_fail(r, failure, "scandir", 1);
    tz_fs_raw_free(&first.path);
    free(stack);
    return;
  }
  stack[depth++] = first;
  int failed = 0;
  while (depth > 0) {
    Pending at = stack[--depth];
    for (size_t i = 0; i < at.entries.count && !failed; ++i) {
      Listed *result = tz_fs_listed_add(&r->entries);
      *result = at.entries.items[i];
      memset(&at.entries.items[i], 0, sizeof(Listed));
      if (!r->recursive) {
        continue;
      }
      Raw child = {NULL, 0, 0};
      tz_fs_raw_append(&child, at.path.data, at.path.length);
#ifdef _WIN32
      if (tz_fs_raw_count(&child) == 0 ||
          tz_fs_raw_wide(&child)[tz_fs_raw_count(&child) - 1] != '\\') {
        tz_fs_raw_unit(&child, '\\');
      }
#else
      if (child.length == 0 || child.data[child.length - 1] != '/') {
        tz_fs_raw_append(&child, "/", 1);
      }
#endif
      tz_fs_raw_append(&child, result->name.data, result->name.length);
      int descend;
      if (r->make_item) {
        descend = result->kind == S_DIR;
      } else {
        TzStatsParts st;
#ifdef _WIN32
        tz_fs_raw_unit(&child, 0);
        descend = tz_fs_stat_of((const wchar_t *)child.data, 1, &st) == 0 &&
                  ((uint32_t)st.mode & S_KIND) == S_DIR;
        child.length -= 2;
#else
        tz_fs_raw_append(&child, "", 1);
        descend = tz_fs_stat_of((const char *)child.data, 1, &st) == 0 &&
                  ((uint32_t)st.mode & S_KIND) == S_DIR;
        child.length -= 1;
#endif
      }
      if (!descend) {
        tz_fs_raw_free(&child);
        continue;
      }
      if (r->dir_count == r->dir_capacity) {
        r->dir_capacity *= 2;
        uint32_t *dirs = (uint32_t *)realloc(r->dirs, sizeof(uint32_t) * r->dir_capacity);
        if (!dirs) {
          abort();
        }
        r->dirs = dirs;
      }
      uint32_t number = (uint32_t)r->dir_count;
      r->dirs[r->dir_count++] = (uint32_t)(r->entries.count - 1);
      ++r->work.turns;
      Pending next = {number, child, {NULL, 0, 0}};
      if (!list_raw((const void *)child.data, child.length / SYSTEM_UNIT, number, &next.entries,
                    &failure)) {
        tz_fs_fsp_fail(r, failure, "scandir", 1);
        tz_fs_raw_append(&r->at, child.data, child.length);
        tz_fs_raw_free(&next.path);
        failed = 1;
        break;
      }
      if (depth == capacity) {
        capacity *= 2;
        Pending *bigger = (Pending *)realloc(stack, sizeof(Pending) * capacity);
        if (!bigger) {
          abort();
        }
        stack = bigger;
      }
      stack[depth++] = next;
    }
    tz_fs_listed_free(&at.entries);
    tz_fs_raw_free(&at.path);
    if (failed) {
      break;
    }
  }
  for (size_t i = 0; i < depth; ++i) {
    tz_fs_listed_free(&stack[i].entries);
    tz_fs_raw_free(&stack[i].path);
  }
  free(stack);
}

static TzStr str_of_system(const Raw *name) {
#ifdef _WIN32
  return tz_str_from_utf16(tz_fs_raw_wide(name), tz_fs_raw_count(name));
#else

  return tz_fs_str_of_utf8(name->data, name->length);
#endif
}

static void buf_system(TzBuf *out, const Raw *name) {
#ifdef _WIN32
  tz_buf_units(out, tz_fs_raw_wide(name), tz_fs_raw_count(name));
#else
  TzStr s = tz_fs_str_of_utf8(name->data, name->length);
  tz_buf_str(out, s);
  tz_str_release_value(s);
#endif
}

static void readdir_done(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  if (!tz_fs_fsp_succeeded(r)) {
    return;
  }
  size_t count = r->entries.count;
  TzArray *result = tz_arr_new(r->element, 0, (int64_t)count);
  TzBuf *seen = (TzBuf *)tz_alloc(sizeof(TzBuf) * r->dir_count);
  TzBuf *relative = (TzBuf *)tz_alloc(sizeof(TzBuf) * r->dir_count);
  for (size_t d = 0; d < r->dir_count; ++d) {
    tz_buf_init(&seen[d]);
    tz_buf_init(&relative[d]);
    if (d == 0) {
      tz_buf_units(&seen[0], r->path.given.data, r->path.given.length);
      continue;
    }
    const Listed *entry = &r->entries.items[r->dirs[d]];
    TzBuf name;
    tz_buf_init(&name);
    buf_system(&name, &entry->name);
    const TzBuf *parent = &seen[entry->dir];
    tz_path_join_units(&seen[d], parent->data, parent->length, name.data, name.length);
    if (relative[entry->dir].length) {
      tz_buf_units(&relative[d], relative[entry->dir].data, relative[entry->dir].length);
      tz_buf_push(&relative[d], SEPARATOR);
    }
    tz_buf_units(&relative[d], name.data, name.length);
    tz_buf_free(&name);
  }
  for (size_t i = 0; i < count; ++i) {
    const Listed *e = &r->entries.items[i];
    if (r->make_item) {
      TzDirentParts parts;
      parts.name = str_of_system(&e->name);
      parts.parent_path = tz_str_from_utf16(seen[e->dir].data, seen[e->dir].length);
      parts.kind = (int32_t)e->kind;
      void *dirent = r->make_item(&parts);
      tz_arr_push_value(result, &dirent);
    } else {
      TzBuf name;
      tz_buf_init(&name);
      if (relative[e->dir].length) {
        tz_buf_units(&name, relative[e->dir].data, relative[e->dir].length);
        tz_buf_push(&name, SEPARATOR);
      }
      buf_system(&name, &e->name);
      TzStr s = tz_buf_take(&name);
      tz_arr_push_value(result, &s);
    }
  }
  for (size_t d = 0; d < r->dir_count; ++d) {
    tz_buf_free(&seen[d]);
    tz_buf_free(&relative[d]);
  }
  tz_free(seen);
  tz_free(relative);
  tz_fs_fsp_fulfill(r, &result, sizeof result);
}

void tz_fsp_readdir(TzPromise *p, void *path_obj, uint64_t path_meta, int32_t recursive,
                    const TzType *element, TzMaker make_dirent, TzMaker make) {
  FsRequest *r = tz_fs_fsp_new(p, make);
  if (!tz_fs_fsp_path(r, path_obj, path_meta, "scandir")) {
    return;
  }
  r->recursive = recursive;
  r->element = element;
  r->make_item = make_dirent;
  tz_fs_fsp_queue(r, readdir_work, readdir_done);
}

static void mkdir_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  Failure failure;
#ifdef _WIN32
  r->made = tz_fs_mkdir_raw(r->path.wide.data, r->path.wide.length - 1, r->recursive, r->mode,
                            &r->name, &failure, &r->at);
#else
  r->made = tz_fs_mkdir_raw(r->path.utf8.data, r->path.utf8.length - 1, r->recursive, r->mode,
                            &r->name, &failure, &r->at);
#endif
  if (r->made < 0) {
    tz_fs_fsp_fail(r, failure, "mkdir", 1);
  }
  if (!r->recursive) {
    return;
  }
  size_t levels = 0;
  if (r->made > 0) {
#ifdef _WIN32
    const uint16_t *target = r->path.wide.data;
    size_t n = r->path.wide.length - 1, from = tz_fs_raw_count(&r->name);
    for (size_t i = from; i < n; ++i) {
      levels += target[i] == '\\';
    }
#else
    const uint8_t *target = r->path.utf8.data;
    size_t n = r->path.utf8.length - 1, from = r->name.length;
    for (size_t i = from; i < n; ++i) {
      levels += target[i] == '/';
    }
#endif
    ++levels;
  }
  r->work.turns = levels ? 2 * (uint32_t)levels - 1 : 2;
}

static void mkdir_done(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  if (!tz_fs_fsp_succeeded(r)) {
    return;
  }
  TzOptStr first;
  memset(&first, 0, sizeof first);
  if (r->made) {
    first.value = tz_fs_made_dir(&r->name);
    first.has = 1;
  }
  tz_fs_fsp_fulfill(r, &first, sizeof first);
}

void tz_fsp_mkdir(TzPromise *p, void *path_obj, uint64_t path_meta, int32_t recursive, int32_t mode,
                  TzMaker make) {
  FsRequest *r = tz_fs_fsp_new(p, make);
  if (!tz_fs_fsp_path(r, path_obj, path_meta, "mkdir")) {
    return;
  }
  r->recursive = recursive;
  r->mode = mode;
  tz_fs_fsp_queue(r, mkdir_work, mkdir_done);
}

static void mkdtemp_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
#ifdef _WIN32
  DWORD error = tz_fs_make_dir(r->dest.wide.data);
  r->made = error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS;
  if (error) {
    tz_fs_fsp_fail_system(r, error, "mkdtemp", 1);
  }
#else
  int error = mkdir(tz_fs_native(&r->dest), 0700) == 0 ? 0 : errno;
  r->made = error == EEXIST;
  if (error) {
    tz_fs_fsp_fail_system(r, error, "mkdtemp", 1);
  }
#endif
}

static void mkdtemp_try(FsRequest *r);

static void mkdtemp_done(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  if (r->failed && r->made && r->attempts < 100) {
    r->failed = 0;
    tz_fs_path_free(&r->dest);
    --r->paths;
    mkdtemp_try(r);
    return;
  }
  if (r->failed) {
    TzBuf shown;
    tz_buf_init(&shown);
    tz_buf_units(&shown, r->path.given.data, r->path.given.length);
    tz_buf_ascii(&shown, "XXXXXX");
    tz_fs_throw_failure(r->make, r->failure, "mkdtemp", &shown, NULL);
    tz_buf_free(&shown);
    tz_fs_fsp_reject_thrown(r);
    return;
  }
  TzStr name = tz_str_from_utf16(r->dest.given.data, r->dest.given.length);
  tz_fs_fsp_fulfill(r, &name, sizeof name);
}

static void mkdtemp_try(FsRequest *r) {
  static const char letters[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  ++r->attempts;
  TzBuf name;
  tz_buf_init(&name);
  tz_buf_units(&name, r->path.given.data, r->path.given.length);
  for (int i = 0; i < 6; ++i) {
    unsigned int n;
#ifdef _WIN32
    if (rand_s(&n) != 0) {
      n = (unsigned int)GetTickCount() * 2654435761u + (unsigned int)i;
    }
#else
    n = (unsigned int)rand() ^ (unsigned int)time(NULL) ^ ((unsigned int)getpid() << 8);
#endif
    tz_buf_push(&name, (uint16_t)letters[n % 62]);
  }
  tz_fs_path_of_units(&r->dest, name.data, name.length);
  ++r->paths;
  tz_buf_free(&name);
  tz_fs_fsp_queue(r, mkdtemp_work, mkdtemp_done);
}

void tz_fsp_mkdtemp(TzPromise *p, void *prefix_obj, uint64_t prefix_meta, TzMaker make) {
  FsRequest *r = tz_fs_fsp_new(p, make);
  if (!tz_fs_fsp_path(r, prefix_obj, prefix_meta, "mkdtemp")) {
    return;
  }
  mkdtemp_try(r);
}

static void rmdir_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
#ifdef _WIN32
  if (!RemoveDirectoryW(tz_fs_wide(&r->path))) {
    tz_fs_fsp_fail_system(r, GetLastError(), "rmdir", 1);
  }
#else
  if (rmdir(tz_fs_native(&r->path)) != 0) {
    tz_fs_fsp_fail_system(r, errno, "rmdir", 1);
  }
#endif
}

static void unlink_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
#ifdef _WIN32
  DWORD error = tz_fs_unlink_path(tz_fs_wide(&r->path));
  if (error) {
    tz_fs_fsp_fail_system(r, error, "unlink", 1);
  }
#else
  if (unlink(tz_fs_native(&r->path)) != 0) {
    tz_fs_fsp_fail_system(r, errno, "unlink", 1);
  }
#endif
}

void tz_fsp_remove(TzPromise *p, void *path_obj, uint64_t path_meta, int32_t which, TzMaker make) {
  FsRequest *r = tz_fs_fsp_new(p, make);
  if (!tz_fs_fsp_path(r, path_obj, path_meta, which ? "unlink" : "rmdir")) {
    return;
  }
  tz_fs_fsp_queue(r, which ? unlink_work : rmdir_work, tz_fs_fsp_done_void);
}

static void rm_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  Failure failure;
  const char *syscall = NULL;
  int depth = 0;
  int found = tz_fs_rm_raw(&r->path, r->recursive, r->force, &failure, &syscall, &depth);
  if (found == RM_FAILED) {
    tz_fs_fsp_fail(r, failure, syscall, 1);
  }
  r->directory = found == RM_DIRECTORY;
  r->work.turns = 3 + 4 * (uint32_t)depth;
}

static void rm_done(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  if (r->directory) {
    tz_fs_throw_rm_directory(&r->path, r->make);
    tz_fs_fsp_reject_thrown(r);
    return;
  }
  tz_fs_fsp_done_void(w);
}

void tz_fsp_rm(TzPromise *p, void *path_obj, uint64_t path_meta, int32_t recursive, int32_t force,
               TzMaker make) {
  FsRequest *r = tz_fs_fsp_new(p, make);
  if (!tz_fs_fsp_path(r, path_obj, path_meta, "rm")) {
    return;
  }
  r->recursive = recursive;
  r->force = force;
  tz_fs_fsp_queue(r, rm_work, rm_done);
}

static void rename_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
#ifdef _WIN32
  if (!MoveFileExW(tz_fs_wide(&r->path), tz_fs_wide(&r->dest), MOVEFILE_REPLACE_EXISTING)) {
    tz_fs_fsp_fail_system(r, GetLastError(), "rename", 2);
  }
#else
  if (rename(tz_fs_native(&r->path), tz_fs_native(&r->dest)) != 0) {
    tz_fs_fsp_fail_system(r, errno, "rename", 2);
  }
#endif
}

void tz_fsp_rename(TzPromise *p, void *from_obj, uint64_t from_meta, void *to_obj, uint64_t to_meta,
                   TzMaker make) {
  FsRequest *r = tz_fs_fsp_new(p, make);
  if (!tz_fs_fsp_path(r, from_obj, from_meta, "rename") || !tz_fs_fsp_path(r, to_obj, to_meta, "rename")) {
    return;
  }
  tz_fs_fsp_queue(r, rename_work, tz_fs_fsp_done_void);
}

static void copy_file_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
#ifdef _WIN32
  if (r->mode & 4) {
    tz_fs_fsp_fail(r, (Failure){E_ENOSYS, tz_fs_errors[E_ENOSYS].windows}, "copyfile", 2);
  } else if (!CopyFileW(tz_fs_wide(&r->path), tz_fs_wide(&r->dest), (r->mode & 1) != 0)) {
    DWORD error = GetLastError();
    TzStatsParts a, b;
    int same = tz_fs_win32_failure(error).index == E_EBUSY &&
               tz_fs_stat_of(tz_fs_wide(&r->path), 1, &a) == 0 &&
               tz_fs_stat_of(tz_fs_wide(&r->dest), 1, &b) == 0 && a.dev == b.dev && a.ino == b.ino;
    if (!same) {
      tz_fs_fsp_fail_system(r, error, "copyfile", 2);
    }
  }
#else
  int in = -1, out = -1;
  if (r->mode & 4) {
    tz_fs_fsp_fail_system(r, ENOSYS, "copyfile", 2);
  } else if ((in = open(tz_fs_native(&r->path), O_RDONLY | O_CLOEXEC)) < 0) {
    tz_fs_fsp_fail_system(r, errno, "copyfile", 2);
  } else {
    struct stat s;
    fstat(in, &s);
    out = open(tz_fs_native(&r->dest),
               O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | ((r->mode & 1) ? O_EXCL : 0),
               s.st_mode & 0777);
    if (out < 0) {
      tz_fs_fsp_fail_system(r, errno, "copyfile", 2);
    } else {
      char chunk[65536];
      ssize_t got;
      while ((got = read(in, chunk, sizeof chunk)) > 0) {
        if (write(out, chunk, (size_t)got) != got) {
          tz_fs_fsp_fail_system(r, errno, "copyfile", 2);
          break;
        }
      }
      close(out);
    }
    close(in);
  }
#endif
}

void tz_fsp_copy_file(TzPromise *p, void *from_obj, uint64_t from_meta, void *to_obj,
                      uint64_t to_meta, int32_t mode, TzMaker make) {
  FsRequest *r = tz_fs_fsp_new(p, make);
  if (!tz_fs_fsp_path(r, from_obj, from_meta, "copyfile") || !tz_fs_fsp_path(r, to_obj, to_meta, "copyfile")) {
    return;
  }
  r->mode = mode;
  tz_fs_fsp_queue(r, copy_file_work, tz_fs_fsp_done_void);
}

static void realpath_work(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
#ifdef _WIN32
  HANDLE h =
      CreateFileW(tz_fs_wide(&r->path), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    tz_fs_fsp_fail_system(r, GetLastError(), "realpath", 1);
    return;
  }
  DWORD n = GetFinalPathNameByHandleW(h, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  Raw buffer = {NULL, 0, 0};
  tz_fs_raw_reserve(&buffer, ((size_t)n + 1) * 2);
  DWORD got = n ? GetFinalPathNameByHandleW(h, (wchar_t *)buffer.data, n + 1,
                                            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)
                : 0;
  DWORD error = got == 0 || got > n ? GetLastError() : 0;
  CloseHandle(h);
  if (error || got == 0 || got > n) {
    tz_fs_fsp_fail_system(r, error ? error : ERROR_INVALID_FUNCTION, "realpath", 1);
    tz_fs_raw_free(&buffer);
    return;
  }
  const uint16_t *s = (const uint16_t *)buffer.data;
  static const uint16_t unc[] = {'\\', '\\', '?', '\\', 'U', 'N', 'C', '\\'};
  if (got >= 8 && memcmp(s, unc, sizeof unc) == 0) {
    static const uint16_t two[] = {'\\', '\\'};
    tz_fs_raw_units(&r->name, two, 2);
    tz_fs_raw_units(&r->name, s + 8, got - 8);
  } else if (got >= 4 && s[0] == '\\' && s[1] == '\\' && s[2] == '?' && s[3] == '\\') {
    tz_fs_raw_units(&r->name, s + 4, got - 4);
  } else {
    tz_fs_raw_units(&r->name, s, got);
  }
  tz_fs_raw_free(&buffer);
#else
  char *real = realpath(tz_fs_native(&r->path), NULL);
  if (!real) {
    tz_fs_fsp_fail_system(r, errno, "realpath", 1);
    return;
  }
  tz_fs_raw_append(&r->name, real, strlen(real));
  free(real);
#endif
}

static void realpath_done(TzWork *w) {
  FsRequest *r = (FsRequest *)w;
  if (!tz_fs_fsp_succeeded(r)) {
    return;
  }
  TzStr path = str_of_system(&r->name);
  tz_fs_fsp_fulfill(r, &path, sizeof path);
}

void tz_fsp_realpath(TzPromise *p, void *path_obj, uint64_t path_meta, TzMaker make) {
  FsRequest *r = tz_fs_fsp_new(p, make);
  if (!tz_fs_fsp_path(r, path_obj, path_meta, "realpath")) {
    return;
  }
  tz_fs_fsp_queue(r, realpath_work, realpath_done);
}
