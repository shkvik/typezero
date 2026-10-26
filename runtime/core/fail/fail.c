#include "runtime/core/fail/fail.h"
#include "runtime/core/parallel/parallel.h"
#include <string.h>

void tz_fail(TzMaker make, const char *code, TzStr message) {
  TzFailParts parts;
  parts.code = code;
  parts.message = message;
  tz_failed = make(&parts);
}

void tz_fail_ascii(TzMaker make, const char *code, const char *message) {
  tz_fail(make, code, tz_str_from_ascii(message, strlen(message)));
}

void tz_fail_utf8(TzMaker make, const char *code, const char *message) {
  TzBuf text;
  tz_buf_init(&text);
  tz_buf_utf8(&text, message, strlen(message));
  tz_fail(make, code, tz_buf_take(&text));
}
