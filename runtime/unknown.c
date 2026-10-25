// Values of the type unknown (docs/decorators.md, section 4), and classes as
// values.
//
// An unknown is a reference, NULL for undefined. An object of the language
// (an instance, a literal, a date, a dictionary, an iterator, a class) is
// that reference itself; any other value lies in a box, an object of the
// language whose layout is tz_box_layout and which holds the value with the
// descriptor of its type. The compiler converts to unknown and back (see
// src/codegen/codegen_unknown.cpp) and calls these functions for what needs
// the type at run time: typeof, the values a check showed, equality.

#include "tzrt.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void box_drop(void *object) {
  TzBox *box = (TzBox *)object;
  // A box that is a member of a group (tz_unknown_box_member) lets go of its
  // value as a member does, and its group frees it.
  if (box->rc < 0) {
    tz_value_release_in(box, box->type, TZ_BOX_VALUE(box));
    tz_group_obj_free(box);
    return;
  }
  tz_value_release(box->type, TZ_BOX_VALUE(box));
  tz_obj_free(box);
}

const TzLayout tz_box_layout = {box_drop, NULL, 0, TZ_LAYOUT_BOX, NULL, 0, 0};

void tz_static_drop(void *object) { (void)object; }

static const TzBox *as_box(const void *u) {
  const TzInstance *o = (const TzInstance *)u;
  return o && o->layout == &tz_box_layout ? (const TzBox *)u : NULL;
}

void *tz_unknown_box(const TzType *type, const void *value) {
  TzBox *box = (TzBox *)tz_obj_alloc((int64_t)(sizeof(TzBox) + type->size));
  box->rc = 1;
  box->layout = &tz_box_layout;
  box->type = type;
  memcpy(TZ_BOX_VALUE(box), value, type->size);
  return box;
}

// The box of a value that may hold a member of a group, such as a function
// whose environment is one (a listener of node:events in its Record): a
// member itself, which joins the groups of what it holds (group.c). A box
// outside them would count its reference from outside the group that holds
// it, which could then never be freed.
void *tz_unknown_box_member(const TzType *type, const void *value) {
  TzBox *box = (TzBox *)tz_group_alloc((int64_t)(sizeof(TzBox) + type->size));
  box->layout = &tz_box_layout;
  box->type = type;
  memcpy(TZ_BOX_VALUE(box), value, type->size);
  tz_group_store_value(box, type, TZ_BOX_VALUE(box));
  return box;
}

void *tz_unknown_of(const TzType *type, const void *value) {
  // Of a union, what it holds; of a string literal, its string.
  type = tz_resolve(type, &value);
  switch (type->kind) {
  case TZ_T_OBJECT:
  case TZ_T_UNKNOWN:
    return *(void *const *)value;
  case TZ_T_UNDEFINED:
    return NULL;
  case TZ_T_OPTIONAL:
    // Undefined holds the zero value, which needs no release.
    return tz_optional_has(type, value) ? tz_unknown_of(type->inner, value) : NULL;
  default:
    return tz_unknown_box(type, value);
  }
}

static int32_t json_kind(const TzJson *j) {
  switch (j->kind) {
  case TZ_JSON_BOOL: return TZ_KIND_BOOLEAN;
  case TZ_JSON_NUMBER: return TZ_KIND_NUMBER;
  case TZ_JSON_STRING: return TZ_KIND_STRING;
  default: return TZ_KIND_OBJECT; // null, an array, an object
  }
}

extern TzErrorInfo tz_error_info; // error.c

// `throw u` of an unknown (owned): the Error it holds, or a TypeError in place
// of anything else, since only an Error can be thrown.
void *tz_unknown_error(void *u) {
  if (u && !as_box(u) && (((const TzInstance *)u)->layout->flags & TZ_LAYOUT_ERROR))
    return u;
  int32_t kind = tz_unknown_kind(u);
  static const char *const names[] = {"undefined", "a number", "a string", "a boolean",
                                      "an object", "a function"};
  char text[96];
  int n = snprintf(text, sizeof text, "only an Error can be thrown, and this is %s",
                   kind >= 0 && kind <= 5 ? names[kind] : "not one");
  if (u)
    tz_instance_release((TzInstance *)u);
  TzStr message = tz_str_from_ascii(text, (size_t)n);
  return tz_error_info.make(TZ_TYPE_ERROR, message.obj, message.meta);
}

int32_t tz_unknown_kind(const void *u) {
  if (!u)
    return TZ_KIND_UNDEFINED;
  const TzBox *box = as_box(u);
  if (!box) { // a class is a function in JavaScript
    const TzLayout *layout = ((const TzInstance *)u)->layout;
    // So is a listener that node:events wraps (see TZ_LAYOUT_LIBRARY).
    if ((layout->flags & TZ_LAYOUT_LIBRARY) &&
        ((layout->name_length == 11 && memcmp(layout->name, "OnceWrapper", 11) == 0) ||
         (layout->name_length == 13 && memcmp(layout->name, "AsyncListener", 13) == 0) ||
         (layout->name_length == 12 && memcmp(layout->name, "ThisListener", 12) == 0)))
      return TZ_KIND_FUNCTION;
    return layout->flags & TZ_LAYOUT_CLASS ? TZ_KIND_FUNCTION : TZ_KIND_OBJECT;
  }
  switch (box->type->kind) {
  case TZ_T_INT:
  case TZ_T_LONG:
  case TZ_T_DOUBLE:
    return TZ_KIND_NUMBER;
  case TZ_T_STRING:
    return TZ_KIND_STRING;
  case TZ_T_BOOL:
    return TZ_KIND_BOOLEAN;
  case TZ_T_FUNCTION:
    return TZ_KIND_FUNCTION;
  case TZ_T_JSON:
    return json_kind(*(const TzJson *const *)TZ_BOX_VALUE(box));
  default: // an array, a decimal (an object of BigNumber.js), a Result
    return TZ_KIND_OBJECT;
  }
}

double tz_unknown_number(const void *u) {
  const TzBox *box = as_box(u);
  const char *value = TZ_BOX_VALUE(box);
  switch (box->type->kind) {
  case TZ_T_INT: return *(const int32_t *)value;
  case TZ_T_LONG: return (double)*(const int64_t *)value;
  case TZ_T_DOUBLE: return *(const double *)value;
  default: return (*(const TzJson *const *)value)->u.number.value;
  }
}

void tz_unknown_string(TzStr *out, const void *u) {
  const TzBox *box = as_box(u);
  const char *value = TZ_BOX_VALUE(box);
  *out = box->type->kind == TZ_T_STRING ? *(const TzStr *)value
                                        : (*(const TzJson *const *)value)->u.string;
  tz_str_retain_value(*out);
}

int32_t tz_unknown_bool(const void *u) {
  const TzBox *box = as_box(u);
  const char *value = TZ_BOX_VALUE(box);
  if (box->type->kind == TZ_T_BOOL)
    return *(const uint8_t *)value != 0;
  return (*(const TzJson *const *)value)->u.boolean != 0;
}

int32_t tz_unknown_is_array(const void *u) {
  const TzBox *box = as_box(u);
  if (!box) // a tuple is an array in JavaScript
    return u && (((const TzInstance *)u)->layout->flags & TZ_LAYOUT_TUPLE) != 0;
  if (box->type->kind == TZ_T_ARRAY)
    return 1;
  return box->type->kind == TZ_T_JSON &&
         (*(const TzJson *const *)TZ_BOX_VALUE(box))->kind == TZ_JSON_ARRAY;
}

void *tz_unknown_array(const void *u, const TzType *array_type) {
  TzBox *box = (TzBox *)as_box(u);
  if (!box) {
    // A tuple: its elements, in a new array each time.
    const TzInstance *tuple = (const TzInstance *)u;
    const TzLayout *layout = tuple->layout;
    TzArray *result = tz_arr_new(array_type->inner, layout->count, layout->count);
    for (uint32_t i = 0; i < layout->count; ++i) {
      const TzField *field = &layout->fields[i];
      char *element = (char *)tuple + field->offset;
      tz_value_retain(field->type, element);
      ((void **)result->data)[i] = tz_unknown_of(field->type, element);
    }
    return result;
  }
  const TzArray *array = box->type->kind == TZ_T_ARRAY
                             ? *(TzArray *const *)TZ_BOX_VALUE(box)
                             : (*(const TzJson *const *)TZ_BOX_VALUE(box))->u.array;
  if (array->type->kind == TZ_T_UNKNOWN) {
    tz_ref_retain((void *)array);
    return (void *)array;
  }
  // Each element becomes an unknown of its own, in another array, which the
  // box holds from now on: every check of this unknown gives that one.
  TzArray *result = tz_arr_new(array_type->inner, array->length, array->length);
  for (uint32_t i = 0; i < array->length; ++i) {
    char *element = array->data + (size_t)i * array->type->size;
    tz_value_retain(array->type, element);
    ((void **)result->data)[i] = tz_unknown_of(array->type, element);
  }
  tz_value_release(box->type, TZ_BOX_VALUE(box));
  box->type = array_type;
  *(TzArray **)TZ_BOX_VALUE(box) = result;
  tz_ref_retain(result);
  return result;
}

// A box's value as a number, if it is one, for equality.
static int box_number(const TzBox *box, double *out) {
  const char *value = TZ_BOX_VALUE(box);
  switch (box->type->kind) {
  case TZ_T_INT: *out = *(const int32_t *)value; return 1;
  case TZ_T_LONG: *out = (double)*(const int64_t *)value; return 1;
  case TZ_T_DOUBLE: *out = *(const double *)value; return 1;
  case TZ_T_JSON: {
    const TzJson *j = *(const TzJson *const *)value;
    if (j->kind != TZ_JSON_NUMBER)
      return 0;
    *out = j->u.number.value;
    return 1;
  }
  default:
    return 0;
  }
}

static int box_string(const TzBox *box, TzStr *out) {
  const char *value = TZ_BOX_VALUE(box);
  if (box->type->kind == TZ_T_STRING) {
    *out = *(const TzStr *)value;
    return 1;
  }
  if (box->type->kind == TZ_T_JSON && (*(const TzJson *const *)value)->kind == TZ_JSON_STRING) {
    *out = (*(const TzJson *const *)value)->u.string;
    return 1;
  }
  return 0;
}

// `a === b`: the same object, or values of the same kind that are equal, as
// JavaScript compares what a box holds.
int32_t tz_unknown_equal(const void *a, const void *b) {
  if (a == b)
    return 1;
  const TzBox *x = as_box(a), *y = as_box(b);
  if (!x || !y)
    return 0;
  double m, n;
  if (box_number(x, &m))
    return box_number(y, &n) && m == n;
  TzStr s, t;
  if (box_string(x, &s))
    return box_string(y, &t) && tz_str_equal(s.obj, s.meta, t.obj, t.meta);
  const char *p = TZ_BOX_VALUE(x), *q = TZ_BOX_VALUE(y);
  uint32_t kind = x->type->kind;
  if (kind == TZ_T_JSON && y->type->kind == TZ_T_JSON)
    return tz_json_strict_equal(*(const TzJson *const *)p, *(const TzJson *const *)q);
  if (kind == TZ_T_JSON || y->type->kind == TZ_T_JSON) {
    // A boolean against a Json true or false.
    const TzJson *j = *(const TzJson *const *)(kind == TZ_T_JSON ? p : q);
    const TzBox *other = kind == TZ_T_JSON ? y : x;
    return j->kind == TZ_JSON_BOOL && other->type->kind == TZ_T_BOOL &&
           (j->u.boolean != 0) == (*(const uint8_t *)TZ_BOX_VALUE(other) != 0);
  }
  if (kind != y->type->kind)
    return 0;
  switch (kind) {
  case TZ_T_BOOL:
    return (*(const uint8_t *)p != 0) == (*(const uint8_t *)q != 0);
  case TZ_T_ARRAY:
    return *(void *const *)p == *(void *const *)q;
  case TZ_T_FUNCTION:
    return memcmp(p, q, sizeof(TzFunction)) == 0;
  case TZ_T_DECIMAL: {
    const TzDec *c = (const TzDec *)p, *d = (const TzDec *)q;
    return tz_dec_equal(c->coef, c->meta, d->coef, d->meta);
  }
  default: // two Results are two objects in JavaScript
    return 0;
  }
}

void tz_unknown_unbox(const void *u, const TzType *type, void *out) {
  switch (type->kind) {
  case TZ_T_OBJECT:
  case TZ_T_UNKNOWN:
    *(const void **)out = u;
    break;
  case TZ_T_OPTIONAL:
    if (type->inner->kind == TZ_T_OBJECT || type->inner->kind == TZ_T_JSON ||
        type->inner->kind == TZ_T_UNKNOWN) {
      if (!u) {
        *(void **)out = NULL;
        return;
      }
      tz_unknown_unbox(u, type->inner, out);
      return;
    }
    memset(out, 0, type->size);
    if (!u)
      return;
    tz_unknown_unbox(u, type->inner, out);
    ((uint8_t *)out)[type->inner->size] = 1;
    return;
  default:
    memcpy(out, TZ_BOX_VALUE(as_box(u)), type->size);
    break;
  }
  tz_value_retain(type, out);
}
