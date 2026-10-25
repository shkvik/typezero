// The checks behind JSON.parse<T> and fromJson<T> (docs/narrowing.md, section 3).

#include "runtime/library/json/json/json.h"
#include "runtime/library/json/json_values/json_values.h"
#include "runtime/library/json/json_stringify/json_stringify.h"
#include "runtime/core/array/array.h"
#include "runtime/core/error/error.h"
#include "runtime/core/output/output.h"
#include "runtime/core/string/string.h"
#include "runtime/library/decimal/decimal/decimal.h"
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The compiler makes a function for each type T that checks a Json and builds
// a T from it (Lowering::fromJsonFunction); these are its steps. `path` says
// where the value is, for the TypeError when it does not fit:
// `address.city: expected string, got number`.
typedef struct TzJsonPath {
  const struct TzJsonPath *parent;
  void *key_obj; // a key of an object, or with `index` >= 0 an element
  uint64_t key_meta;
  int64_t index;
} TzJsonPath;

static int is_identifier(TzStr key) {
  View v;
  tz_json_view_init(&v, key);
  if (v.length == 0) {
    return 0;
  }
  for (uint32_t i = 0; i < v.length; ++i) {
    uint16_t c = tz_json_view_at(&v, i);
    int letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
    if (!letter && !(i > 0 && c >= '0' && c <= '9')) {
      return 0;
    }
  }

  return 1;
}

static void path_text(TzBuf *b, const TzJsonPath *path) {
  if (!path) {
    return;
  }
  path_text(b, path->parent);
  if (path->index >= 0) {
    char text[32];
    int n = snprintf(text, sizeof text, "[%lld]", (long long)path->index);
    tz_buf_ascii_n(b, text, (size_t)n);
    return;
  }
  TzStr key = {path->key_obj, path->key_meta};
  if (is_identifier(key)) {
    if (b->length) {
      tz_buf_push(b, '.');
    }
    tz_buf_str(b, key);
  } else {
    tz_buf_push(b, '[');
    tz_json_quote(b, key);
    tz_buf_push(b, ']');
  }
}

// `got` describes what was there: its kind, or for a number the number. The
// caller returns next, with zero: the program checks tz_thrown after each
// step (Lowering::fromJsonFunction).
static void convert_error(const TzJsonPath *path, const char *type, int64_t type_length,
                          const TzJson *got, int show_number) {
  TzBuf b;
  tz_buf_init(&b);
  path_text(&b, path);
  if (b.length) {
    tz_buf_ascii(&b, ": ");
  }
  tz_buf_ascii(&b, "expected ");
  tz_buf_ascii_n(&b, type, (size_t)type_length);
  tz_buf_ascii(&b, ", got ");
  if (show_number && got && got->kind == TZ_JSON_NUMBER) {
    char text[32];
    tz_buf_ascii_n(&b, text, tz_format_f64(got->u.number.value, text));
  } else {
    tz_buf_ascii(&b, tz_json_kind_name(got));
  }
  tz_json_throw(TZ_TYPE_ERROR, &b);
}

void tz_json_expect_object(const TzJson *j, const TzJsonPath *path, const char *type,
                           int64_t type_length) {
  if (!j || j->kind != TZ_JSON_OBJECT) {
    convert_error(path, type, type_length, j, 0);
  }
}

// The value of a key of an object, borrowed; NULL when it is not there.
TzJson *tz_json_field(const TzJson *object, void *key_obj, uint64_t key_meta) {
  TzStr key = {key_obj, key_meta};
  int64_t at = tz_json_object_find(object, key);

  return at >= 0 ? object->u.object.entries[at].value : NULL;
}

// The entries of an object, for a Record<string, T>: how many, and the key
// and the value of each (borrowed), in the order of the text, which is the
// order the Record keeps.
uint32_t tz_json_object_count(const TzJson *object) { return object->u.object.count; }

TzJson *tz_json_object_entry(const TzJson *object, uint32_t k, TzStr *key) {
  const TzJsonEntry *e = &object->u.object.entries[k];
  *key = e->key;

  return e->value;
}

// The elements of an array, borrowed.
TzArray *tz_json_expect_array(const TzJson *j, const TzJsonPath *path, const char *type,
                              int64_t type_length) {
  if (!j || j->kind != TZ_JSON_ARRAY) {
    convert_error(path, type, type_length, j, 0);
    return NULL;
  }

  return j->u.array;
}

double tz_json_expect_double(const TzJson *j, const TzJsonPath *path) {
  if (!j || j->kind != TZ_JSON_NUMBER) {
    convert_error(path, "double", 6, j, 0);
    return 0;
  }

  return j->u.number.value;
}

// An int: a whole number in its range.
int32_t tz_json_expect_int(const TzJson *j, const TzJsonPath *path) {
  if (!j || j->kind != TZ_JSON_NUMBER) {
    convert_error(path, "int", 3, j, 0);
    return 0;
  }
  double d = j->u.number.value;
  if (!(d == floor(d) && d >= -2147483648.0 && d <= 2147483647.0)) {
    convert_error(path, "int", 3, j, 1);
    return 0;
  }

  return (int32_t)d;
}

// A long: a whole number in its range, exactly as written even beyond 2^53.
int64_t tz_json_expect_long(const TzJson *j, const TzJsonPath *path) {
  if (!j || j->kind != TZ_JSON_NUMBER) {
    convert_error(path, "long", 4, j, 0);
    return 0;
  }
  if (j->flags & F_TEXT) {
    View v;
    tz_json_view_init(&v, j->u.number.text);
    uint32_t i = tz_json_view_at(&v, 0) == '-';
    int negative = i == 1;
    uint64_t magnitude = 0;
    int ok = i < v.length;
    for (; i < v.length && ok; ++i) {
      uint16_t c = tz_json_view_at(&v, i);
      ok = c >= '0' && c <= '9' && magnitude <= (UINT64_C(9223372036854775808) - (c - '0')) / 10;
      magnitude = magnitude * 10 + (c - '0');
    }
    // Only plain digits are read here; "1e3" and the like go by the double.
    if (ok && magnitude <= UINT64_C(9223372036854775807) + (uint64_t)negative) {
      return negative ? (int64_t)(0 - magnitude) : (int64_t)magnitude;
    }
  }
  double d = j->u.number.value;
  if (!(d == floor(d) && d >= -9223372036854775808.0 && d < 9223372036854775808.0)) {
    convert_error(path, "long", 4, j, 1);
    return 0;
  }

  return (int64_t)d;
}

int32_t tz_json_expect_bool(const TzJson *j, const TzJsonPath *path) {
  if (!j || j->kind != TZ_JSON_BOOL) {
    convert_error(path, "boolean", 7, j, 0);
    return 0;
  }

  return j->u.boolean;
}

void tz_json_expect_string(TzStr *out, const TzJson *j, const TzJsonPath *path) {
  if (!j || j->kind != TZ_JSON_STRING) {
    convert_error(path, "string", 6, j, 0);
    *out = tz_str_from_ascii("", 0);
    return;
  }
  *out = j->u.string;
  tz_str_retain_value(*out);
}

// A decimal from the text of the number, exactly (docs/decimal.md); a number
// that kept no text has at most 15 digits, which the double gives back.
void tz_json_expect_decimal(TzDec *out, const TzJson *j, const TzJsonPath *path) {
  if (!j || j->kind != TZ_JSON_NUMBER) {
    convert_error(path, "decimal", 7, j, 0);
    memset(out, 0, sizeof *out);
    return;
  }
  TzStr text;
  if (j->flags & F_TEXT) {
    text = j->u.number.text;
    tz_str_retain_value(text);
  } else {
    tz_str_from_f64(&text, j->u.number.value == 0 ? 0 : j->u.number.value);
  }
  tz_dec_from_string(out, text.obj, text.meta);
  tz_str_release_value(text);
}

// A Json taken as it is: anything but undefined. Owned.
TzJson *tz_json_expect_value(const TzJson *j, const TzJsonPath *path) {
  if (!j) {
    convert_error(path, "Json", 4, j, 0);
    return NULL;
  }

  return tz_json_retained((TzJson *)j);
}

// Literal types and unions. Whether `j` is the value of a literal type: the
// string of `obj`/`meta`, a number, or a boolean.
int32_t tz_json_is_string(const TzJson *j, void *obj, uint64_t meta) {
  return j && j->kind == TZ_JSON_STRING &&
         tz_str_equal(j->u.string.obj, j->u.string.meta, obj, meta);
}

int32_t tz_json_is_number(const TzJson *j, double value) {
  return j && j->kind == TZ_JSON_NUMBER && j->u.number.value == value;
}

int32_t tz_json_is_bool(const TzJson *j, int32_t value) {
  return j && j->kind == TZ_JSON_BOOL && j->u.boolean == value;
}

// A value that no member of a union takes, or that a literal type does not
// have: what is there, with the value of a string or a number, as in
// `kind: expected "circle" | "square", got "triangle"`.
void tz_json_mismatch(const TzJson *j, const TzJsonPath *path, const char *type,
                      int64_t type_length) {
  if (!j || j->kind != TZ_JSON_STRING) {
    convert_error(path, type, type_length, j, 1);
    return;
  }
  TzBuf b;
  tz_buf_init(&b);
  path_text(&b, path);
  if (b.length) {
    tz_buf_ascii(&b, ": ");
  }
  tz_buf_ascii(&b, "expected ");
  tz_buf_ascii_n(&b, type, (size_t)type_length);
  tz_buf_ascii(&b, ", got ");
  tz_json_quote(&b, j->u.string);
  tz_json_throw(TZ_TYPE_ERROR, &b);
}
