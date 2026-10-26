#ifndef TZ_BYTES_H
#define TZ_BYTES_H

#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"

enum {
  TZ_BYTES_U8 = 1,
  TZ_BYTES_I8 = 2,
  TZ_BYTES_U8C = 3,
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

typedef struct TzArrayBuffer {
  int64_t rc;
  const TzLayout *layout;
  uint8_t *data;
  int32_t length;
  uint32_t flags;
} TzArrayBuffer;

#define TZ_ABUF_STATIC 1u

typedef struct TzBytes {
  int64_t rc;
  const TzLayout *layout;
  uint8_t *data;
  int32_t length;
  int32_t offset;
  TzArrayBuffer *buffer;
} TzBytes;

static inline int32_t tz_bytes_kind(const void *object) {
  return (int32_t)(((const TzInstance *)object)->layout->flags >> TZ_LAYOUT_BYTES_SHIFT);
}

int32_t tz_bytes_element_size(int32_t kind);
double tz_bytes_get(const TzBytes *b, int64_t i);

static inline int64_t tz_bytes_byte_length(const TzBytes *b) {
  return (int64_t)b->length * tz_bytes_element_size(tz_bytes_kind(b));
}

void tz_buf_bytes_join(TzBuf *out, const TzBytes *b, const TzBuf *sep);
void tz_buf_bytes_hex(TzBuf *out, const uint8_t *data, int64_t length, int64_t max,
                      const char *more);

#endif
