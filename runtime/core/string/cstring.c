#include "runtime/core/string/cstring.h"
#include "runtime/core/closure/closure.h"
#include <string.h>

static void cstring_drop(void *object) {
  tz_obj_free(object);
}

const TzLayout tz_cstring_layout = {cstring_drop, NULL, 0, TZ_LAYOUT_CSTRING, "cstring", 7, 0};

static size_t utf8_length(uint32_t code) {
  return code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
}

void tz_cstring_to_str(TzStr *out, const void *cstring) {
  const TzCString *c = (const TzCString *)cstring;
  TzBuf buf;
  tz_buf_init(&buf);
  tz_buf_utf8(&buf, c->text, (size_t)c->length);
  *out = tz_buf_take(&buf);
}

int32_t tz_cstring_equal(const void *a, const void *b) {
  const TzCString *x = (const TzCString *)a;
  const TzCString *y = (const TzCString *)b;
  if (x == y) {
    return 1;
  }
  if (!x || !y || x->length != y->length) {
    return 0;
  }
  return memcmp(x->text, y->text, (size_t)x->length) == 0;
}

