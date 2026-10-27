#ifndef TZ_ERROR_H
#define TZ_ERROR_H

#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"

typedef struct TzFrame {
  const char *function;
  uint32_t function_length;
  uint32_t line;
  const char *file;
  uint32_t file_length;
  uint32_t column;
} TzFrame;

TZ_COLD void tz_set_failed(void *error);

typedef struct {
  TzStr message;
  int64_t errno_value;
  TzStr code, syscall;
  TzOptStr path, dest;
} TzSystemErrorParts;

#endif
