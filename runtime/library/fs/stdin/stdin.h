// stdin: what readLine of tz:io, readFileSync(0), node:readline and
// process.stdin read, from one buffer.

#ifndef TZ_STDIN_H
#define TZ_STDIN_H

#include "runtime/library/fs/fs/fs.h"
#include <stddef.h>
#include <stdint.h>
#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// What stdin gave that nobody took yet, as UTF-8, and whether it ended.
// Not static: see the rules in tzrt.h.
extern Bytes tz_in;

extern int tz_in_eof;

#ifdef _WIN32
extern uint16_t tz_in_surrogate;
#endif

// Everything left in stdin, into `data`.
void tz_fs_read_stdin(Bytes *data);

void tz_fs_stdin_serve_chunks(void);

#endif
