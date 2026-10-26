#include "runtime/core/error/error.h"
#include "runtime/core/array/array.h"
#include "runtime/core/group/group.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TzErrorInfo tz_error_info;

enum { kStackTraceLimit = 10 };

void tz_error_init(const TzErrorInfo *info) { tz_error_info = *info; }

void *tz_error_take(void) {
  void *error = tz_thrown;
  tz_thrown = NULL;

  return error;
}

static TzStr *field_str(const void *error, uint32_t offset) {
  return (TzStr *)((char *)error + offset);
}

static TzArray **frames_of(const void *error) {
  return (TzArray **)((char *)error + tz_error_info.frames_offset);
}

const void *tz_error_cause(const void *error) {
  return *(void *const *)((const char *)error + tz_error_info.cause_offset);
}

static void buf_u32(TzBuf *out, uint32_t value) {
  char text[16];
  int n = snprintf(text, sizeof text, "%u", value);
  tz_buf_ascii_n(out, text, (size_t)n);
}

void tz_error_frame(const TzFrame *frame) {
  if (!tz_thrown) {
    return;
  }
  TzArray **frames = frames_of(tz_thrown);
  if (!*frames) {
    *frames = tz_arr_new(tz_error_info.frames_type, 0, kStackTraceLimit);
  }
  if ((*frames)->length >= kStackTraceLimit) {
    return;
  }
  int64_t address = (int64_t)(intptr_t)frame;
  tz_arr_push_value(*frames, &address);
}

void tz_error_throw(void *error, const TzFrame *frame) {
  tz_thrown = error;
  TzArray *frames = *frames_of(error);
  if (!frames || frames->length == 0) {
    tz_error_frame(frame);
  }
}

void tz_error_rejected(void *error, const TzFrame *frame) {
  TzArray *frames = *frames_of(error);
  if (frames && frames->length) {
    return;
  }
  void *thrown = tz_thrown;
  tz_thrown = error;
  tz_error_frame(frame);
  tz_thrown = thrown;
}

void tz_throw_str(int32_t kind, void *obj, uint64_t meta) {
  tz_str_retain_value((TzStr){obj, meta});
  void *error = tz_error_info.make(kind, obj, meta);
  if (tz_thrown) {
    tz_instance_release((TzInstance *)tz_thrown);
  }
  tz_thrown = error;
}

void tz_throw(int32_t kind, const char *format, ...) {
  char text[512];
  va_list args;
  va_start(args, format);
  int len = vsnprintf(text, sizeof text, format, args);
  va_end(args);
  if (len < 0) {
    len = 0;
  }
  if ((size_t)len >= sizeof text) {
    len = (int)sizeof text - 1;
  }
  TzBuf buf;
  tz_buf_init(&buf);
  tz_buf_utf8(&buf, text, (size_t)len);
  TzStr message = tz_buf_take(&buf);
  tz_throw_str(kind, message.obj, message.meta);
  tz_str_release_value(message);
}

TZ_COLD void tz_throw_stack_overflow(void) {
  tz_throw(TZ_RANGE_ERROR, "Maximum call stack size exceeded");
}

void tz_buf_error_text(TzBuf *out, const void *error) {
  TzStr name = *field_str(error, tz_error_info.name_offset);
  TzStr message = *field_str(error, tz_error_info.message_offset);
  size_t start = out->length;
  tz_buf_str(out, name);
  size_t nameLength = out->length - start;
  TzBuf text;
  tz_buf_init(&text);
  tz_buf_str(&text, message);
  if (nameLength != 0 && text.length != 0) {
    tz_buf_ascii(out, ": ");
  }
  tz_buf_units(out, text.data, text.length);
  tz_buf_free(&text);
}

int tz_buf_error_stack(TzBuf *out, const void *error) {
  tz_buf_error_text(out, error);
  const TzArray *frames = *frames_of(error);
  uint32_t count = frames ? frames->length : 0;
  for (uint32_t i = 0; i < count; ++i) {
    const TzFrame *frame = (const TzFrame *)(intptr_t)((const int64_t *)frames->data)[i];
    tz_buf_ascii(out, "\n    at ");
    int named = frame->function_length != 0;
    if (frame->function_length == 5 && memcmp(frame->function, "async", 5) == 0) {
      tz_buf_ascii(out, "async ");
      named = 0;
    } else if (named) {
      tz_buf_utf8(out, frame->function, frame->function_length);
      tz_buf_ascii(out, " (");
    }
    tz_buf_utf8(out, frame->file, frame->file_length);
    tz_buf_push(out, ':');
    buf_u32(out, frame->line);
    tz_buf_push(out, ':');
    buf_u32(out, frame->column);
    if (named) {
      tz_buf_push(out, ')');
    }
  }

  return count != 0;
}

int tz_error_base_field(const TzField *field) {
  return field->offset == tz_error_info.message_offset ||
         field->offset == tz_error_info.name_offset || field->offset == tz_error_info.cause_offset;
}

int tz_error_named(const void *error) {
  static const char *const builtins[] = {"Error",       "TypeError", "RangeError",
                                         "SyntaxError", "URIError",  "SystemError"};
  TzBuf name;
  tz_buf_init(&name);
  tz_buf_str(&name, *field_str(error, tz_error_info.name_offset));
  int named = 1;
  for (size_t k = 0; k < sizeof builtins / sizeof *builtins && named; ++k) {
    size_t n = strlen(builtins[k]);
    if (name.length != n) {
      continue;
    }
    size_t i = 0;
    while (i < n && name.data[i] == (uint16_t)builtins[k][i]) {
      ++i;
    }
    named = i != n;
  }
  tz_buf_free(&name);

  return named;
}

void tz_error_stack(TzStr *out, void *error) {
  TzBuf buf;
  tz_buf_init(&buf);
  tz_buf_error_stack(&buf, error);
  *out = tz_buf_take(&buf);
}

void tz_error_to_string(TzStr *out, void *error) {
  TzBuf buf;
  tz_buf_init(&buf);
  tz_buf_error_text(&buf, error);
  *out = tz_buf_take(&buf);
}

void tz_buf_inspect_error(TzBuf *out, const void *error);
void tz_err_begin(void);
void tz_err_end(void);

TZ_NORETURN void tz_uncaught(void) {
  TzBuf buf;
  tz_buf_init(&buf);
  tz_buf_inspect_error(&buf, tz_thrown);
  tz_buf_push(&buf, '\n');
  tz_err_begin();
  tz_out_write_utf16(buf.data, buf.length);
  tz_err_end();
  tz_buf_free(&buf);
  exit(1);
}
