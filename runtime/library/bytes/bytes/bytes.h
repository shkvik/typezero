// Binary data (docs/bytes.md).

#ifndef TZ_BYTES_H
#define TZ_BYTES_H

#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"

// The kinds of the objects of binary data (docs/bytes.md), numbered as
// Type::Bytes in compiler/program/types/types.h. The typed arrays come first,
// by their elements; a Buffer is a Uint8Array that prints as node prints a
// Buffer.
enum {
  TZ_BYTES_U8 = 1,
  TZ_BYTES_I8 = 2,
  TZ_BYTES_U8C = 3, // Uint8ClampedArray
  TZ_BYTES_I16 = 4,
  TZ_BYTES_U16 = 5,
  TZ_BYTES_I32 = 6,
  TZ_BYTES_U32 = 7,
  TZ_BYTES_F16 = 8,
  TZ_BYTES_F32 = 9,
  TZ_BYTES_F64 = 10,
  TZ_BYTES_BUFFER = 11,
  TZ_BYTES_ARRAY_BUFFER = 12,
  TZ_BYTES_DATA_VIEW = 13,
  TZ_BYTES_TEXT_ENCODER = 14,
  TZ_BYTES_TEXT_DECODER = 15,
};

// An ArrayBuffer: the memory, which its views share. Its bytes follow the
// header in the same block, or, with TZ_ABUF_STATIC, lie in the executable
// (embedBytes) and are never freed. No buffer is longer than kMaxLength of
// compiler/check/facts/facts.h, so that the proofs of indices hold. Must match
// Lowering (compiler/library/bytes/codegen/codegen_bytes.cpp).
typedef struct TzArrayBuffer {
  int64_t rc;
  const TzLayout *layout;
  uint8_t *data;
  int32_t length; // byteLength
  uint32_t flags;
} TzArrayBuffer;

#define TZ_ABUF_STATIC 1u

// A typed array, a Buffer or a DataView: a window of `length` elements (of
// bytes, for a DataView) into the memory of `buffer`, from its byte `offset`
// on; `data` is where the window starts. Neither ever changes: the length of
// a typed array is fixed, and the code reads `data` and `length` straight
// from here (see Lowering::bytesData).
typedef struct TzBytes {
  int64_t rc;
  const TzLayout *layout;
  uint8_t *data;
  int32_t length;
  int32_t offset;
  TzArrayBuffer *buffer; // counted
} TzBytes;

// The kind of an object of binary data, from its layout.
static inline int32_t tz_bytes_kind(const void *object) {
  return (int32_t)(((const TzInstance *)object)->layout->flags >> TZ_LAYOUT_BYTES_SHIFT);
}

// The bytes of an element of a typed array of `kind`.
int32_t tz_bytes_element_size(int32_t kind);
// Element i of a typed array as a double, which holds every element exactly.
double tz_bytes_get(const TzBytes *b, int64_t i);

// The bytes of a view (a typed array, a Buffer or a DataView), and how many:
// what writeFileSync writes of it.
static inline int64_t tz_bytes_byte_length(const TzBytes *b) {
  return (int64_t)b->length * tz_bytes_element_size(tz_bytes_kind(b));
}

// Appends the text of a typed array, as join() gives it.
void tz_buf_bytes_join(TzBuf *out, const TzBytes *b, const TzBuf *sep);
// Appends `<Buffer 68 65>`, and `ArrayBuffer`'s `<00 01>`: the hex of the
// first `max` bytes, and how many more there are.
void tz_buf_bytes_hex(TzBuf *out, const uint8_t *data, int64_t length, int64_t max,
                      const char *more);

#endif
