#ifndef TZ_CLOSURE_H
#define TZ_CLOSURE_H

#include <stddef.h>
#include <stdint.h>

typedef struct TzObjectDesc {
  void (*drop)(void *object);
} TzObjectDesc;

typedef struct TzObject {
  int64_t rc;
  const TzObjectDesc *desc;
} TzObject;

typedef struct {
  const struct TzFunctionInfo *info;
  TzObject *env;
} TzFunction;

typedef struct TzFunctionInfo {
  void *code;
  struct {
    void *obj;
    uint64_t meta;
  } name;
} TzFunctionInfo;

/* An object made immortal (Interned, a copyable module constant) is never freed and is no leak. */

#endif
