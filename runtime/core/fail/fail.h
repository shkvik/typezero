#ifndef TZ_FAIL_H
#define TZ_FAIL_H

#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"

typedef struct {
  const char *code;
  TzStr message;
} TzFailParts;

void tz_fail(TzMaker make, const char *code, TzStr message);
void tz_fail_ascii(TzMaker make, const char *code, const char *message);
void tz_fail_utf8(TzMaker make, const char *code, const char *message);

#endif
