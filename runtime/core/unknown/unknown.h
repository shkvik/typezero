// unknown, and classes as values.

#ifndef TZ_UNKNOWN_H
#define TZ_UNKNOWN_H

#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"

// A value of the type unknown (docs/decorators.md, section 4) is a reference,
// NULL for undefined: an object of the language as it is, any other value in
// a box. The box is an object of the language too, with tz_box_layout, so
// that code that sees it as an `object` releases and prints it as one. The
// value follows the header, as a value of `type` lies in memory, and the box
// owns it. Must match Lowering (compiler/lower/codegen/codegen_unknown.cpp).
typedef struct TzBox {
  int64_t rc;
  const TzLayout *layout; // &tz_box_layout
  const TzType *type;     // of the value
} TzBox;

#define TZ_BOX_VALUE(box) ((char *)(box) + sizeof(TzBox))

extern const TzLayout tz_box_layout;

// What `typeof` says of an unknown, in the order of the strings Lowering
// gives them: "undefined", "number", "string", "boolean", "object",
// "function".
enum {
  TZ_KIND_UNDEFINED = 0,
  TZ_KIND_NUMBER = 1,
  TZ_KIND_STRING = 2,
  TZ_KIND_BOOLEAN = 3,
  TZ_KIND_OBJECT = 4,
  TZ_KIND_FUNCTION = 5,
};

// A class as a value: an object that lives as long as the program, which the
// compiler emits for each class code uses as one. Its layout has
// TZ_LAYOUT_CLASS and the class's name; `parent` is the class it extends.
typedef struct TzClass {
  int64_t rc;
  const TzLayout *layout;
  const struct TzClass *parent;
  uint32_t id, last_id; // its number, and the last of the classes down its chain
  // What reflection knows of it (reflect.c), NULL if reflection does not
  // reach it. The static fields follow, when it is used as a value.
  const struct TzClassInfo *info;
} TzClass;

// The drop function of objects that are never freed (classes).
void tz_static_drop(void *object);
// A value of `type` at `value` as an unknown, which takes it over.
void *tz_unknown_of(const TzType *type, const void *value);
// A new box of a value of `type` at `value`, which it takes over.
void *tz_unknown_box(const TzType *type, const void *value);
// The same as a member of a group of its own, for a value that may hold one.
void *tz_unknown_box_member(const TzType *type, const void *value);
int32_t tz_unknown_kind(const void *u);
// What a check showed the unknown `u` (borrowed) holds: a number (as a
// double), a string (owned), a boolean, an array.
double tz_unknown_number(const void *u);
void tz_unknown_string(TzStr *out, const void *u);
int32_t tz_unknown_bool(const void *u);
int32_t tz_unknown_is_array(const void *u);
// The array an unknown holds as an `unknown[]` (described by `array_type`):
// itself if its elements are unknowns, else a new array of them, which the
// unknown holds from then on.
void *tz_unknown_array(const void *u, const TzType *array_type);
// `a === b` of two unknowns.
int32_t tz_unknown_equal(const void *a, const void *b);
// The value of `type` in the unknown `u`, which holds one, copied to `out`
// and retained.
void tz_unknown_unbox(const void *u, const TzType *type, void *out);

#endif
