#ifndef TZ_JSON_H
#define TZ_JSON_H

#include "runtime/core/array/array.h"
#include "runtime/core/group/group.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

enum {
  TZ_JSON_NULL = 0,
  TZ_JSON_BOOL = 1,
  TZ_JSON_NUMBER = 2,
  TZ_JSON_STRING = 3,
  TZ_JSON_ARRAY = 4,
  TZ_JSON_OBJECT = 5,
};

typedef struct TzJson TzJson;

typedef struct {
  TzStr key;
  TzJson *value;
} TzJsonEntry;

struct TzJson {
  int64_t rc;
  uint32_t kind;
  uint32_t flags;
  union {
    int32_t boolean;
    struct {
      double value;
      TzStr text;
    } number;
    TzStr string;
    TzArray *array;
    struct {
      TzJsonEntry *entries;
      uint32_t count, capacity;
      uint32_t *index;
      uint32_t index_size;
    } object;
  } u;
};

extern const TzType tz_json_type;
int64_t tz_json_live_objects(void);
static inline void tz_json_release(TzJson *j) {
  if (j && tz_rc_dec(&j->rc)) {
  }
}

#define F_TEXT 2u

extern TzJson tz_json_null_node;

extern TzJson tz_json_true_node;

extern TzJson tz_json_false_node;



#if TZ_PARALLEL
void tz_json_publish(TzJson *root);
#endif


typedef struct {
  TzStr flat;
  const void *units;
  uint32_t length;
  int two;
} View;










#endif
