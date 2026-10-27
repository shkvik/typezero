#ifdef _WIN32
#define _CRT_RAND_S
#else
#define _DEFAULT_SOURCE
#endif

#include "runtime/library/fs/stdin/stdin.h"
#include "runtime/library/fs/fs/fs.h"
#include "runtime/library/fs/readline/readline.h"
#include "runtime/core/async/async.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/group/group.h"
#include "runtime/core/output/output.h"
#include "runtime/core/string/string.h"
#include "runtime/library/bytes/buffer/buffer.h"
#include "runtime/library/bytes/bytes/bytes.h"
#include <stddef.h>
#include <stdint.h>
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

Bytes tz_in;

int tz_in_eof;

#ifdef _WIN32
uint16_t tz_in_surrogate;
#endif

static int in_fill(void) {
  if (tz_in_eof) {
    return 0;
  }
#ifdef _WIN32
  HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode;
  if (h == NULL || h == INVALID_HANDLE_VALUE) {
    tz_in_eof = 1;
    return 0;
  }
  if (GetConsoleMode(h, &mode)) {
    wchar_t text[4096];
    DWORD got = 0;
    if (!ReadConsoleW(h, text, 4096, &got, NULL) || got == 0 || text[0] == 0x1A) {
      tz_in_eof = 1;
      return 0;
    }
    uint16_t units[4097];
    size_t n = 0;
    if (tz_in_surrogate) {
      units[n++] = tz_in_surrogate;
    }
    tz_in_surrogate = 0;
    for (DWORD i = 0; i < got; ++i) {
      units[n++] = text[i];
    }
    if (n > 0 && units[n - 1] >= 0xD800 && units[n - 1] <= 0xDBFF) {
      tz_in_surrogate = units[--n];
    }
    tz_fs_bytes_utf8(&tz_in, units, n);
    return 1;
  }
  tz_fs_bytes_reserve(&tz_in, 65536);
  DWORD got = 0;
  if (!ReadFile(h, tz_in.data + tz_in.length, 65536, &got, NULL) || got == 0) {
    tz_in_eof = 1;
    return 0;
  }
  tz_in.length += got;

  return 1;
#else
  tz_fs_bytes_reserve(&tz_in, 65536);
  for (;;) {
    ssize_t got = read(0, tz_in.data + tz_in.length, 65536);
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got <= 0) {
      tz_in_eof = 1;
      return 0;
    }
    tz_in.length += (size_t)got;
    return 1;
  }
#endif
}

int32_t tz_io_read_line(TzStr *out, void *question_obj, uint64_t question_meta,
                        int32_t has_question) {
  if (has_question) {
    tz_stdout_write(question_obj, question_meta);
  }
  tz_out_flush(1);
  size_t scanned = 0;
  for (;;) {
    for (size_t i = scanned; i < tz_in.length; ++i) {
      if (tz_in.data[i] != '\n') {
        continue;
      }
      size_t end = i > 0 && tz_in.data[i - 1] == '\r' ? i - 1 : i;
      *out = tz_fs_str_of_utf8(tz_in.data, end);
      tz_fs_bytes_consume(&tz_in, i + 1);

      return 1;
    }
    scanned = tz_in.length;
    if (!in_fill()) {
      if (tz_in.length == 0) {
        out->obj = NULL;
        out->meta = 0;
        return 0;
      }
      *out = tz_fs_str_of_utf8(tz_in.data, tz_in.length);
      tz_in.length = 0;
      return 1;
    }
  }
}

void tz_fs_read_stdin(Bytes *data) {
  tz_out_flush(1);
  while (in_fill()) {
  }
  tz_fs_bytes_append(data, tz_in.data, tz_in.length);
  tz_in.length = 0;
}

typedef struct ChunkWaiter {
  struct ChunkWaiter *next;
  TzPromise *promise;
  int32_t max;
} ChunkWaiter;

ChunkWaiter *tz_chunk_head, *tz_chunk_tail;

void tz_fs_stdin_serve_chunks(void) {
  while (tz_chunk_head && (tz_in.length > 0 || tz_in_eof)) {
    ChunkWaiter *w = tz_chunk_head;
    tz_chunk_head = w->next;
    if (!tz_chunk_head) {
      tz_chunk_tail = NULL;
    }
    TzBytes *chunk = NULL;
    if (tz_in.length > 0) {
      size_t n = tz_in.length < (size_t)w->max ? tz_in.length : (size_t)w->max;
      chunk = tz_buffer_copy_of(tz_in.data, (int64_t)n);
      tz_fs_bytes_consume(&tz_in, n);
    }
    memcpy((char *)w->promise + TZ_PROMISE_VALUE, &chunk, sizeof chunk);
    tz_promise_fulfill(w->promise);
    tz_obj_release((TzObject *)w->promise);
    free(w);
  }
  if (tz_chunk_head && !tz_in_eof) {
    tz_fs_stdin_read(1);
  }
}

void tz_stdin_chunk(TzPromise *p, int64_t max) {
  ChunkWaiter *w = (ChunkWaiter *)calloc(1, sizeof(ChunkWaiter));
  if (!w) {
    abort();
  }
  w->promise = p;
  tz_rc_inc(&p->rc);
  w->max = max > 0 ? max : 65536;
  if (tz_chunk_tail) {
    tz_chunk_tail->next = w;
  } else {
    tz_chunk_head = w;
  }
  tz_chunk_tail = w;
  tz_fs_stdin_serve_chunks();
}
