#ifndef TZ_UNKNOWN_H
#define TZ_UNKNOWN_H

#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"

typedef struct TzBox {
  int64_t rc;
  const TzLayout *layout;
  const TzType *type;
} TzBox;

#define TZ_BOX_VALUE(box) ((char *)(box) + sizeof(TzBox))

extern const TzLayout tz_box_layout;

enum {
  TZ_KIND_UNDEFINED = 0,
  TZ_KIND_NUMBER = 1,
  TZ_KIND_STRING = 2,
  TZ_KIND_BOOLEAN = 3,
  TZ_KIND_OBJECT = 4,
  TZ_KIND_FUNCTION = 5,
};

typedef struct TzClass {
  int64_t rc;
  const TzLayout *layout;
  const struct TzClass *parent;
  uint32_t id, last_id;
  const struct TzClassInfo *info;
} TzClass;

void tz_static_drop(void *object);
void *tz_unknown_of(const TzType *type, const void *value);
void *tz_unknown_box(const TzType *type, const void *value);
void *tz_unknown_box_member(const TzType *type, const void *value);
int32_t tz_unknown_kind(const void *u);
double tz_unknown_number(const void *u);
void tz_unknown_string(TzStr *out, const void *u);
int32_t tz_unknown_bool(const void *u);
int32_t tz_unknown_is_array(const void *u);
void *tz_unknown_array(const void *u, const TzType *array_type);
int32_t tz_unknown_equal(const void *a, const void *b);
void tz_unknown_unbox(const void *u, const TzType *type, void *out);

#endif
