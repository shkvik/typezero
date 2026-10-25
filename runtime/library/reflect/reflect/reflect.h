// Reflection (docs/decorators.md, section 6).

#ifndef TZ_REFLECT_H
#define TZ_REFLECT_H

#include "runtime/core/array/array.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/core/unknown/unknown.h"
#include "runtime/library/json/json/json.h"

// Reflection (reflect.c, docs/decorators.md section 6): what the compiler
// puts in the exe about the classes reflection reaches.
//
// A method as a value of Reflect.get: a function of `type`, `(this: unknown,
// ...params) => R`, whose code checks the `this` it gets.
typedef struct TzReflectMethod {
  const char *name; // UTF-8
  uint32_t name_length;
  const TzType *type;
  const TzFunctionInfo *info;
} TzReflectMethod;

typedef struct TzClassInfo {
  // `new` with `count` arguments as unknowns (borrowed), checked against a
  // constructor: the object, or NULL after throwing. NULL for a class that
  // cannot be made (an abstract one).
  void *(*construct)(void *const *args, uint32_t count);
  // fromJson(cls, j): the object made through the constructor, or NULL
  // after throwing; NULL if the program does not ask.
  void *(*from_json)(const TzJson *json, const void *path);
  // Its methods and those up its chain it does not override.
  const TzReflectMethod *methods;
  uint32_t method_count;
} TzClassInfo;

// What calls a function value of `type` with arguments as unknowns: the
// arguments (borrowed) are checked against its parameters, `self` goes to a
// `this` parameter, and the result comes back as an unknown (owned), or
// NULL after throwing.
typedef void *(*TzInvoker)(const TzFunction *f, void *self, void *const *args, uint32_t count);
typedef struct TzInvokerEntry {
  const TzType *type;
  TzInvoker invoke;
} TzInvokerEntry;

// The objects of the classes by number, and the invokers of the function
// types that Reflect.apply may meet; main gives them first.
void tz_reflect_init(TzClass *const *classes, uint32_t count, const TzInvokerEntry *invokers,
                     uint32_t invoker_count);
// `new cls(...)` of a class known only when the program runs, with the
// arguments as unknowns (borrowed); Reflect.construct(cls, args) with an
// array of any values (borrowed).
void *tz_reflect_new(const TzClass *cls, void *const *args, uint32_t count);
void *tz_reflect_construct(const TzClass *cls, const TzArray *args);
// Reflect.construct(C, args) of a class known when compiling: its function
// `construct` (as TzClassInfo has it) makes the object from the array of
// any values (borrowed), without the description of the class.
void *tz_reflect_make(void *(*construct)(void *const *args, uint32_t count), const TzArray *args);
// Reflect.get(target, key): a field as an unknown, a method as a function,
// or undefined (NULL); a target that is no object is a TypeError.
void *tz_reflect_get(const void *target, void *key_obj, uint64_t key_meta);
// Reflect.apply(f, self, args), all borrowed.
void *tz_reflect_apply(const void *f, void *self, const TzArray *args);
// The name of a class, as `cls.name` gives it.
void tz_class_name(TzStr *out, const TzClass *cls);
// fromJson(cls, j) of a class known only when the program runs.
void *tz_reflect_from_json(const TzClass *cls, const TzJson *json);
// The TypeErrors of reflection: `what` (`new User`, `User.greet`) got the
// value `got` (borrowed) as its argument `index` (from 0), which is not a
// `expected`; or `count` arguments where it takes from `min` to `max` (-1:
// any number).
TZ_COLD void tz_reflect_mismatch(const char *what, uint32_t what_length, int32_t index,
                                 const char *expected, uint32_t expected_length, const void *got);
TZ_COLD void tz_reflect_arity(const char *what, uint32_t what_length, int32_t min, int32_t max,
                              uint32_t count);
// Whether the unknown `u` (borrowed) holds a value of a kind, and the value
// (owned) at `out`: a number of an int, of a long, any number, a boolean, a
// string, or a box of exactly `type` (any Json for a Json).
int32_t tz_unknown_to_int(const void *u, int32_t *out);
int32_t tz_unknown_to_long(const void *u, int64_t *out);
int32_t tz_unknown_to_double(const void *u, double *out);
int32_t tz_unknown_to_bool(const void *u, int32_t *out);
int32_t tz_unknown_to_string(const void *u, TzStr *out);
int32_t tz_unknown_to_box(const void *u, const TzType *type, void *out);
// The layout of the object of the language an unknown holds; NULL for
// undefined and a value in a box.
const TzLayout *tz_unknown_layout(const void *u);
// What tells the variants of a method apart when the program chooses one
// (docs/overloads.md, section 6): the kind (TZ_T_*) of the value an unknown
// holds in a box, 0 for undefined and an object of the language; whether it
// holds a box of exactly `type` (of a Json, any Json), as tz_unknown_to_box
// takes it.
uint32_t tz_unknown_box_kind(const void *u);
int32_t tz_unknown_holds(const void *u, const TzType *type);

#endif
