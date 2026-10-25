// Reflection (docs/decorators.md, section 6): Reflect.get, Reflect.apply,
// Reflect.construct and `new` of a class known only when the program runs.
//
// The fields and methods of an object of tz lie at places fixed when
// compiling, without names. For the classes that reflection reaches, the
// compiler puts a description in the exe (TzClassInfo, which the object of
// the class points to): a function that makes an object from arguments as
// unknowns, checking each against the constructor, and the methods as
// functions that take their `this`. The fields need nothing more: the layout
// of every object names them, for printing. A function value in an unknown
// is called through the invoker of its type, which checks the arguments
// against its parameters.

#include "tzrt.h"

#include <stdio.h>
#include <string.h>

int32_t tz_json_kind(const TzJson *j); // json.c

typedef struct {
  TzClass *const *classes; // by number
  uint32_t class_count;
  const TzInvokerEntry *invokers;
  uint32_t invoker_count;
} TzReflectStore;

TzReflectStore tz_reflect_store; // not static: see the rules in tzrt.h

void tz_reflect_init(TzClass *const *classes, uint32_t count, const TzInvokerEntry *invokers,
                     uint32_t invoker_count) {
  tz_reflect_store.classes = classes;
  tz_reflect_store.class_count = count;
  tz_reflect_store.invokers = invokers;
  tz_reflect_store.invoker_count = invoker_count;
}

static const TzBox *as_box(const void *u) {
  const TzInstance *o = (const TzInstance *)u;
  return o && o->layout == &tz_box_layout ? (const TzBox *)u : NULL;
}

// Whether the UTF-8 name is the string `key`.
static int same_name(const char *name, uint32_t length, TzStr key) {
  uint32_t units;
  int two;
  const void *data = tz_str_units(&key, &units, &two);
  uint32_t k = 0;
  for (uint32_t i = 0; i < length;) {
    uint8_t c = (uint8_t)name[i];
    uint32_t cp = c, n = 1;
    if (c >= 0xF0)
      cp = c & 0x07, n = 4;
    else if (c >= 0xE0)
      cp = c & 0x0F, n = 3;
    else if (c >= 0xC0)
      cp = c & 0x1F, n = 2;
    for (uint32_t j = 1; j < n && i + j < length; ++j)
      cp = (cp << 6) | ((uint8_t)name[i + j] & 0x3F);
    i += n;
    uint16_t want[2];
    uint32_t count = 1;
    if (cp >= 0x10000) {
      want[0] = (uint16_t)(0xD800 + ((cp - 0x10000) >> 10));
      want[1] = (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
      count = 2;
    } else {
      want[0] = (uint16_t)cp;
    }
    for (uint32_t j = 0; j < count; ++j, ++k) {
      if (k >= units)
        return 0;
      uint16_t unit = two ? ((const uint16_t *)data)[k] : ((const uint8_t *)data)[k];
      if (unit != want[j])
        return 0;
    }
  }
  return k == units;
}

// What an argument is, for messages: "undefined", "2.5", "a string", "an
// object of class User"...
static void describe(const void *u, char *out, size_t size) {
  int32_t kind = tz_unknown_kind(u);
  const TzBox *box = as_box(u);
  if (kind == TZ_KIND_NUMBER) {
    char number[32];
    size_t n = tz_format_f64(tz_unknown_number(u), number);
    snprintf(out, size, "%.*s", (int)n, number);
    return;
  }
  if (kind == TZ_KIND_BOOLEAN) {
    snprintf(out, size, "%s", tz_unknown_bool(u) ? "true" : "false");
    return;
  }
  if (u && !box) {
    const TzLayout *layout = ((const TzInstance *)u)->layout;
    if (layout->flags & TZ_LAYOUT_CLASS)
      snprintf(out, size, "the class %.*s", (int)layout->name_length, layout->name);
    else if (layout->name && layout->name_length)
      snprintf(out, size, "an object of class %.*s", (int)layout->name_length, layout->name);
    else
      snprintf(out, size, "an object");
    return;
  }
  if (box && box->type->kind == TZ_T_ARRAY) {
    snprintf(out, size, "an array");
    return;
  }
  static const char *const names[] = {"undefined", "a number",  "a string",
                                      "a boolean", "an object", "a function"};
  snprintf(out, size, "%s", kind >= 0 && kind <= 5 ? names[kind] : "a value");
}

void tz_reflect_mismatch(const char *what, uint32_t what_length, int32_t index,
                         const char *expected, uint32_t expected_length, const void *got) {
  char given[160];
  describe(got, given, sizeof given);
  tz_throw(TZ_TYPE_ERROR, "%.*s: argument %d: expected %.*s, got %s", (int)what_length, what,
           (int)index + 1, (int)expected_length, expected, given);
}

void tz_reflect_arity(const char *what, uint32_t what_length, int32_t min, int32_t max,
                      uint32_t count) {
  char takes[64];
  if (max < 0)
    snprintf(takes, sizeof takes, "%d or more arguments", (int)min);
  else if (min == max)
    snprintf(takes, sizeof takes, "%d argument%s", (int)min, min == 1 ? "" : "s");
  else
    snprintf(takes, sizeof takes, "%d to %d arguments", (int)min, (int)max);
  tz_throw(TZ_TYPE_ERROR, "%.*s: expected %s, got %u", (int)what_length, what, takes, count);
}

void tz_class_name(TzStr *out, const TzClass *cls) {
  const TzLayout *layout = cls->layout;
  TzBuf buf;
  tz_buf_init(&buf);
  const char *name = layout->name;
  for (uint32_t i = 0; i < layout->name_length;) {
    uint8_t c = (uint8_t)name[i];
    uint32_t cp = c, n = 1;
    if (c >= 0xF0)
      cp = c & 0x07, n = 4;
    else if (c >= 0xE0)
      cp = c & 0x0F, n = 3;
    else if (c >= 0xC0)
      cp = c & 0x1F, n = 2;
    for (uint32_t j = 1; j < n && i + j < layout->name_length; ++j)
      cp = (cp << 6) | ((uint8_t)name[i + j] & 0x3F);
    i += n;
    if (cp >= 0x10000) {
      tz_buf_push(&buf, (uint16_t)(0xD800 + ((cp - 0x10000) >> 10)));
      tz_buf_push(&buf, (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF)));
    } else {
      tz_buf_push(&buf, (uint16_t)cp);
    }
  }
  *out = tz_buf_take(&buf);
}

//===----------------------------------------------------------------------===//
// new and Reflect.construct
//===----------------------------------------------------------------------===//

// Whether `u` is a class, which `new` takes; throws a TypeError if not.
static const TzClass *as_class(const void *u) {
  if (u && !as_box(u) && (((const TzInstance *)u)->layout->flags & TZ_LAYOUT_CLASS))
    return (const TzClass *)u;
  char given[160];
  describe(u, given, sizeof given);
  tz_throw(TZ_TYPE_ERROR, "%s is not a constructor", given);
  return NULL;
}

void *tz_reflect_new(const TzClass *cls, void *const *args, uint32_t count) {
  const TzLayout *layout = cls->layout;
  if (!cls->info) { // String, Number... as `design:*` records give them
    tz_throw(TZ_TYPE_ERROR, "%.*s of JavaScript cannot be made through reflection",
             (int)layout->name_length, layout->name);
    return NULL;
  }
  if (!cls->info->construct) {
    tz_throw(TZ_TYPE_ERROR, "class %.*s is abstract and cannot be made", (int)layout->name_length,
             layout->name);
    return NULL;
  }
  return cls->info->construct(args, count);
}

// The elements of an array of any type as unknowns, which `out` (of the
// array's length) owns.
static void as_unknowns(const TzArray *array, void **out) {
  for (uint32_t i = 0; i < array->length; ++i) {
    char *element = array->data + (size_t)i * array->type->size;
    tz_value_retain(array->type, element);
    out[i] = tz_unknown_of(array->type, element);
  }
}

static void release_unknowns(void **values, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i)
    tz_instance_release((TzInstance *)values[i]);
}

void *tz_reflect_construct(const TzClass *cls, const TzArray *args) {
  if (!as_class(cls))
    return NULL;
  void *small[8];
  void **values = args->length <= 8 ? small : (void **)tz_alloc(args->length * sizeof(void *));
  as_unknowns(args, values);
  void *result = tz_reflect_new(cls, values, args->length);
  release_unknowns(values, args->length);
  if (values != small)
    tz_free(values);
  return result;
}

//===----------------------------------------------------------------------===//
// Reflect.get
//===----------------------------------------------------------------------===//

// The description of the class of an object, if reflection reaches it.
static const TzClassInfo *info_of(const TzInstance *object) {
  if (object->layout->flags & TZ_LAYOUT_CLASS)
    return NULL; // the static methods are not described
  uint32_t id = object->layout->class_id;
  if (!id || id >= tz_reflect_store.class_count || !tz_reflect_store.classes[id])
    return NULL;
  return tz_reflect_store.classes[id]->info;
}

void *tz_reflect_get(const void *target, void *key_obj, uint64_t key_meta) {
  TzStr key = {key_obj, key_meta};
  if (!target || as_box(target)) {
    tz_throw(TZ_TYPE_ERROR, "Reflect.get called on non-object");
    return NULL;
  }
  const TzInstance *object = (const TzInstance *)target;
  // A field, by the names the layout gives for printing; of a class, its
  // static fields, and its name.
  const TzLayout *layout = object->layout;
  for (uint32_t i = 0; i < layout->count; ++i) {
    const TzField *field = &layout->fields[i];
    if (field->offset == TZ_FIELD_METHOD || !same_name(field->name, field->name_length, key))
      continue;
    char *value = (char *)object + field->offset;
    tz_value_retain(field->type, value);
    return tz_unknown_of(field->type, value);
  }
  if ((layout->flags & TZ_LAYOUT_CLASS) && same_name("name", 4, key)) {
    TzStr name;
    tz_class_name(&name, (const TzClass *)object);
    return tz_unknown_box(&tz_string_type, &name);
  }
  // A method, as a function that takes its `this`.
  const TzClassInfo *info = info_of(object);
  if (info)
    for (uint32_t i = 0; i < info->method_count; ++i) {
      const TzReflectMethod *method = &info->methods[i];
      if (!same_name(method->name, method->name_length, key))
        continue;
      TzFunction fn = {method->info, NULL};
      return tz_unknown_box(method->type, &fn);
    }
  return NULL;
}

//===----------------------------------------------------------------------===//
// Reflect.apply
//===----------------------------------------------------------------------===//

void *tz_reflect_apply(const void *f, void *self, const TzArray *args) {
  const TzBox *box = as_box(f);
  if (!box || box->type->kind != TZ_T_FUNCTION) {
    char given[160];
    describe(f, given, sizeof given);
    tz_throw(TZ_TYPE_ERROR, "Function.prototype.apply was called on %s, which is not a function",
             given);
    return NULL;
  }
  TzInvoker invoke = NULL;
  for (uint32_t i = 0; i < tz_reflect_store.invoker_count && !invoke; ++i)
    if (tz_reflect_store.invokers[i].type == box->type)
      invoke = tz_reflect_store.invokers[i].invoke;
  if (!invoke) { // Sema gives one to each type of function that can be here
    tz_throw(TZ_TYPE_ERROR, "Reflect.apply cannot call this function");
    return NULL;
  }
  void *small[8];
  void **values = args->length <= 8 ? small : (void **)tz_alloc(args->length * sizeof(void *));
  as_unknowns(args, values);
  void *result = invoke((const TzFunction *)TZ_BOX_VALUE(box), self, values, args->length);
  release_unknowns(values, args->length);
  if (values != small)
    tz_free(values);
  return result;
}

//===----------------------------------------------------------------------===//
// fromJson(cls, j)
//===----------------------------------------------------------------------===//

void *tz_reflect_from_json(const TzClass *cls, const TzJson *json) {
  const TzLayout *layout = cls->layout;
  if (!cls->info || !cls->info->from_json) {
    tz_throw(TZ_TYPE_ERROR, "class %.*s cannot be made from JSON", (int)layout->name_length,
             layout->name);
    return NULL;
  }
  return cls->info->from_json(json, NULL);
}

//===----------------------------------------------------------------------===//
// Checked conversions of unknowns
//===----------------------------------------------------------------------===//

// What the code that reflection runs needs to take an argument as unknown
// (borrowed) for a parameter: whether it holds a value of the kind, and
// that value (owned) in `out`.

static int box_number(const void *u, double *out) {
  const TzBox *box = as_box(u);
  if (!box)
    return 0;
  const char *value = TZ_BOX_VALUE(box);
  switch (box->type->kind) {
  case TZ_T_INT: *out = *(const int32_t *)value; return 1;
  case TZ_T_LONG: *out = (double)*(const int64_t *)value; return 1;
  case TZ_T_DOUBLE: *out = *(const double *)value; return 1;
  case TZ_T_JSON:
    if (tz_json_kind(*(const TzJson *const *)value) != TZ_JSON_NUMBER)
      return 0;
    *out = tz_unknown_number(u);
    return 1;
  default: return 0;
  }
}

int32_t tz_unknown_to_double(const void *u, double *out) { return box_number(u, out); }

int32_t tz_unknown_to_int(const void *u, int32_t *out) {
  double n;
  if (!box_number(u, &n) || n != (double)(int32_t)n || (n == 0 && 1 / n < 0))
    return 0;
  *out = (int32_t)n;
  return 1;
}

int32_t tz_unknown_to_long(const void *u, int64_t *out) {
  const TzBox *box = as_box(u);
  if (box && box->type->kind == TZ_T_LONG) { // exactly, beyond 2^53 too
    *out = *(const int64_t *)TZ_BOX_VALUE(box);
    return 1;
  }
  double n;
  if (!box_number(u, &n) || n < -9223372036854775808.0 || n >= 9223372036854775808.0 ||
      n != (double)(int64_t)n)
    return 0;
  *out = (int64_t)n;
  return 1;
}

int32_t tz_unknown_to_bool(const void *u, int32_t *out) {
  if (tz_unknown_kind(u) != TZ_KIND_BOOLEAN)
    return 0;
  *out = tz_unknown_bool(u);
  return 1;
}

int32_t tz_unknown_to_string(const void *u, TzStr *out) {
  if (tz_unknown_kind(u) != TZ_KIND_STRING)
    return 0;
  tz_unknown_string(out, u);
  return 1;
}

// A box of exactly `type`: an array, a function, a decimal, a Result; of a
// Json, any Json.
int32_t tz_unknown_to_box(const void *u, const TzType *type, void *out) {
  const TzBox *box = as_box(u);
  if (!box || (box->type != type && !(type->kind == TZ_T_JSON && box->type->kind == TZ_T_JSON)))
    return 0;
  memcpy(out, TZ_BOX_VALUE(box), type->size);
  tz_value_retain(type, out);
  return 1;
}

// The layout of an object of the language in an unknown; NULL for undefined
// and a value in a box.
const TzLayout *tz_unknown_layout(const void *u) {
  return u && !as_box(u) ? ((const TzInstance *)u)->layout : NULL;
}
