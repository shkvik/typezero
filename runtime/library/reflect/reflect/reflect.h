#ifndef TZ_REFLECT_H
#define TZ_REFLECT_H

#include "runtime/core/array/array.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/core/unknown/unknown.h"
#include "runtime/library/json/json/json.h"

typedef struct TzReflectMethod {
  const char *name;
  uint32_t name_length;
  const TzType *type;
  const TzFunctionInfo *info;
} TzReflectMethod;

typedef struct TzClassInfo {
  void *(*construct)(void *const *args, uint32_t count);
  void *(*from_json)(const TzJson *json, const void *path);
  const TzReflectMethod *methods;
  uint32_t method_count;
} TzClassInfo;

typedef void *(*TzInvoker)(const TzFunction *f, void *self, void *const *args, uint32_t count);
typedef struct TzInvokerEntry {
  const TzType *type;
  TzInvoker invoke;
} TzInvokerEntry;

void tz_reflect_init(TzClass *const *classes, uint32_t count, const TzInvokerEntry *invokers,
                     uint32_t invoker_count);
void *tz_reflect_new(const TzClass *cls, void *const *args, uint32_t count);
void *tz_reflect_construct(const TzClass *cls, const TzArray *args);
void *tz_reflect_make(void *(*construct)(void *const *args, uint32_t count), const TzArray *args);
void *tz_reflect_get(const void *target, void *key_obj, uint64_t key_meta);
void *tz_reflect_apply(const void *f, void *self, const TzArray *args);
void tz_class_name(TzStr *out, const TzClass *cls);
void *tz_reflect_from_json(const TzClass *cls, const TzJson *json);
TZ_COLD void tz_reflect_mismatch(const char *what, uint32_t what_length, int32_t index,
                                 const char *expected, uint32_t expected_length, const void *got);
TZ_COLD void tz_reflect_arity(const char *what, uint32_t what_length, int32_t min, int32_t max,
                              uint32_t count);
int32_t tz_unknown_to_int(const void *u, int32_t *out);
int32_t tz_unknown_to_long(const void *u, int64_t *out);
int32_t tz_unknown_to_double(const void *u, double *out);
int32_t tz_unknown_to_bool(const void *u, int32_t *out);
int32_t tz_unknown_to_string(const void *u, TzStr *out);
int32_t tz_unknown_to_box(const void *u, const TzType *type, void *out);
const TzLayout *tz_unknown_layout(const void *u);
uint32_t tz_unknown_box_kind(const void *u);
int32_t tz_unknown_holds(const void *u, const TzType *type);

#endif
