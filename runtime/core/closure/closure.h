// Functions and the objects of closures.

#ifndef TZ_CLOSURE_H
#define TZ_CLOSURE_H

#include <stddef.h>
#include <stdint.h>

// The environments of closures and the boxes of the variables they share
// start with this header. The compiler emits a `drop` function for each kind
// of them, which releases what the object holds and frees it, and a constant
// descriptor that holds it. An object of the language (TzInstance) starts the
// same way, its layout with its drop function, so that the environment of a
// function value may be one: a method bound to its object.
typedef struct TzObjectDesc {
  void (*drop)(void *object);
} TzObjectDesc;

typedef struct TzObject {
  int64_t rc;
  const TzObjectDesc *desc;
} TzObject;

// A function value: what it is, and its environment (NULL if it captures
// nothing).
typedef struct {
  const struct TzFunctionInfo *info;
  TzObject *env;
} TzFunction;

typedef struct TzFunctionInfo {
  void *code;
  struct {
    void *obj;
    uint64_t meta;
  } name; // a TzStr, empty for an anonymous function
} TzFunctionInfo;

int64_t tz_obj_live_objects(void);
void *tz_obj_alloc(int64_t size);
void tz_obj_free(void *object);

#endif
