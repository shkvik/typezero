#include "runtime/library/tz_modules/library/library.h"
#include "runtime/core/group/group.h"
#include "runtime/core/map/map.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

void *tz_lib_bare(void *map, TzMaker make) {
  (void)make;
  ((TzMap *)map)->flags |= TZ_MAP_BARE;
  tz_ref_retain(map);

  return map;
}

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
  for (uint32_t i = 0; i < layout->name_length; ++i) {
    tz_buf_push(&b, (uint8_t)layout->name[i]);
  }
  *out = tz_buf_take(&b);
}

int64_t tz_lib_pid(TzMaker make) {
  (void)make;
#ifdef _WIN32

  return (int32_t)GetCurrentProcessId();
#else

  return (int32_t)getpid();
#endif
}
