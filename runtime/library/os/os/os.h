#ifndef TZ_OS_H
#define TZ_OS_H

#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
  TzStr model;
  double speed, user, nice, sys, idle, irq;
} TzCpuParts;

typedef struct {
  int32_t uid, gid;
  TzStr username, homedir;
  TzOptStr shell;
} TzUserInfoParts;

#endif
