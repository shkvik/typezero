#ifdef _WIN32
#define _CRT_RAND_S
#else
#define _DEFAULT_SOURCE
#endif

#include "runtime/library/fs/readline/readline.h"
#include "runtime/library/fs/fs/fs.h"
#include "runtime/library/fs/fs_errors/fs_errors.h"
#include "runtime/library/fs/stdin/stdin.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/async/async.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/error/error.h"
#include "runtime/core/group/group.h"
#include "runtime/core/io/io.h"
#include "runtime/core/output/output.h"
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

typedef struct StdinRead {
  TzWork work;
  Raw data;
  int32_t ended;
  int32_t failed;
  int32_t console;
} StdinRead;

StdinRead *tz_stdin_read;

typedef struct Asker {
  struct Asker *next;
  TzPromise *promise;
  int32_t step;
  TzMaker make;
} Asker;

typedef struct Readline {
  TzInstance base;
  int32_t output;
  int32_t closed;
  int32_t skip_lf;
  Asker *head, *tail;
} Readline;

Readline **tz_readlines;

size_t tz_readline_count, tz_readline_capacity;

static void readline_drop(void *object);

static void readline_step(void *object, TzPromise *p);

static void readline_close_step(void *object, TzPromise *p);

const TzAsyncIterLayout tz_readline_layout = {
    {readline_drop, NULL, 0, TZ_LAYOUT_ASYNC_ITERATOR, "Interface", 9, 0},
    readline_step,
    readline_close_step};

static void stdin_read_work(TzWork *w) {
  StdinRead *r = (StdinRead *)w;
#ifdef _WIN32
  HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode;
  if (h == NULL || h == INVALID_HANDLE_VALUE) {
    r->ended = 1;
    return;
  }
  if (GetConsoleMode(h, &mode)) {
    r->console = 1;
    tz_fs_raw_reserve(&r->data, 4096 * 2);
    DWORD got = 0;
    if (!ReadConsoleW(h, (wchar_t *)r->data.data, 4096, &got, NULL) || got == 0 ||
        ((const uint16_t *)r->data.data)[0] == 0x1A) {
      r->ended = 1;
    } else {
      r->data.length = (size_t)got * 2;
    }
    return;
  }
  tz_fs_raw_reserve(&r->data, 65536);
  DWORD got = 0;
  BOOL read = ReadFile(h, r->data.data, 65536, &got, NULL);
  if (!read || got == 0) {
    DWORD failure = read ? ERROR_SUCCESS : GetLastError();
    r->ended = 1;
    r->failed = failure != ERROR_SUCCESS && failure != ERROR_BROKEN_PIPE &&
                failure != ERROR_HANDLE_EOF && failure != ERROR_NO_DATA;
  } else {
    r->data.length = got;
  }
#else
  tz_fs_raw_reserve(&r->data, 65536);
  for (;;) {
    ssize_t got = read(0, r->data.data, 65536);
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got <= 0) {
      r->ended = 1;
      r->failed = got < 0;
    } else {
      r->data.length = (size_t)got;
    }
    return;
  }
#endif
}

static void readline_serve(Readline *rl);

static void stdin_read_done(TzWork *w) {
  StdinRead *r = (StdinRead *)w;
  tz_stdin_read = NULL;
  if (r->ended) {
    tz_in_eof = 1;
    tz_in_failed = r->failed;
  } else if (r->console) {
#ifdef _WIN32
    const uint16_t *units = (const uint16_t *)r->data.data;
    size_t n = r->data.length / 2;
    uint16_t joined[4097];
    size_t k = 0;
    if (tz_in_surrogate) {
      joined[k++] = tz_in_surrogate;
    }
    tz_in_surrogate = 0;
    for (size_t i = 0; i < n; ++i) {
      joined[k++] = units[i];
    }
    if (k > 0 && joined[k - 1] >= 0xD800 && joined[k - 1] <= 0xDBFF) {
      tz_in_surrogate = joined[--k];
    }
    tz_fs_bytes_utf8(&tz_in, joined, k);
#endif
  } else {
    tz_fs_bytes_reserve(&tz_in, r->data.length);
    memcpy(tz_in.data + tz_in.length, r->data.data, r->data.length);
    tz_in.length += r->data.length;
  }
  tz_fs_raw_free(&r->data);
  free(r);
  for (size_t i = 0; i < tz_readline_count; ++i) {
    readline_serve(tz_readlines[i]);
  }
  tz_fs_stdin_serve_chunks();
}

void tz_fs_stdin_read(int ref) {
  if (tz_stdin_read) {
    if (ref) {
      tz_work_ref(&tz_stdin_read->work);
    }
    return;
  }
  tz_out_flush(1);
  StdinRead *r = (StdinRead *)calloc(1, sizeof(StdinRead));
  if (!r) {
    abort();
  }
  r->work.work = stdin_read_work;
  r->work.done = stdin_read_done;
  r->work.unref = !ref;
  tz_stdin_read = r;
  tz_work_queue(&r->work);
}

static int line_end(size_t start, size_t *end, size_t *next) {
  const uint8_t *s = tz_in.data;
  size_t n = tz_in.length;
  for (size_t i = start; i < n; ++i) {
    if (s[i] == '\n') {
      *end = i;
      *next = i + 1;
      return 1;
    }
    if (s[i] == '\r') {
      *end = i;
      *next = i + 1 < n && s[i + 1] == '\n' ? i + 2 : i + 1;
      return 1;
    }
    if (s[i] == 0xE2 && i + 2 < n && s[i + 1] == 0x80 && (s[i + 2] == 0xA8 || s[i + 2] == 0xA9)) {
      *end = i;
      *next = i + 3;
      return 1;
    }
  }

  return 0;
}

static int take_line(Readline *rl, TzStr *line) {
  size_t start = 0;
  if (rl->skip_lf && tz_in.length > 0) {
    if (tz_in.data[0] == '\n') {
      start = 1;
    }
    rl->skip_lf = 0;
  }
  size_t end, next;
  if (line_end(start, &end, &next)) {
    *line = tz_fs_str_of_utf8(tz_in.data + start, end - start);
    rl->skip_lf = tz_in.data[end] == '\r' && next == end + 1;
    tz_fs_bytes_consume(&tz_in, next);
    return 1;
  }
  if (!tz_in_eof) {
    if (start) {
      tz_fs_bytes_consume(&tz_in, start);
    }
    return 0;
  }
  if (tz_in.length == start) {
    tz_in.length = 0;
    return 0;
  }
  *line = tz_fs_str_of_utf8(tz_in.data + start, tz_in.length - start);
  tz_in.length = 0;

  return 1;
}

static void asker_free(Asker *a) {
  tz_obj_release((TzObject *)a->promise);
  tz_free(a);
}

static void readline_shut(Readline *rl) {
  if (rl->closed) {
    return;
  }
  rl->closed = 1;
  Asker **link = &rl->head;
  while (*link) {
    Asker *a = *link;
    if (!a->step) {
      link = &a->next;
      continue;
    }
    *link = a->next;
    if (rl->tail == a) {
      rl->tail = NULL;
    }
    a->promise->flags |= TZ_PROMISE_DONE;
    tz_promise_fulfill(a->promise);
    asker_free(a);
  }
  rl->tail = NULL;
  for (Asker *a = rl->head; a; a = a->next) {
    rl->tail = a;
  }
  if (tz_stdin_read && !tz_stdin_read->work.unref) {
    int needed = 0;
    for (size_t i = 0; i < tz_readline_count && !needed; ++i) {
      needed = tz_readlines[i] != rl && !tz_readlines[i]->closed && tz_readlines[i]->head;
    }
    if (!needed) {
      tz_work_unref(&tz_stdin_read->work);
    }
  }
}

static void readline_serve(Readline *rl) {
  ++rl->base.rc;
  while (rl->head && !rl->closed) {
    TzStr line;
    if (!take_line(rl, &line)) {
      if (tz_in_eof) {
        readline_shut(rl);
      } else {
        tz_fs_stdin_read(1);
      }
      break;
    }
    Asker *a = rl->head;
    rl->head = a->next;
    if (!rl->head) {
      rl->tail = NULL;
    }
    tz_promise_succeed(a->promise, &line, sizeof line);
    asker_free(a);
  }
  tz_instance_release(&rl->base);
}

static Asker *readline_ask(Readline *rl, TzPromise *p, int32_t step) {
  Asker *a = (Asker *)tz_alloc(sizeof(Asker));
  memset(a, 0, sizeof *a);
  a->promise = p;
  tz_ref_retain(p);
  a->step = step;
  if (rl->tail) {
    rl->tail->next = a;
  } else {
    rl->head = a;
  }
  rl->tail = a;

  return a;
}

void *tz_readline_new(int64_t output) {
  Readline *rl = (Readline *)tz_obj_alloc(sizeof(Readline));
  memset(rl, 0, sizeof *rl);
  rl->base.rc = 1;
  rl->base.layout = &tz_readline_layout.base;
  rl->output = output;
  if (tz_readline_count == tz_readline_capacity) {
    size_t capacity = tz_readline_capacity * 2 + 4;
    Readline **grown = (Readline **)tz_alloc(sizeof(Readline *) * capacity);
    if (tz_readline_count) {
      memcpy(grown, tz_readlines, sizeof(Readline *) * tz_readline_count);
    }
    tz_free(tz_readlines);
    tz_readlines = grown;
    tz_readline_capacity = capacity;
  }
  tz_readlines[tz_readline_count++] = rl;

  return rl;
}

static void readline_drop(void *object) {
  TZ_ORDERED();
  Readline *rl = (Readline *)object;
  for (size_t i = 0; i < tz_readline_count; ++i) {
    if (tz_readlines[i] == rl) {
      tz_readlines[i] = tz_readlines[--tz_readline_count];
      break;
    }
  }
  if (tz_readline_count == 0) {
    tz_free(tz_readlines);
    tz_readlines = NULL;
    tz_readline_capacity = 0;
  }
  readline_shut(rl);
  while (rl->head) {
    Asker *a = rl->head;
    rl->head = a->next;
    asker_free(a);
  }
  tz_obj_free(rl);
}

static void readline_step(void *object, TzPromise *p) {
  Readline *rl = (Readline *)object;
  if (rl->closed) {
    p->flags |= TZ_PROMISE_DONE;
    tz_promise_fulfill(p);
    return;
  }
  readline_ask(rl, p, 1);
  readline_serve(rl);
}

static void readline_close_step(void *object, TzPromise *p) {
  readline_shut((Readline *)object);
  tz_promise_fulfill(p);
}

void tz_readline_close(void *object) { readline_shut((Readline *)object); }

static void question_failed(TzPromise *p, TzMaker make) {
  if (!make) {
    return;
  }
  TzFsErrorParts parts;
  memset(&parts, 0, sizeof parts);
  parts.code = "ERR_USE_AFTER_CLOSE";
  parts.message = tz_fs_ascii("readline was closed");
  void *error = make(&parts);
  tz_promise_fail(p, error);
}

void tz_readline_question(void *object, TzPromise *p, void *query_obj, uint64_t query_meta,
                          TzMaker make) {
  Readline *rl = (Readline *)object;
  if (rl->closed) {
    question_failed(p, make);
    return;
  }
  if (rl->output == 1) {
    tz_stdout_write(query_obj, query_meta);
  } else if (rl->output == 2) {
    tz_stderr_write(query_obj, query_meta);
  }
  Asker *a = readline_ask(rl, p, 0);
  a->make = make;
  readline_serve(rl);
}

void tz_fs_shutdown(void) { tz_fs_bytes_free(&tz_in); }
