// Json values (docs/modules.md, section 5): their nodes and counts, the errors of
// JSON, and the keys and entries of objects.

#ifndef TZ_JSON_H
#define TZ_JSON_H

#include "runtime/core/array/array.h"
#include "runtime/core/group/group.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

// A value of the type Json (docs/modules.md, section 5) is a reference to a
// counted node; `Json | undefined` is that reference, NULL for undefined.
// null, true and false are nodes of the runtime that are never freed. A Json
// array keeps its elements in a TzArray of Json, which a program sees as
// `Json[]` once it checked Array.isArray. Must match Lowering
// (compiler/library/json/codegen/codegen_json.cpp).
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
  TzStr key; // flat
  TzJson *value;
} TzJsonEntry;

struct TzJson {
  int64_t rc;
  uint32_t kind;
  uint32_t flags; // see json.c
  union {
    int32_t boolean;
    // A number keeps the text it was read from when the double cannot give
    // its digits back (see json.c), for JSON.parse<T> into a decimal or long.
    struct {
      double value;
      TzStr text;
    } number;
    TzStr string;
    TzArray *array; // of Json
    struct {
      TzJsonEntry *entries; // in the order the keys were added
      uint32_t count, capacity;
      uint32_t *index; // positions + 1 by the hash of the key, or NULL
      uint32_t index_size;
    } object;
  } u;
};

// The descriptor of Json, the elements of the arrays of the runtime.
extern const TzType tz_json_type;
int64_t tz_json_live_objects(void);
// Releases what the runtime holds (the files embedJson read), before the
// leak check.
void tz_json_shutdown(void);
// Frees a node whose count reached zero, and everything that dies with it.
void tz_json_free(TzJson *j);
static inline void tz_json_release(TzJson *j) {
  if (j && tz_rc_dec(&j->rc)) {
    tz_json_free(j);
  }
}
// The positions of the entries of an object in the order JavaScript gives
// its keys: those that are array indices first, by their value, then the
// others as they were added. NULL when that is the order they are in;
// otherwise a block to free with tz_free.
uint32_t *tz_json_key_order(const TzJson *object);
// `a === b` of two Json values, NULL for undefined.
int32_t tz_json_strict_equal(const TzJson *a, const TzJson *b);

// Of a number: u.number.text holds the text it was read from.
#define F_TEXT 2u

// Not static: see the rules in tzrt.h.
extern TzJson tz_json_null_node;

extern TzJson tz_json_true_node;

extern TzJson tz_json_false_node;

TzJson *tz_json_node_new(uint32_t kind, size_t payload);

TzJson *tz_json_retained(TzJson *j);

#if TZ_PARALLEL
// publish.c: a Json other threads may reach, and everything in it, shared;
// as tz_json_free walks it, without recursion.
void tz_json_publish(TzJson *root);
#endif

// Throws the error of `kind` (TZ_TYPE_ERROR, TZ_SYNTAX_ERROR) with the
// message in `text`, which node would throw; the caller returns next. JSON.parse,
// JSON.parse<T>, fromJson<T>, JSON.stringify and reading a key of null or
// undefined throw these in JavaScript.
void tz_json_throw(int32_t kind, TzBuf *text);

// The code units of a string. Must stay where it is while `units` is used:
// an inline string keeps them in `flat` itself.
typedef struct {
  TzStr flat;
  const void *units;
  uint32_t length;
  int two;
} View;

void tz_json_view_init(View *v, TzStr s);

uint16_t tz_json_view_at(const View *v, uint32_t i);

int tz_json_key_equal(TzStr a, TzStr b);

// The value of a key that is an array index, "0" to "4294967294" without
// leading zeros; -1 for any other key.
int64_t tz_json_index_key(TzStr key);

int tz_json_is_length_key(TzStr key);

TzStr tz_json_index_text(uint32_t i);

// The position of `key` among the entries of object `o`, or -1.
int64_t tz_json_object_find(const TzJson *o, TzStr key);

// Sets `key` (borrowed) to `value` (owned). A key that is there keeps its
// place, as in JavaScript.
void tz_json_put(TzJson *o, TzStr key, TzJson *value);

void tz_json_object_remove(TzJson *o, TzStr key);

#endif
