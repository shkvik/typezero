// node:readline over stdin (docs/io.md, step 4).

#ifdef _WIN32
#define _CRT_RAND_S // rand_s
#else
#define _DEFAULT_SOURCE // lstat, realpath, d_type and the like, which -std=c17 hides
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

// An Interface of node:readline over stdin (docs/io.md, step 4). Its lines
// come from the buffer that readLine of tz:io reads (tz_in), as they are
// asked for, by a question or by a step of `for await`, in the order they
// are asked. What stdin does not have yet is read on a thread of the pool,
// a chunk at a time, while someone waits for a line; that read keeps the
// program alive. The lines end as Node's do: at "\n", "\r\n", a lone "\r",
// U+2028 or U+2029; the last line may end with the input.
//
// Unlike Node, which gives each line to whoever listens when its chunk
// comes and drops it if nobody does, a line waits here until it is asked
// for: `await rl.question(a)` after another question gets the next line of
// a file, as it does from a terminal. And an Interface nobody asks for
// lines reads nothing, so it does not keep the program waiting for the end
// of the input.

// A read of stdin on a thread of the pool: what it gave, in memory of its
// own. On Windows a console gives UTF-16, which the main thread puts
// together as readLine does.
typedef struct StdinRead {
  TzWork work;
  Raw data;
  int32_t ended;   // nothing more: the end of the input, or an error
  int32_t console; // `data` holds UTF-16 units of a console
} StdinRead;

// Not static: see the rules in tzrt.h. The read in flight, if any.
StdinRead *tz_stdin_read;

extern TzErrorInfo tz_error_info;

// Someone waiting for a line: a step of `for await` (a promise of the line,
// or done at the end) or a question (a promise of the line).
typedef struct Asker {
  struct Asker *next;
  TzPromise *promise; // held
  int32_t step;
  TzAbortListener listener; // of the question's signal
  TzSignal *signal;         // held
  struct Readline *readline;
} Asker;

typedef struct Readline {
  TzInstance base;
  int32_t output; // where questions go: 1 stdout, 2 stderr, 0 nowhere
  int32_t closed;
  int32_t skip_lf;  // a line ended with "\r": a "\n" right after it goes with it
  TzSignal *signal; // of createInterface: an abort closes it
  TzAbortListener listener;
  Asker *head, *tail;
} Readline;

// All the interfaces there are, so that the reads find them.
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
      r->ended = 1; // a line that starts with Ctrl+Z ends the input
    } else {
      r->data.length = (size_t)got * 2;
    }
    return;
  }
  tz_fs_raw_reserve(&r->data, 65536);
  DWORD got = 0;
  if (!ReadFile(h, r->data.data, 65536, &got, NULL) || got == 0) {
    r->ended = 1;
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
  } else if (r->console) {
#ifdef _WIN32
    // As in_fill: a pair of surrogates the console split waits for its half.
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
  // The interfaces take their lines, in the order they were made; then
  // process.stdin as a stream takes what is left.
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
  // What waits to be written goes first: it may be the question.
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

// The end of the next line in tz_in, from `start`: where it ends and where
// the next starts; false if it has not come yet.
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

// The next line, if it is there (or the rest at the end of the input):
// false otherwise.
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
  tz_signal_unlisten(&a->listener);
  if (a->signal) {
    tz_instance_release((TzInstance *)a->signal);
  }
  tz_obj_release((TzObject *)a->promise);
  tz_free(a);
}

static void readline_unlink(Readline *rl, Asker *a) {
  Asker **link = &rl->head;
  Asker *prev = NULL;
  while (*link && *link != a) {
    prev = *link;
    link = &(*link)->next;
  }
  if (!*link) {
    return;
  }
  *link = a->next;
  if (rl->tail == a) {
    rl->tail = prev;
  }
  a->next = NULL;
}

// close(), the end of the input or an abort: the steps of `for await` are
// done; questions wait for nothing more, as in Node, where they never
// settle then.
static void readline_shut(Readline *rl) {
  if (rl->closed) {
    return;
  }
  rl->closed = 1;
  tz_signal_unlisten(&rl->listener);
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
  // A read in flight no longer keeps the program alive for it.
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

// Gives the lines there are to those who wait, then reads more if someone
// still does.
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
    *(TzStr *)((char *)a->promise + TZ_PROMISE_VALUE) = line;
    tz_promise_fulfill(a->promise);
    asker_free(a);
  }
  tz_instance_release(&rl->base);
}

static Asker *readline_ask(Readline *rl, TzPromise *p, int32_t step) {
  Asker *a = (Asker *)tz_alloc(sizeof(Asker));
  memset(a, 0, sizeof *a);
  a->promise = p;
  tz_ref_retain(p); // it may be a member of a group (async.c)
  a->step = step;
  a->readline = rl;
  if (rl->tail) {
    rl->tail->next = a;
  } else {
    rl->head = a;
  }
  rl->tail = a;

  return a;
}

static void readline_aborted(TzAbortListener *l) {
  readline_shut((Readline *)((char *)l - offsetof(Readline, listener)));
}

void *tz_readline_new(int32_t output, TzSignal *signal) {
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
  if (signal) {
    if (signal->aborted) {
      rl->closed = 1;
    } else {
      rl->signal = signal;
      tz_ref_retain(signal); // a member of a group (signal.c)
      rl->listener.run = readline_aborted;
      tz_signal_listen(signal, &rl->listener);
    }
  }

  return rl;
}

static void readline_drop(void *object) {
  // It leaves lists of the runtime: in node's order (parallel.c).
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
  if (rl->signal) {
    tz_instance_release((TzInstance *)rl->signal);
  }
  tz_obj_free(rl);
}

// A step of `for await (const line of rl)`.
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

// `for await` left early closes the interface, as in Node.
static void readline_close_step(void *object, TzPromise *p) {
  readline_shut((Readline *)object);
  tz_promise_fulfill(p);
}

void tz_readline_close(void *object) { readline_shut((Readline *)object); }

static void question_aborted(TzAbortListener *l) {
  Asker *a = (Asker *)((char *)l - offsetof(Asker, listener));
  Readline *rl = a->readline;
  readline_unlink(rl, a);
  tz_promise_reject(a->promise, tz_abort_error(a->signal));
  asker_free(a);
}

// rl.question(query, { signal }): the query is written to the output, then
// the next line answers it.
void tz_readline_question(void *object, TzPromise *p, void *query_obj, uint64_t query_meta,
                          TzSignal *signal) {
  Readline *rl = (Readline *)object;
  if (rl->closed) {
    TzStr message = tz_fs_ascii("readline was closed");
    void *error = tz_error_info.make(TZ_ERROR, message.obj, message.meta);
    tz_promise_reject(p, error);
    return;
  }
  if (signal && signal->aborted) {
    tz_promise_reject(p, tz_abort_error(signal));
    return;
  }
  if (rl->output == 1) {
    tz_stdout_write(query_obj, query_meta);
  } else if (rl->output == 2) {
    tz_stderr_write(query_obj, query_meta);
  }
  Asker *a = readline_ask(rl, p, 0);
  if (signal) {
    a->signal = signal;
    tz_ref_retain(signal); // a member of a group (signal.c)
    a->listener.run = question_aborted;
    tz_signal_listen(signal, &a->listener);
  }
  readline_serve(rl);
}

// Releases what the runtime holds, before the leak check.
void tz_fs_shutdown(void) { tz_fs_bytes_free(&tz_in); }
