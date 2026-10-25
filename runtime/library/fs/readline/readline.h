// node:readline over stdin (docs/io.md, step 4).

#ifndef TZ_READLINE_H
#define TZ_READLINE_H

#include <stddef.h>
#include <stdint.h>
#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Releases what stdin gave, before the leak check.
void tz_fs_shutdown(void);
#endif

// Reads more of stdin, unless a read is in flight; with `ref`, it keeps the
// program alive.
void tz_fs_stdin_read(int ref);

#endif
