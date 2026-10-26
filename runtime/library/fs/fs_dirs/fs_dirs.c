#ifdef _WIN32
#define _CRT_RAND_S
#else
#define _DEFAULT_SOURCE
#endif

#include "runtime/library/fs/fs_dirs/fs_dirs.h"
#include "runtime/library/fs/fs/fs.h"
#include "runtime/library/fs/fs_errors/fs_errors.h"
#include "runtime/library/fs/fs_sync/fs_sync.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/error/error.h"
#include "runtime/core/group/group.h"
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

typedef struct {
  TzBuf name;
  uint32_t kind;
} Entry;

typedef struct {
  Entry *items;
  size_t count, capacity;
} Entries;

static void entries_free(Entries *e) {
  for (size_t i = 0; i < e->count; ++i) {
    tz_buf_free(&e->items[i].name);
  }
  tz_free(e->items);
  e->items = NULL;
  e->count = e->capacity = 0;
}

static Entry *entries_add(Entries *e) {
  if (e->count == e->capacity) {
    size_t capacity = e->capacity * 2 + 16;
    Entry *items = (Entry *)tz_alloc(capacity * sizeof(Entry));
    if (e->count) {
      memcpy(items, e->items, e->count * sizeof(Entry));
    }
    tz_free(e->items);
    e->items = items;
    e->capacity = capacity;
  }
  Entry *entry = &e->items[e->count++];
  tz_buf_init(&entry->name);
  entry->kind = 0;

  return entry;
}

#ifdef _WIN32
static int list_dir(const Path *p, Entries *out, TzMaker make) {
  DWORD attributes = GetFileAttributesW(tz_fs_wide(p));
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    tz_fs_fail_win32(make, GetLastError(), "scandir", p, NULL);
    return 0;
  }
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    tz_fs_fail_index(make, E_ENOTDIR, "scandir", p);
    return 0;
  }
  TzBuf pattern;
  tz_buf_init(&pattern);
  tz_buf_units(&pattern, p->wide.data, p->wide.length - 1);
  if (pattern.length == 0 || pattern.data[pattern.length - 1] != '\\') {
    tz_buf_push(&pattern, '\\');
  }
  tz_buf_push(&pattern, '*');
  tz_buf_push(&pattern, 0);
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileExW((const wchar_t *)pattern.data, FindExInfoBasic, &data,
                                 FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
  tz_buf_free(&pattern);
  if (find == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_NO_MORE_FILES) {
      return 1;
    }
    tz_fs_fail_win32(make, error, "scandir", p, NULL);
    return 0;
  }
  do {
    const wchar_t *name = data.cFileName;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) {
      continue;
    }
    Entry *entry = entries_add(out);
    tz_buf_units(&entry->name, (const uint16_t *)name, wcslen(name));
    DWORD a = data.dwFileAttributes;
    entry->kind = a & FILE_ATTRIBUTE_DEVICE          ? S_CHR
                  : a & FILE_ATTRIBUTE_REPARSE_POINT ? S_LNK
                  : a & FILE_ATTRIBUTE_DIRECTORY     ? S_DIR
                                                     : S_REG;
  } while (FindNextFileW(find, &data));
  FindClose(find);

  return 1;
}

static int is_directory(const Path *p) {
  TzStatsParts st;

  return tz_fs_stat_of(tz_fs_wide(p), 1, &st) == 0 && ((uint32_t)st.mode & S_KIND) == S_DIR;
}
#else
static int compare_entries(const void *a, const void *b) {
  const Entry *x = (const Entry *)a, *y = (const Entry *)b;
  Bytes u, v;
  tz_fs_bytes_init(&u);
  tz_fs_bytes_init(&v);
  tz_fs_bytes_utf8(&u, x->name.data, x->name.length);
  tz_fs_bytes_utf8(&v, y->name.data, y->name.length);
  size_t n = u.length < v.length ? u.length : v.length;
  int c = n ? memcmp(u.data, v.data, n) : 0;
  if (c == 0) {
    c = u.length < v.length ? -1 : u.length > v.length;
  }
  tz_fs_bytes_free(&u);
  tz_fs_bytes_free(&v);

  return c;
}

static int list_dir(const Path *p, Entries *out, TzMaker make) {
  DIR *dir = opendir(tz_fs_native(p));
  if (!dir) {
    tz_fs_fail_errno(make, errno, "scandir", p, NULL);
    return 0;
  }
  struct dirent *d;
  while ((d = readdir(dir)) != NULL) {
    if (d->d_name[0] == '.' && (d->d_name[1] == 0 || (d->d_name[1] == '.' && d->d_name[2] == 0))) {
      continue;
    }
    Entry *entry = entries_add(out);
    TzStr name = tz_fs_str_of_utf8((const uint8_t *)d->d_name, strlen(d->d_name));
    tz_buf_str(&entry->name, name);
    tz_str_release_value(name);
    switch (d->d_type) {
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
  closedir(dir);
  qsort(out->items, out->count, sizeof(Entry), compare_entries);

  return 1;
}

static int is_directory(const Path *p) {
  struct stat s;

  return stat(tz_fs_native(p), &s) == 0 && S_ISDIR(s.st_mode);
}
#endif

TzArray *tz_fs_readdir(void *path_obj, uint64_t path_meta, int32_t recursive, const TzType *element,
                       TzMaker make_dirent, TzMaker make) {
  Path base;
  if (!tz_fs_path_init(&base, path_obj, path_meta, make, "scandir")) {
    return NULL;
  }
  TzArray *result = tz_arr_new(element, 0, 16);
  typedef struct {
    TzBuf path, relative;
  } Dir;
  Dir *queue = (Dir *)tz_alloc(sizeof(Dir) * 16);
  size_t count = 1, capacity = 16;
  tz_buf_init(&queue[0].path);
  tz_buf_init(&queue[0].relative);
  tz_buf_units(&queue[0].path, base.given.data, base.given.length);
  int ok = 1;
  for (size_t q = 0; q < count && ok; ++q) {
    Path dir;
    tz_fs_path_of_units(&dir, queue[q].path.data, queue[q].path.length);
    Entries entries = {NULL, 0, 0};
    ok = list_dir(q == 0 ? &base : &dir, &entries, make);
    for (size_t i = 0; ok && i < entries.count; ++i) {
      Entry *e = &entries.items[i];
      TzBuf child;
      tz_buf_init(&child);
      tz_path_join_units(&child, queue[q].path.data, queue[q].path.length, e->name.data,
                         e->name.length);
      int descend = 0;
      if (recursive) {
        if (make_dirent) {
          descend = e->kind == S_DIR;
        } else {
          Path c;
          tz_fs_path_of_units(&c, child.data, child.length);
          descend = is_directory(&c);
          tz_fs_path_free(&c);
        }
      }
      if (make_dirent) {
        TzDirentParts parts;
        parts.name = tz_str_from_utf16(e->name.data, e->name.length);
        parts.parent_path = tz_str_from_utf16(queue[q].path.data, queue[q].path.length);
        parts.kind = (int32_t)e->kind;
        void *dirent = make_dirent(&parts);
        tz_arr_push_value(result, &dirent);
      } else {
        TzBuf name;
        tz_buf_init(&name);
        if (queue[q].relative.length) {
          tz_buf_units(&name, queue[q].relative.data, queue[q].relative.length);
          tz_buf_push(&name, SEPARATOR);
        }
        tz_buf_units(&name, e->name.data, e->name.length);
        TzStr s = tz_buf_take(&name);
        tz_arr_push_value(result, &s);
      }
      if (descend) {
        if (count == capacity) {
          Dir *bigger = (Dir *)tz_alloc(sizeof(Dir) * capacity * 2);
          memcpy(bigger, queue, sizeof(Dir) * count);
          tz_free(queue);
          queue = bigger;
          capacity *= 2;
        }
        Dir *next = &queue[count++];
        next->path = child;
        tz_buf_init(&next->relative);
        if (queue[q].relative.length) {
          tz_buf_units(&next->relative, queue[q].relative.data, queue[q].relative.length);
          tz_buf_push(&next->relative, SEPARATOR);
        }
        tz_buf_units(&next->relative, e->name.data, e->name.length);
      } else {
        tz_buf_free(&child);
      }
    }
    entries_free(&entries);
    tz_fs_path_free(&dir);
  }
  for (size_t q = 0; q < count; ++q) {
    tz_buf_free(&queue[q].path);
    tz_buf_free(&queue[q].relative);
  }
  tz_free(queue);
  tz_fs_path_free(&base);
  if (!ok) {
    if (tz_rc_dec(&result->rc)) {
      tz_arr_free(result);
    }
    return NULL;
  }

  return result;
}

#ifdef _WIN32
DWORD tz_fs_make_dir(const uint16_t *path) {
  if (CreateDirectoryW((const wchar_t *)path, NULL)) {
    return 0;
  }
  DWORD error = GetLastError();

  return error == ERROR_INVALID_NAME || error == ERROR_DIRECTORY ? ERROR_INVALID_PARAMETER : error;
}

static ptrdiff_t last_separator(const uint16_t *units, size_t n) {
  for (size_t i = n; i > 0; --i) {
    if (units[i - 1] == '\\') {
      return (ptrdiff_t)i - 1;
    }
  }

  return -1;
}
#endif

#ifdef _WIN32
int tz_fs_mkdir_raw(const uint16_t *path, size_t n, int32_t recursive, int32_t mode, Raw *first,
                    Failure *failure, Raw *at) {
  (void)mode;
  Raw target = {NULL, 0, 0};
  tz_fs_raw_units(&target, path, n);
  tz_fs_raw_unit(&target, 0);
  if (!recursive) {
    DWORD error = tz_fs_make_dir(tz_fs_raw_wide(&target));
    tz_fs_raw_free(&target);
    if (!error) {
      return 0;
    }
    *failure = tz_fs_win32_failure(error);
    return -1;
  }
  Raw *stack = (Raw *)malloc(sizeof(Raw) * 8);
  if (!stack) {
    abort();
  }
  size_t depth = 1, capacity = 8;
  stack[0] = target;
  int made = 0, failed = 0;
  while (depth > 0 && !failed) {
    Raw next = stack[--depth];
    const uint16_t *units = tz_fs_raw_wide(&next);
    size_t length = tz_fs_raw_count(&next) - 1;
    DWORD error = tz_fs_make_dir(units);
    Failure f = tz_fs_win32_failure(error);
    if (error == 0) {
      if (!made) {
        tz_fs_raw_units(first, units, length);
        made = 1;
      }
    } else if (f.index == E_EACCES || f.index == E_ENOSPC || f.index == E_ENOTDIR ||
               f.index == E_EPERM) {
      failed = 1;
      *failure = f;
    } else if (f.index == E_ENOENT && (depth > 0 || last_separator(units, length) >= 0)) {
      ptrdiff_t cut = last_separator(units, length);
      if (cut >= 0) {
        if (depth + 2 > capacity) {
          capacity *= 2;
          Raw *bigger = (Raw *)realloc(stack, sizeof(Raw) * capacity);
          if (!bigger) {
            abort();
          }
          stack = bigger;
        }
        Raw parent = {NULL, 0, 0};
        tz_fs_raw_units(&parent, units, (size_t)cut);
        tz_fs_raw_unit(&parent, 0);
        stack[depth++] = next;
        stack[depth++] = parent;
        continue;
      }
    } else {
      if (f.index == E_ENOENT) {
        f = (Failure){E_EEXIST, tz_fs_errors[E_EEXIST].windows};
      }
      TzStatsParts st;
      DWORD stat_error = tz_fs_stat_of((const wchar_t *)units, 1, &st);
      if (stat_error == 0 && ((uint32_t)st.mode & S_KIND) != S_DIR) {
        failed = 1;
        *failure = f.index == E_EEXIST && depth > 0
                       ? (Failure){E_ENOTDIR, tz_fs_errors[E_ENOTDIR].windows}
                       : (Failure){E_EEXIST, tz_fs_errors[E_EEXIST].windows};
      } else if (stat_error) {
        failed = 1;
        *failure = tz_fs_win32_failure(stat_error);
      }
    }
    if (failed && at) {
      tz_fs_raw_append(at, next.data, next.length - 2);
    }
    tz_fs_raw_free(&next);
  }
  for (size_t i = 0; i < depth; ++i) {
    tz_fs_raw_free(&stack[i]);
  }
  free(stack);
  if (failed) {
    tz_fs_raw_free(first);
    return -1;
  }

  return made;
}
#else
int tz_fs_mkdir_raw(const uint8_t *path, size_t n, int32_t recursive, int32_t mode, Raw *first,
                    Failure *failure, Raw *at) {
  Raw target = {NULL, 0, 0};
  tz_fs_raw_append(&target, path, n);
  tz_fs_raw_append(&target, "", 1);
  if (!recursive) {
    int error = mkdir((const char *)target.data, (mode_t)mode) == 0 ? 0 : errno;
    tz_fs_raw_free(&target);
    if (!error) {
      return 0;
    }
    *failure = tz_fs_errno_failure(error);
    return -1;
  }
  Raw *stack = (Raw *)malloc(sizeof(Raw) * 8);
  if (!stack) {
    abort();
  }
  size_t depth = 1, capacity = 8;
  stack[0] = target;
  int made = 0, failed = 0, error_number = 0;
  while (depth > 0 && !failed) {
    Raw next = stack[--depth];
    int error = mkdir((const char *)next.data, (mode_t)mode) == 0 ? 0 : errno;
    if (error == 0) {
      if (!made) {
        tz_fs_raw_append(first, next.data, next.length - 1);
        made = 1;
      }
    } else if (error == EACCES || error == ENOSPC || error == ENOTDIR || error == EPERM) {
      failed = 1;
      error_number = error;
    } else if (error == ENOENT) {
      char *slash = strrchr((char *)next.data, '/');
      if (slash && slash != (char *)next.data) {
        if (depth + 2 > capacity) {
          capacity *= 2;
          Raw *bigger = (Raw *)realloc(stack, sizeof(Raw) * capacity);
          if (!bigger) {
            abort();
          }
          stack = bigger;
        }
        Raw parent = {NULL, 0, 0};
        tz_fs_raw_append(&parent, next.data, (size_t)(slash - (char *)next.data));
        tz_fs_raw_append(&parent, "", 1);
        stack[depth++] = next;
        stack[depth++] = parent;
        continue;
      }
      if (depth == 0) {
        failed = 1;
        error_number = EEXIST;
      }
    } else {
      struct stat st;
      if (stat((const char *)next.data, &st) == 0 && !S_ISDIR(st.st_mode)) {
        failed = 1;
        error_number = error == EEXIST && depth > 0 ? ENOTDIR : EEXIST;
      }
    }
    if (failed && at) {
      tz_fs_raw_append(at, next.data, next.length - 1);
    }
    tz_fs_raw_free(&next);
  }
  for (size_t i = 0; i < depth; ++i) {
    tz_fs_raw_free(&stack[i]);
  }
  free(stack);
  if (failed) {
    tz_fs_raw_free(first);
    *failure = tz_fs_errno_failure(error_number);
    return -1;
  }

  return made;
}
#endif

TzStr tz_fs_made_dir(const Raw *first) {
#ifdef _WIN32
  return tz_str_from_utf16(tz_fs_raw_wide(first), tz_fs_raw_count(first));
#else

  return tz_fs_str_of_utf8(first->data, first->length);
#endif
}

int32_t tz_fs_mkdir(TzStr *out, void *path_obj, uint64_t path_meta, int32_t recursive, int32_t mode,
                    TzMaker make) {
  out->obj = NULL;
  out->meta = 0;
  Path p;
  if (!tz_fs_path_init(&p, path_obj, path_meta, make, "mkdir")) {
    return 0;
  }
  Raw first = {NULL, 0, 0};
  Failure failure;
#ifdef _WIN32
  int made =
      tz_fs_mkdir_raw(p.wide.data, p.wide.length - 1, recursive, mode, &first, &failure, NULL);
#else
  int made =
      tz_fs_mkdir_raw(p.utf8.data, p.utf8.length - 1, recursive, mode, &first, &failure, NULL);
#endif
  if (made < 0) {
    tz_fs_throw_failure(make, failure, "mkdir", &p.shown, NULL);
    made = 0;
  } else if (made) {
    *out = tz_fs_made_dir(&first);
  }
  tz_fs_raw_free(&first);
  tz_fs_path_free(&p);

  return made;
}

void tz_fs_mkdtemp(TzStr *out, void *prefix_obj, uint64_t prefix_meta, TzMaker make) {
  static const char letters[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  out->obj = NULL;
  out->meta = 0;
  Path prefix;
  if (!tz_fs_path_init(&prefix, prefix_obj, prefix_meta, make, "mkdtemp")) {
    return;
  }
  for (int attempt = 0;; ++attempt) {
    TzBuf name;
    tz_buf_init(&name);
    tz_buf_units(&name, prefix.given.data, prefix.given.length);
    for (int i = 0; i < 6; ++i) {
      unsigned int r;
#ifdef _WIN32
      if (rand_s(&r) != 0) {
        r = (unsigned int)GetTickCount() * 2654435761u + (unsigned int)i;
      }
#else
      r = (unsigned int)rand() ^ (unsigned int)time(NULL) ^ ((unsigned int)getpid() << 8);
#endif
      tz_buf_push(&name, (uint16_t)letters[r % 62]);
    }
    Path dir;
    tz_fs_path_of_units(&dir, name.data, name.length);
#ifdef _WIN32
    DWORD error = tz_fs_make_dir(dir.wide.data);
    int exists = error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS;
    Failure f = tz_fs_win32_failure(error);
#else
    int error = mkdir(tz_fs_native(&dir), 0700) == 0 ? 0 : errno;
    int exists = error == EEXIST;
    Failure f = error ? tz_fs_errno_failure(error) : (Failure){0, 0};
#endif
    tz_fs_path_free(&dir);
    if (!error) {
      *out = tz_buf_take(&name);
      break;
    }
    if (!exists || attempt >= 100) {
      TzBuf shown;
      tz_buf_init(&shown);
      tz_buf_units(&shown, prefix.given.data, prefix.given.length);
      tz_buf_ascii(&shown, "XXXXXX");
      tz_fs_throw_failure(make, f, "mkdtemp", &shown, NULL);
      tz_buf_free(&shown);
      tz_buf_free(&name);
      break;
    }
    tz_buf_free(&name);
  }
  tz_fs_path_free(&prefix);
}

void tz_fs_rmdir(void *path_obj, uint64_t path_meta, TzMaker make) {
  Path p;
  if (!tz_fs_path_init(&p, path_obj, path_meta, make, "rmdir")) {
    return;
  }
#ifdef _WIN32
  if (!RemoveDirectoryW(tz_fs_wide(&p))) {
    tz_fs_fail_win32(make, GetLastError(), "rmdir", &p, NULL);
  }
#else
  if (rmdir(tz_fs_native(&p)) != 0) {
    tz_fs_fail_errno(make, errno, "rmdir", &p, NULL);
  }
#endif
  tz_fs_path_free(&p);
}

#ifdef _WIN32
DWORD tz_fs_unlink_path(const wchar_t *path) {
  DWORD attributes = GetFileAttributesW(path);
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return GetLastError();
  }
  if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
    if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      return ERROR_ACCESS_DENIED;
    }
    return RemoveDirectoryW(path) ? 0 : GetLastError();
  }
  if ((attributes & FILE_ATTRIBUTE_READONLY) &&
      !SetFileAttributesW(path, attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY)) {
    return GetLastError();
  }
  if (DeleteFileW(path)) {
    return 0;
  }
  DWORD error = GetLastError();
  if (attributes & FILE_ATTRIBUTE_READONLY) {
    SetFileAttributesW(path, attributes);
  }

  return error;
}

static DWORD remove_tree(Raw *path, int *depth) {
  size_t length = path->length;
  tz_fs_raw_unit(path, 0);
  DWORD attributes = GetFileAttributesW((const wchar_t *)path->data);
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    path->length = length;
    return GetLastError();
  }
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    DWORD error = tz_fs_unlink_path((const wchar_t *)path->data);
    path->length = length;
    return error;
  }
  path->length = length;
  static const uint16_t all[] = {'\\', '*', 0};
  tz_fs_raw_units(path, all, 3);
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileExW((const wchar_t *)path->data, FindExInfoBasic, &data,
                                 FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
  path->length = length;
  DWORD error = 0;
  int deepest = 0;
  if (find != INVALID_HANDLE_VALUE) {
    do {
      const wchar_t *name = data.cFileName;
      if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) {
        continue;
      }
      tz_fs_raw_unit(path, '\\');
      tz_fs_raw_units(path, (const uint16_t *)name, wcslen(name));
      int below = 0;
      error = remove_tree(path, &below);
      if (below > deepest) {
        deepest = below;
      }
      path->length = length;
    } while (!error && FindNextFileW(find, &data));
    FindClose(find);
  }
  *depth = deepest + 1;
  if (error) {
    return error;
  }
  tz_fs_raw_unit(path, 0);
  if ((attributes & FILE_ATTRIBUTE_READONLY)) {
    SetFileAttributesW((const wchar_t *)path->data, attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY);
  }
  if (!RemoveDirectoryW((const wchar_t *)path->data)) {
    error = GetLastError();
  }
  path->length = length;

  return error;
}
#else
static int remove_tree(Raw *path, int *depth) {
  size_t length = path->length;
  struct stat s;
  if (lstat((const char *)path->data, &s) != 0) {
    return errno;
  }
  if (!S_ISDIR(s.st_mode)) {
    return unlink((const char *)path->data) == 0 ? 0 : errno;
  }
  DIR *dir = opendir((const char *)path->data);
  if (!dir) {
    return errno;
  }
  int error = 0, deepest = 0;
  struct dirent *d;
  while (!error && (d = readdir(dir)) != NULL) {
    if (d->d_name[0] == '.' && (d->d_name[1] == 0 || (d->d_name[1] == '.' && d->d_name[2] == 0))) {
      continue;
    }
    path->length = length - 1;
    tz_fs_raw_append(path, "/", 1);
    tz_fs_raw_append(path, d->d_name, strlen(d->d_name) + 1);
    int below = 0;
    error = remove_tree(path, &below);
    if (below > deepest) {
      deepest = below;
    }
  }
  closedir(dir);
  *depth = deepest + 1;
  path->length = length - 1;
  tz_fs_raw_append(path, "", 1);
  if (!error && rmdir((const char *)path->data) != 0) {
    error = errno;
  }

  return error;
}
#endif

void tz_fs_unlink(void *path_obj, uint64_t path_meta, TzMaker make) {
  Path p;
  if (!tz_fs_path_init(&p, path_obj, path_meta, make, "unlink")) {
    return;
  }
#ifdef _WIN32
  DWORD error = tz_fs_unlink_path(tz_fs_wide(&p));
  if (error) {
    tz_fs_fail_win32(make, error, "unlink", &p, NULL);
  }
#else
  if (unlink(tz_fs_native(&p)) != 0) {
    tz_fs_fail_errno(make, errno, "unlink", &p, NULL);
  }
#endif
  tz_fs_path_free(&p);
}

int tz_fs_rm_raw(const Path *p, int32_t recursive, int32_t force, Failure *failure,
                 const char **syscall, int *depth) {
  TzStatsParts st;
  *depth = 0;
#ifdef _WIN32
  DWORD error = tz_fs_stat_of(tz_fs_wide(p), 0, &st);
  Failure missing = tz_fs_win32_failure(error);
#else
  int error = tz_fs_stat_of(tz_fs_native(p), 0, &st);
  Failure missing = error ? tz_fs_errno_failure(error) : (Failure){0, 0};
#endif
  if (error) {
    if (force && (missing.index == E_ENOENT || missing.index == E_ENOTDIR)) {
      return RM_DONE;
    }
    *failure = missing;
    *syscall = "lstat";
    return RM_FAILED;
  }
  if (((uint32_t)st.mode & S_KIND) == S_DIR && !recursive) {
    return RM_DIRECTORY;
  }
  Raw tree = {NULL, 0, 0};
#ifdef _WIN32
  tz_fs_raw_units(&tree, p->wide.data, p->wide.length - 1);
  error = remove_tree(&tree, depth);
  tz_fs_raw_free(&tree);
  if (error && !(force && tz_fs_win32_failure(error).index == E_ENOENT)) {
    *failure = tz_fs_win32_failure(error);
    *syscall = "rm";
    return RM_FAILED;
  }
#else
  tz_fs_raw_append(&tree, p->utf8.data, p->utf8.length);
  error = remove_tree(&tree, depth);
  tz_fs_raw_free(&tree);
  if (error && !(force && error == ENOENT)) {
    *failure = tz_fs_errno_failure(error);
    *syscall = "rm";
    return RM_FAILED;
  }
#endif

  return RM_DONE;
}

void tz_fs_throw_rm_directory(const Path *p, TzMaker make) {
  TzBuf m;
  tz_buf_init(&m);
  tz_buf_ascii(&m, "Path is a directory: rm returned EISDIR (is a directory) ");
  tz_buf_units(&m, p->given.data, p->given.length);
  TzSystemErrorParts parts;
  memset(&parts, 0, sizeof parts);
  parts.message = tz_buf_take(&m);
  parts.name = tz_fs_ascii("SystemError");
  parts.errno_value = 21;
  parts.code = tz_fs_ascii("ERR_FS_EISDIR");
  parts.syscall = tz_fs_ascii("rm");
  parts.path = tz_fs_opt_of(&p->given);
  tz_fs_throw_parts(make, &parts);
}

void tz_fs_rm(void *path_obj, uint64_t path_meta, int32_t recursive, int32_t force, TzMaker make) {
  Path p;
  if (!tz_fs_path_init(&p, path_obj, path_meta, make, "rm")) {
    return;
  }
  Failure failure;
  const char *syscall = NULL;
  int depth;
  switch (tz_fs_rm_raw(&p, recursive, force, &failure, &syscall, &depth)) {
  case RM_FAILED:
    tz_fs_throw_failure(make, failure, syscall, &p.shown, NULL);
    break;
  case RM_DIRECTORY:
    tz_fs_throw_rm_directory(&p, make);
    break;
  default:
    break;
  }
  tz_fs_path_free(&p);
}
