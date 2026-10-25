// node:os.

#ifndef TZ_OS_H
#define TZ_OS_H

#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>

// CpuInfo of os.cpus(), with the fields of its `times` in place.
typedef struct {
  TzStr model;
  double speed, user, nice, sys, idle, irq;
} TzCpuParts;

// os.userInfo().
typedef struct {
  int32_t uid, gid;
  TzStr username, homedir;
  TzOptStr shell;
} TzUserInfoParts;

#endif
