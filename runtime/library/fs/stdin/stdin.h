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

extern Bytes tz_in;

extern int tz_in_eof;
extern int tz_in_failed;

int32_t tz_stdin_failed(void);

#ifdef _WIN32
extern uint16_t tz_in_surrogate;
#endif

void tz_fs_read_stdin(Bytes *data);

void tz_fs_stdin_serve_chunks(void);

#endif
