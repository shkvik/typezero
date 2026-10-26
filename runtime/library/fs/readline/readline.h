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

void tz_fs_shutdown(void);
#endif

void tz_fs_stdin_read(int ref);

#endif
