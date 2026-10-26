#ifndef TZ_ERROR_H
#define TZ_ERROR_H

#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"

enum { TZ_ERROR = 0, TZ_TYPE_ERROR = 1, TZ_RANGE_ERROR = 2, TZ_SYNTAX_ERROR = 3, TZ_URI_ERROR = 4 };

typedef struct TzFrame {
  const char *function;
  uint32_t function_length;
  uint32_t line;
  const char *file;
  uint32_t file_length;
  uint32_t column;
} TzFrame;

typedef struct TzErrorInfo {
  void *(*make)(int32_t kind, void *message_obj, uint64_t message_meta);
  const TzType *frames_type;
  uint32_t name_offset, message_offset, cause_offset, frames_offset;
} TzErrorInfo;

void *tz_error_take(void);

void tz_error_init(const TzErrorInfo *info);
TZ_COLD void tz_throw(int32_t kind, const char *format, ...);
TZ_COLD void tz_throw_str(int32_t kind, void *obj, uint64_t meta);
TZ_COLD void tz_set_failed(void *error);
void tz_buf_error_text(TzBuf *out, const void *error);
int tz_buf_error_stack(TzBuf *out, const void *error);
const void *tz_error_cause(const void *error);
int tz_error_base_field(const TzField *field);
int tz_error_named(const void *error);

typedef struct {
  TzStr message, name;
  int32_t errno_value;
  TzStr code, syscall;
  TzOptStr path, dest;
} TzSystemErrorParts;

#endif
