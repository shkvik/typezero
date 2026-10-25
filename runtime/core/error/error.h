// Errors: the error in flight, and what the runtime knows of the class Error.

#ifndef TZ_ERROR_H
#define TZ_ERROR_H

#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"

// The kinds of errors the runtime throws, numbered as kErrorKinds in
// compiler/program/ast/ast.h.
enum { TZ_ERROR = 0, TZ_TYPE_ERROR = 1, TZ_RANGE_ERROR = 2, TZ_SYNTAX_ERROR = 3, TZ_URI_ERROR = 4 };

// A place an error passed on its way up: a function, and where in which file.
// The compiler emits one constant for each call and throw that may throw
// (Lowering::frameDescriptor).
typedef struct TzFrame {
  const char *function; // UTF-8; empty for the top-level code of a file
  uint32_t function_length;
  uint32_t line;
  const char *file; // UTF-8, the full path
  uint32_t file_length;
  uint32_t column;
} TzFrame;

// What the runtime knows of the class Error, which the program declares
// (Lowering::emitErrorSupport passes it to tz_error_init first in main): how
// to make an Error of a kind with a message, and where its fields lie in it.
typedef struct TzErrorInfo {
  void *(*make)(int32_t kind, void *message_obj, uint64_t message_meta);
  const TzType *frames_type; // of the elements of `#frames`: long
  uint32_t name_offset, message_offset, cause_offset, frames_offset;
} TzErrorInfo;

// The error in flight, which the caller takes over: NULL if there is none.
void *tz_error_take(void);

void tz_error_init(const TzErrorInfo *info);
// Throws a new Error of `kind` with the message `format` makes (UTF-8): it
// becomes tz_thrown, and the caller goes on as if nothing happened, to
// return soon; the program checks tz_thrown after calling it.
TZ_COLD void tz_throw(int32_t kind, const char *format, ...);
// The same with a message of the program's (borrowed).
TZ_COLD void tz_throw_str(int32_t kind, void *obj, uint64_t meta);
// "TypeError: message", as Error.prototype.toString makes it, appended.
void tz_buf_error_text(TzBuf *out, const void *error);
// The stack of an error, as `e.stack` gives it: the text, then a line for
// each place it passed. Returns whether there was any.
int tz_buf_error_stack(TzBuf *out, const void *error);
// What `cause` holds, or NULL.
const void *tz_error_cause(const void *error);
// Whether a field of an Error's layout is message, name or cause, which are
// no keys of the object in JavaScript.
int tz_error_base_field(const TzField *field);
// Whether the program named the error (`this.name = "HttpError"`), which
// makes `name` a key in JavaScript.
int tz_error_named(const void *error);

// SystemError: the fields of Error it sets, then its own.
typedef struct {
  TzStr message, name;
  int32_t errno_value;
  TzStr code, syscall;
  TzOptStr path, dest;
} TzSystemErrorParts;

#endif
