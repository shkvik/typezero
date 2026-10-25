// What the built-in modules tz has written in tz (src/lib) ask of the
// runtime through their intrinsics, `function __tz_lib_pid(): int` without
// a body (src/codegen/codegen_library.cpp): node:events, and the events of
// the web platform. util.inspect of an unknown, `__tz_lib_inspect`, is
// tz_lib_inspect in inspect.c. Each takes the maker of SystemError last,
// which these do not use.

#include "tzrt.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

// `__tz_lib_bare(r)`: the Record `map` is an object without a prototype,
// and prints as one, `[Object: null prototype] {}`. Gives it back, owned.
void *tz_lib_bare(void *map, TzMaker make) {
  (void)make;
  ((TzMap *)map)->flags |= TZ_MAP_BARE;
  tz_ref_retain(map); // it may be a member of a group (group.c)
  return map;
}

// `__tz_lib_class_name(o)`: the name of the class of the object `o`
// (borrowed), as node's `[EventEmitter]` shows it; "Object" for an object of
// no class.
void tz_lib_class_name(TzStr *out, const void *o, TzMaker make) {
  (void)make;
  const TzLayout *layout = ((const TzInstance *)o)->layout;
  if (layout->flags & TZ_LAYOUT_BOX) {
    *out = tz_str_from_ascii("Object", 6);
    return;
  }
  if (!layout->name_length) {
    *out = tz_str_from_ascii("Object", 6);
    return;
  }
  TzBuf b;
  tz_buf_init(&b);
  for (uint32_t i = 0; i < layout->name_length; ++i) // class names are identifiers
    tz_buf_push(&b, (uint8_t)layout->name[i]);
  *out = tz_buf_take(&b);
}

// `__tz_lib_pid()`: process.pid.
int32_t tz_lib_pid(TzMaker make) {
  (void)make;
#ifdef _WIN32
  return (int32_t)GetCurrentProcessId();
#else
  return (int32_t)getpid();
#endif
}
