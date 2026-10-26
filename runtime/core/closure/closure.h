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

int64_t tz_obj_live_objects(void);
void *tz_obj_alloc(int64_t size);
void tz_obj_free(void *object);

#endif
