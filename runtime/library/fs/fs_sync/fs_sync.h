#ifndef TZ_FS_SYNC_H
#define TZ_FS_SYNC_H

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

enum {
  S_KIND = 0170000,
  S_REG = 0100000,
  S_DIR = 0040000,
  S_LNK = 0120000,
  S_CHR = 0020000,
  S_BLK = 0060000,
  S_FIFO = 0010000,
  S_SOCK = 0140000,
};

#ifdef _WIN32
DWORD tz_fs_stat_of(const wchar_t *path, int follow, TzStatsParts *st);
#else
int tz_fs_stat_of(const char *path, int follow, TzStatsParts *st);
#endif

#endif
