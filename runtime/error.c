// Errors (docs/errors.md): the error in flight, the places an error passes on
// its way up, the text of its stack, and an error nobody catches.
//
// An exception is a value that travels back up the calls: `throw` puts the
// Error in tz_thrown and returns from the function, and every caller of a
// function that may throw checks tz_thrown after the call, releasing what it
// holds and returning in turn, until a `catch` takes the error. The class
// Error is the program's (CodeGen declares it); the runtime makes Errors for
// its own failures through the function the program gives it.
//
// The stack of an error is collected on the way: the function that throws
// and each one the error passes add their place, up to Node's default of ten
// (Error.stackTraceLimit). Places cost nothing until something throws.

#include "tzrt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Not static: see the rules in tzrt.h.
void *tz_thrown;
char *tz_stack_limit;
TzErrorInfo tz_error_info;

enum { kStackTraceLimit = 10 };

void tz_error_init(const TzErrorInfo *info) { tz_error_info = *info; }

static TzStr *field_str(const void *error, uint32_t offset) {
  return (TzStr *)((char *)error + offset);
}

static TzArray **frames_of(const void *error) {
  return (TzArray **)((char *)error + tz_error_info.frames_offset);
}

const void *tz_error_cause(const void *error) {
  return *(void *const *)((const char *)error + tz_error_info.cause_offset);
}

// Appends UTF-8 text as UTF-16; a malformed sequence becomes U+FFFD.
static void buf_utf8(TzBuf *out, const char *text, size_t n) {
  const unsigned char *s = (const unsigned char *)text;
  for (size_t i = 0; i < n;) {
    uint32_t c = s[i];
    size_t len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
    if (len == 0 || i + len > n) {
      tz_buf_push(out, 0xFFFD);
      ++i;
      continue;
    }
    if (len > 1) {
      c &= 0x3Fu >> (len - 1);
      for (size_t k = 1; k < len; ++k)
        c = (c << 6) | (s[i + k] & 0x3Fu);
    }
    i += len;
    if (c >= 0x10000) {
      c -= 0x10000;
      tz_buf_push(out, (uint16_t)(0xD800 + (c >> 10)));
      tz_buf_push(out, (uint16_t)(0xDC00 + (c & 0x3FF)));
    } else {
      tz_buf_push(out, (uint16_t)c);
    }
  }
}

static void buf_u32(TzBuf *out, uint32_t value) {
  char text[16];
  int n = snprintf(text, sizeof text, "%u", value);
  tz_buf_ascii_n(out, text, (size_t)n);
}

//===----------------------------------------------------------------------===//
// Throwing
//===----------------------------------------------------------------------===//

void tz_error_frame(const TzFrame *frame) {
  if (!tz_thrown)
    return;
  TzArray **frames = frames_of(tz_thrown);
  if (!*frames)
    *frames = tz_arr_new(tz_error_info.frames_type, 0, kStackTraceLimit);
  if ((*frames)->length >= kStackTraceLimit)
    return;
  int64_t address = (int64_t)(intptr_t)frame;
  tz_arr_push_value(*frames, &address);
}

// `throw error`. An error thrown again (`catch (e) { throw e }`) keeps the
// places it has: the stack says where it came from.
void tz_error_throw(void *error, const TzFrame *frame) {
  tz_thrown = error;
  TzArray *frames = *frames_of(error);
  if (!frames || frames->length == 0)
    tz_error_frame(frame);
}

// reject(error) and Promise.reject(error): an error that was never thrown
// gets the place where it was rejected, as `throw` gives it its place.
void tz_error_rejected(void *error, const TzFrame *frame) {
  TzArray *frames = *frames_of(error);
  if (frames && frames->length)
    return;
  void *thrown = tz_thrown;
  tz_thrown = error;
  tz_error_frame(frame);
  tz_thrown = thrown;
}

// An Error of `kind` with the message, thrown.
void tz_throw_str(int32_t kind, void *obj, uint64_t meta) {
  tz_str_retain_value((TzStr){obj, meta});
  void *error = tz_error_info.make(kind, obj, meta);
  if (tz_thrown) // never, as the program runs only without one
    tz_instance_release((TzInstance *)tz_thrown);
  tz_thrown = error;
}

void tz_throw(int32_t kind, const char *format, ...) {
  char text[512];
  va_list args;
  va_start(args, format);
  int len = vsnprintf(text, sizeof text, format, args);
  va_end(args);
  if (len < 0)
    len = 0;
  if ((size_t)len >= sizeof text)
    len = (int)sizeof text - 1;
  TzBuf buf;
  tz_buf_init(&buf);
  buf_utf8(&buf, text, (size_t)len);
  TzStr message = tz_buf_take(&buf);
  tz_throw_str(kind, message.obj, message.meta);
  tz_str_release_value(message);
}

// A function that may recurse found the stack too low at its start.
TZ_COLD void tz_throw_stack_overflow(void) {
  tz_throw(TZ_RANGE_ERROR, "Maximum call stack size exceeded");
}

//===----------------------------------------------------------------------===//
// Text
//===----------------------------------------------------------------------===//

// Error.prototype.toString: the name and the message, or the one that is not
// empty.
void tz_buf_error_text(TzBuf *out, const void *error) {
  TzStr name = *field_str(error, tz_error_info.name_offset);
  TzStr message = *field_str(error, tz_error_info.message_offset);
  size_t start = out->length;
  tz_buf_str(out, name);
  size_t nameLength = out->length - start;
  TzBuf text;
  tz_buf_init(&text);
  tz_buf_str(&text, message);
  if (nameLength != 0 && text.length != 0)
    tz_buf_ascii(out, ": ");
  tz_buf_units(out, text.data, text.length);
  tz_buf_free(&text);
}

// The text, then "    at f (file:line:column)" for each place, as V8 makes
// them; top-level code has no function name. An `await` that an error comes
// out of is "async f", or in top-level code "async" before the place.
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
      buf_utf8(out, frame->function, frame->function_length);
      tz_buf_ascii(out, " (");
    }
    buf_utf8(out, frame->file, frame->file_length);
    tz_buf_push(out, ':');
    buf_u32(out, frame->line);
    tz_buf_push(out, ':');
    buf_u32(out, frame->column);
    if (named)
      tz_buf_push(out, ')');
  }
  return count != 0;
}

// Whether a field of the layout of an Error is one of the class Error's
// (message, name, cause): JavaScript keeps them apart from the keys of the
// object, which a class derived from Error adds.
int tz_error_base_field(const TzField *field) {
  return field->offset == tz_error_info.message_offset ||
         field->offset == tz_error_info.name_offset || field->offset == tz_error_info.cause_offset;
}

// Whether the program gave the error a name of its own (`this.name =
// "HttpError"`), which is a key of the object in JavaScript: a name that is
// not the one of a built-in class.
int tz_error_named(const void *error) {
  static const char *const builtins[] = {"Error", "TypeError", "RangeError", "SyntaxError",
                                         "SystemError"};
  TzBuf name;
  tz_buf_init(&name);
  tz_buf_str(&name, *field_str(error, tz_error_info.name_offset));
  int named = 1;
  for (size_t k = 0; k < sizeof builtins / sizeof *builtins && named; ++k) {
    size_t n = strlen(builtins[k]);
    if (name.length != n)
      continue;
    size_t i = 0;
    while (i < n && name.data[i] == (uint16_t)builtins[k][i])
      ++i;
    named = i != n;
  }
  tz_buf_free(&name);
  return named;
}

// `e.stack`
void tz_error_stack(TzStr *out, void *error) {
  TzBuf buf;
  tz_buf_init(&buf);
  tz_buf_error_stack(&buf, error);
  *out = tz_buf_take(&buf);
}

// `e.toString()`
void tz_error_to_string(TzStr *out, void *error) {
  TzBuf buf;
  tz_buf_init(&buf);
  tz_buf_error_text(&buf, error);
  *out = tz_buf_take(&buf);
}

//===----------------------------------------------------------------------===//
// An error nobody catches
//===----------------------------------------------------------------------===//

void tz_buf_inspect_error(TzBuf *out, const void *error);
void tz_err_begin(void);
void tz_err_end(void);

// As in Node: what is printed so far is written out, the error goes to
// stderr as console.error would show it, and the program ends with 1.
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
