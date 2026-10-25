// What node:stream, which tz has written in tz (library/stream/stream.tz), asks
// of the runtime through its intrinsics
// (compiler/library/stream/sema/sema_stream.cpp).

#include "runtime/library/stream/stream/stream.h"
#include "runtime/core/output/output.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/bytes/bytes/bytes.h"
#include "runtime/library/reflect/reflect/reflect.h"

// `__tz_chunk<T>(u)` of an unknown that holds no T: the types of the
// library promise it never happens, so it is a fault of tz.
void tz_lib_bad_chunk(const void *u, const char *type, uint32_t type_length) {
  (void)u;
  tz_panicf("a chunk of a stream is not a %.*s, as its type says", (int)type_length, type);
}

// `__tz_stdWrite(fd, buffer)`: the bytes, to stdout (1) or stderr (2), where
// console.log writes.
void tz_lib_std_write(int32_t fd, const TzBytes *b) {
  tz_std_write_bytes(fd, b->data, (size_t)tz_bytes_byte_length(b));
}

// Whether the unknown `u` (borrowed) holds binary data of `kind` (TZ_BYTES_*),
// which it holds as the object itself.
int32_t tz_lib_is_bytes(const void *u, int32_t kind) {
  const TzLayout *layout = tz_unknown_layout(u);

  return layout && (int32_t)(layout->flags >> TZ_LAYOUT_BYTES_SHIFT) == kind;
}
