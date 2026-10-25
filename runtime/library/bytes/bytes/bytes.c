// Binary data (docs/bytes.md): ArrayBuffer, the typed arrays and DataView.
//
// An ArrayBuffer is the memory; a typed array, a Buffer or a DataView is a
// window into it (TzBytes in tzrt.h) that holds a reference to its buffer.
// The code reads and writes elements itself, straight from `data` (see
// compiler/library/bytes/codegen/codegen_bytes.cpp); the runtime makes the
// objects and does what takes a loop: copies with conversions, fill, set, sort,
// join...
//
// Sema checks that what is stored fits in the element (docs/bytes.md,
// section 3), so the conversions here only need to agree with JavaScript
// where JavaScript converts too: between the kinds of typed arrays, and from
// doubles.

#include "runtime/core/tzrt/tzrt.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// kMaxLength of compiler/check/facts/facts.h: the proofs of indices count on
// it.
#define MAX_LENGTH ((int64_t)(1 << 30) - 1)

void tz_bytes_drop(void *object);
void tz_abuf_drop(void *object);
void tz_bytes_object_drop(void *object);

#define LAYOUT(kind, name, drop)                                                                   \
  {drop,                                                                                           \
   NULL,                                                                                           \
   0,                                                                                              \
   TZ_LAYOUT_BYTES | ((uint32_t)(kind) << TZ_LAYOUT_BYTES_SHIFT),                                  \
   name,                                                                                           \
   sizeof(name) - 1,                                                                               \
   TZ_BYTES_CLASS + (kind)}

// Not static: see the rules in tzrt.h. Indexed by TZ_BYTES_*.
const TzLayout tz_bytes_layouts[] = {
    {0},
    LAYOUT(TZ_BYTES_U8, "Uint8Array", tz_bytes_drop),
    LAYOUT(TZ_BYTES_I8, "Int8Array", tz_bytes_drop),
    LAYOUT(TZ_BYTES_U8C, "Uint8ClampedArray", tz_bytes_drop),
    LAYOUT(TZ_BYTES_I16, "Int16Array", tz_bytes_drop),
    LAYOUT(TZ_BYTES_U16, "Uint16Array", tz_bytes_drop),
    LAYOUT(TZ_BYTES_I32, "Int32Array", tz_bytes_drop),
    LAYOUT(TZ_BYTES_U32, "Uint32Array", tz_bytes_drop),
    LAYOUT(TZ_BYTES_F16, "Float16Array", tz_bytes_drop),
    LAYOUT(TZ_BYTES_F32, "Float32Array", tz_bytes_drop),
    LAYOUT(TZ_BYTES_F64, "Float64Array", tz_bytes_drop),
    LAYOUT(TZ_BYTES_BUFFER, "Buffer", tz_bytes_drop),
    LAYOUT(TZ_BYTES_ARRAY_BUFFER, "ArrayBuffer", tz_abuf_drop),
    LAYOUT(TZ_BYTES_DATA_VIEW, "DataView", tz_bytes_drop),
    LAYOUT(TZ_BYTES_TEXT_ENCODER, "TextEncoder", tz_bytes_object_drop),
    LAYOUT(TZ_BYTES_TEXT_DECODER, "TextDecoder", tz_bytes_object_drop),
};

int32_t tz_bytes_element_size(int32_t kind) {
  switch (kind) {
  case TZ_BYTES_I16:
  case TZ_BYTES_U16:
  case TZ_BYTES_F16:
    return 2;
  case TZ_BYTES_I32:
  case TZ_BYTES_U32:
  case TZ_BYTES_F32:
    return 4;
  case TZ_BYTES_F64:
    return 8;
  default:
    return 1;
  }
}

static int is_float(int32_t kind) {
  return kind == TZ_BYTES_F16 || kind == TZ_BYTES_F32 || kind == TZ_BYTES_F64;
}

//===----------------------------------------------------------------------===//
// Conversions, as JavaScript makes them
//===----------------------------------------------------------------------===//

// IEEE 754 binary16, rounding to the nearest, ties to even, as Float16Array
// stores a number. Done by hand: the C library has no conversion of its own
// that every target links.
TZ_HOT double tz_f16_to_f64(uint32_t h) {
  int exponent = (int)((h >> 10) & 0x1f);
  double mantissa = (double)(h & 0x3ff);
  double value;
  if (exponent == 0) {
    value = ldexp(mantissa, -24);
  } else if (exponent == 31) {
    value = mantissa != 0 ? NAN : INFINITY;
  } else {
    value = ldexp(mantissa + 1024, exponent - 25);
  }

  return (h & 0x8000) ? -value : value;
}

TZ_HOT uint32_t tz_f16_from_f64(double d) {
  uint32_t sign = signbit(d) ? 0x8000u : 0;
  if (isnan(d)) {
    return 0x7e00u;
  }
  double a = fabs(d);
  if (a >= 65520.0) { // rounds to infinity
    return sign | 0x7c00u;
  }
  int exponent;
  frexp(a, &exponent); // a = m * 2^exponent, 0.5 <= m < 1
  if (a == 0 || exponent - 1 < -14) {
    // Subnormal: a multiple of 2^-24; 1024 of them make the smallest normal.
    double q = rint(ldexp(a, 24));
    return sign | (uint32_t)q;
  }
  double q = rint(ldexp(a, 10 - (exponent - 1))); // 1024 to 2048
  if (q == 2048) {
    q = 1024;
    ++exponent;
  }

  return sign | ((uint32_t)(exponent - 1 + 15) << 10) | ((uint32_t)q - 1024);
}

// ToUint32 of a number, before the narrower ones take its low bits: NaN and
// the infinities are 0, fractions are cut off, the rest wraps around.
static uint32_t wrap32(double d) {
  if (!isfinite(d)) {
    return 0;
  }
  double t = fmod(trunc(d), 4294967296.0);
  if (t < 0) {
    t += 4294967296.0;
  }

  return (uint32_t)t;
}

// ToUint8Clamp: rounds half to even, as Uint8ClampedArray stores a number.
static uint8_t clamp8(double d) {
  if (!(d > 0)) {
    return 0; // NaN too
  }
  if (d >= 255) {
    return 255;
  }
  double f = floor(d);
  if (f + 0.5 < d) {
    return (uint8_t)(f + 1);
  }
  if (d < f + 0.5) {
    return (uint8_t)f;
  }

  return (uint8_t)((int)f % 2 == 0 ? f : f + 1);
}

static void put(uint8_t *data, int32_t kind, int64_t i, double v) {
  switch (kind) {
  case TZ_BYTES_U8:
  case TZ_BYTES_I8:
  case TZ_BYTES_BUFFER:
    data[i] = (uint8_t)wrap32(v);
    break;
  case TZ_BYTES_U8C:
    data[i] = clamp8(v);
    break;
  case TZ_BYTES_I16:
  case TZ_BYTES_U16:
    ((uint16_t *)data)[i] = (uint16_t)wrap32(v);
    break;
  case TZ_BYTES_I32:
  case TZ_BYTES_U32:
    ((uint32_t *)data)[i] = wrap32(v);
    break;
  case TZ_BYTES_F16:
    ((uint16_t *)data)[i] = (uint16_t)tz_f16_from_f64(v);
    break;
  case TZ_BYTES_F32:
    ((float *)data)[i] = (float)v;
    break;
  case TZ_BYTES_F64:
    ((double *)data)[i] = v;
    break;
  }
}

static double get(const uint8_t *data, int32_t kind, int64_t i) {
  switch (kind) {
  case TZ_BYTES_I8:
    return (int8_t)data[i];
  case TZ_BYTES_I16:
    return ((const int16_t *)data)[i];
  case TZ_BYTES_U16:
    return ((const uint16_t *)data)[i];
  case TZ_BYTES_I32:
    return ((const int32_t *)data)[i];
  case TZ_BYTES_U32:
    return ((const uint32_t *)data)[i];
  case TZ_BYTES_F16:
    return tz_f16_to_f64(((const uint16_t *)data)[i]);
  case TZ_BYTES_F32:
    return ((const float *)data)[i];
  case TZ_BYTES_F64:
    return ((const double *)data)[i];
  default:
    return data[i];
  }
}

double tz_bytes_get(const TzBytes *b, int64_t i) { return get(b->data, tz_bytes_kind(b), i); }

// Whether storing the elements of `from` in `kind` keeps their bits: the same
// kind, or integers of one size (an Int8Array clamped is the exception).
static int same_bits(int32_t kind, int32_t from) {
  if (kind == from) {
    return 1;
  }
  if (is_float(kind) || is_float(from) ||
      tz_bytes_element_size(kind) != tz_bytes_element_size(from)) {
    return 0;
  }

  return !(kind == TZ_BYTES_U8C && from == TZ_BYTES_I8);
}

//===----------------------------------------------------------------------===//
// Making them
//===----------------------------------------------------------------------===//

static TzArrayBuffer *abuf_alloc(int64_t length, int zero) {
  TzArrayBuffer *ab = (TzArrayBuffer *)tz_obj_alloc((int64_t)sizeof(TzArrayBuffer) + length);
  ab->rc = 1;
  ab->layout = &tz_bytes_layouts[TZ_BYTES_ARRAY_BUFFER];
  ab->data = (uint8_t *)(ab + 1);
  ab->length = (int32_t)length;
  ab->flags = 0;
  if (zero && length) {
    memset(ab->data, 0, (size_t)length);
  }

  return ab;
}

// A window of `length` elements of `kind` from byte `offset` of `buffer`,
// whose reference it takes over.
static TzBytes *view_of(int32_t kind, TzArrayBuffer *buffer, int64_t offset, int64_t length) {
  TzBytes *b = (TzBytes *)tz_obj_alloc(sizeof(TzBytes));
  b->rc = 1;
  b->layout = &tz_bytes_layouts[kind];
  b->data = buffer->data + offset;
  b->length = (int32_t)length;
  b->offset = (int32_t)offset;
  b->buffer = buffer;

  return b;
}

// A typed array of its own buffer; zeroed unless `zero` is 0.
static TzBytes *bytes_alloc(int32_t kind, int64_t length, int zero) {
  return view_of(kind, abuf_alloc(length * tz_bytes_element_size(kind), zero), 0, length);
}

void tz_bytes_drop(void *object) {
  TzBytes *b = (TzBytes *)object;
  tz_instance_release((TzInstance *)b->buffer);
  tz_obj_free(b);
}

void tz_abuf_drop(void *object) {
  // A buffer of embedBytes is a constant of the program, never released to
  // zero; its bytes too lie in the executable.
  tz_obj_free(object);
}

void tz_bytes_object_drop(void *object) { tz_obj_free(object); }

TzBytes *tz_bytes_new(int32_t kind, int64_t length) {
  if (length < 0 || length > MAX_LENGTH / tz_bytes_element_size(kind)) {
    tz_throw(TZ_RANGE_ERROR, "Invalid typed array length: %lld", (long long)length);
    return NULL;
  }

  return bytes_alloc(kind, length, 1);
}

// A new typed array whose elements the caller sets, all of them.
TzBytes *tz_bytes_new_unset(int32_t kind, int64_t length) { return bytes_alloc(kind, length, 0); }

TzArrayBuffer *tz_abuf_new(int64_t length) {
  if (length < 0 || length > MAX_LENGTH) {
    tz_throw(TZ_RANGE_ERROR, "Invalid array buffer length");
    return NULL;
  }

  return abuf_alloc(length, 1);
}

static const char *kind_name(int32_t kind) { return tz_bytes_layouts[kind].name; }

// `new Uint16Array(buffer, offset?, length?)`; `has` says which were given
// (1: offset, 2: length). The buffer is borrowed.
TzBytes *tz_bytes_view(int32_t kind, TzArrayBuffer *buffer, int64_t offset, int64_t length,
                       int32_t has) {
  int64_t size = tz_bytes_element_size(kind);
  if (!(has & 1)) {
    offset = 0;
  }
  if (offset < 0 || offset % size != 0) {
    if (offset < 0) {
      tz_throw(TZ_RANGE_ERROR, "Start offset %lld is outside the bounds of the buffer",
               (long long)offset);
    } else {
      tz_throw(TZ_RANGE_ERROR, "start offset of %s should be a multiple of %lld", kind_name(kind),
               (long long)size);
    }
    return NULL;
  }
  if (has & 2) {
    if (length < 0 || offset + length * size > buffer->length) {
      tz_throw(TZ_RANGE_ERROR, "Invalid typed array length: %lld", (long long)length);
      return NULL;
    }
  } else {
    if (buffer->length % size != 0) {
      tz_throw(TZ_RANGE_ERROR, "byte length of %s should be a multiple of %lld", kind_name(kind),
               (long long)size);
      return NULL;
    }
    if (offset > buffer->length) {
      tz_throw(TZ_RANGE_ERROR, "Start offset %lld is outside the bounds of the buffer",
               (long long)offset);
      return NULL;
    }
    length = (buffer->length - offset) / size;
  }
  tz_rc_inc(&buffer->rc);

  return view_of(kind, buffer, offset, length);
}

// `new DataView(buffer, offset?, length?)`, its length in bytes.
TzBytes *tz_dataview_new(TzArrayBuffer *buffer, int64_t offset, int64_t length, int32_t has) {
  if (!(has & 1)) {
    offset = 0;
  }
  if (offset < 0 || offset > buffer->length) {
    tz_throw(TZ_RANGE_ERROR, "Start offset %lld is outside the bounds of the buffer",
             (long long)offset);
    return NULL;
  }
  if (has & 2) {
    if (length < 0 || offset + length > buffer->length) {
      tz_throw(TZ_RANGE_ERROR, "Invalid DataView length %lld", (long long)length);
      return NULL;
    }
  } else {
    length = buffer->length - offset;
  }
  tz_rc_inc(&buffer->rc);

  return view_of(TZ_BYTES_DATA_VIEW, buffer, offset, length);
}

TZ_COLD void tz_dataview_range_error(void) {
  tz_throw(TZ_RANGE_ERROR, "Offset is outside the bounds of the DataView");
}

// The elements of an array of numbers, converted as JavaScript stores them.
TzBytes *tz_bytes_from_array(int32_t kind, const TzArray *a) {
  TzBytes *b = bytes_alloc(kind, a->length, 0);
  uint32_t n = a->length;
  switch (a->type->kind) {
  case TZ_T_INT:
    for (uint32_t i = 0; i < n; ++i) {
      put(b->data, kind, i, ((const int32_t *)a->data)[i]);
    }
    break;
  case TZ_T_LONG:
    for (uint32_t i = 0; i < n; ++i) {
      put(b->data, kind, i, (double)((const int64_t *)a->data)[i]);
    }
    break;
  default:
    for (uint32_t i = 0; i < n; ++i) {
      put(b->data, kind, i, ((const double *)a->data)[i]);
    }
    break;
  }

  return b;
}

// A typed array of `kind` with the values of another, converted.
TzBytes *tz_bytes_from_bytes(int32_t kind, const TzBytes *src) {
  int32_t from = tz_bytes_kind(src);
  TzBytes *b = bytes_alloc(kind, src->length, 0);
  if (same_bits(kind, from)) {
    memcpy(b->data, src->data, (size_t)src->length * (size_t)tz_bytes_element_size(kind));
    return b;
  }
  for (int32_t i = 0; i < src->length; ++i) {
    put(b->data, kind, i, get(src->data, from, i));
  }

  return b;
}

// The elements as an array of `element` (int, long or double): spreading,
// Array.from.
TzArray *tz_bytes_to_array(const TzBytes *b, const TzType *element) {
  int32_t kind = tz_bytes_kind(b);
  TzArray *a = tz_arr_new(element, b->length, b->length);
  for (int32_t i = 0; i < b->length; ++i) {
    double v = get(b->data, kind, i);
    if (element->kind == TZ_T_INT) {
      ((int32_t *)a->data)[i] = (int32_t)v;
    } else if (element->kind == TZ_T_LONG) {
      ((int64_t *)a->data)[i] = (int64_t)v;
    } else {
      ((double *)a->data)[i] = v;
    }
  }

  return a;
}

TzArrayBuffer *tz_bytes_buffer(TzBytes *b) {
  tz_rc_inc(&b->buffer->rc);

  return b->buffer;
}

// Buffer.isBuffer of a Uint8Array, which may be a Buffer.
int32_t tz_bytes_is_buffer(const TzBytes *b) { return tz_bytes_kind(b) == TZ_BYTES_BUFFER; }

//===----------------------------------------------------------------------===//
// Methods
//===----------------------------------------------------------------------===//

// A relative position: from the end if negative, clamped to [0, length].
static int64_t relative(int64_t position, int64_t length) {
  if (position < 0) {
    return position + length < 0 ? 0 : position + length;
  }

  return position > length ? length : position;
}

// `has` says which of start (1) and end (2) were given.
static void range_of(int64_t length, int64_t start, int64_t end, int32_t has, int64_t *from,
                     int64_t *to) {
  *from = (has & 1) ? relative(start, length) : 0;
  *to = (has & 2) ? relative(end, length) : length;
  if (*to < *from) {
    *to = *from;
  }
}

TzBytes *tz_bytes_subarray(TzBytes *b, int64_t start, int64_t end, int32_t has) {
  int64_t from, to;
  range_of(b->length, start, end, has, &from, &to);
  int32_t kind = tz_bytes_kind(b);
  tz_rc_inc(&b->buffer->rc);

  return view_of(kind, b->buffer, b->offset + from * tz_bytes_element_size(kind), to - from);
}

TzBytes *tz_bytes_slice(TzBytes *b, int64_t start, int64_t end, int32_t has) {
  int64_t from, to;
  range_of(b->length, start, end, has, &from, &to);
  int32_t kind = tz_bytes_kind(b);
  int64_t size = tz_bytes_element_size(kind);
  TzBytes *copy = bytes_alloc(kind, to - from, 0);
  if (to > from) {
    memcpy(copy->data, b->data + from * size, (size_t)((to - from) * size));
  }

  return copy;
}

TzArrayBuffer *tz_abuf_slice(TzArrayBuffer *ab, int64_t start, int64_t end, int32_t has) {
  int64_t from, to;
  range_of(ab->length, start, end, has, &from, &to);
  TzArrayBuffer *copy = abuf_alloc(to - from, 0);
  if (to > from) {
    memcpy(copy->data, ab->data + from, (size_t)(to - from));
  }

  return copy;
}

void tz_bytes_fill(TzBytes *b, double value, int64_t start, int64_t end, int32_t has) {
  int64_t from, to;
  range_of(b->length, start, end, has, &from, &to);
  int32_t kind = tz_bytes_kind(b);
  if (tz_bytes_element_size(kind) == 1 && to > from) {
    // One byte: the value stored is the same everywhere.
    uint8_t byte[1];
    put(byte, kind, 0, value);
    memset(b->data + from, byte[0], (size_t)(to - from));
    return;
  }
  for (int64_t i = from; i < to; ++i) {
    put(b->data, kind, i, value);
  }
}

TZ_COLD static int set_out_of_bounds(void) {
  tz_throw(TZ_RANGE_ERROR, "offset is out of bounds");

  return 0;
}

// `dst.set(src, offset)` of an array of numbers.
void tz_bytes_set_array(TzBytes *dst, const TzArray *src, int64_t offset) {
  if (offset < 0 || offset + src->length > dst->length) {
    set_out_of_bounds();
    return;
  }
  int32_t kind = tz_bytes_kind(dst);
  for (uint32_t i = 0; i < src->length; ++i) {
    double v = src->type->kind == TZ_T_INT    ? ((const int32_t *)src->data)[i]
               : src->type->kind == TZ_T_LONG ? (double)((const int64_t *)src->data)[i]
                                              : ((const double *)src->data)[i];
    put(dst->data, kind, offset + i, v);
  }
}

// `dst.set(src, offset)` of a typed array, which may share memory with dst:
// then the values are read before any is written, as JavaScript does.
void tz_bytes_set_bytes(TzBytes *dst, const TzBytes *src, int64_t offset) {
  if (offset < 0 || offset + src->length > dst->length) {
    set_out_of_bounds();
    return;
  }
  int32_t kind = tz_bytes_kind(dst), from = tz_bytes_kind(src);
  int64_t size = tz_bytes_element_size(kind);
  if (same_bits(kind, from)) {
    memmove(dst->data + offset * size, src->data, (size_t)(src->length * size));
    return;
  }
  double *values = (double *)tz_alloc((size_t)src->length * sizeof(double) + 1);
  for (int32_t i = 0; i < src->length; ++i) {
    values[i] = get(src->data, from, i);
  }
  for (int32_t i = 0; i < src->length; ++i) {
    put(dst->data, kind, offset + i, values[i]);
  }
  tz_free(values);
}

// indexOf (mode 0), lastIndexOf (1) and includes (2), which also finds NaN.
int32_t tz_bytes_index_of(const TzBytes *b, double value, int64_t from, int32_t has_from,
                          int32_t mode) {
  int32_t kind = tz_bytes_kind(b);
  int64_t n = b->length;
  if (mode == 1) {
    int64_t i = has_from ? (from < 0 ? from + n : (from >= n ? n - 1 : from)) : n - 1;
    for (; i >= 0; --i) {
      if (get(b->data, kind, i) == value) {
        return (int32_t)i;
      }
    }
    return -1;
  }
  int64_t i = has_from ? relative(from, n) : 0;
  int nan = mode == 2 && isnan(value);
  for (; i < n; ++i) {
    double v = get(b->data, kind, i);
    if (v == value || (nan && isnan(v))) {
      return mode == 2 ? 1 : (int32_t)i;
    }
  }

  return mode == 2 ? 0 : -1;
}

// The text of an element, as String(x) of the number.
static void buf_element(TzBuf *out, const TzBytes *b, int32_t kind, int64_t i) {
  char text[32];
  size_t n;
  double v = get(b->data, kind, i);
  if (is_float(kind)) {
    n = tz_format_f64(v, text);
  } else {
    n = (size_t)snprintf(text, sizeof text, "%lld", (long long)v);
  }
  tz_buf_ascii_n(out, text, n);
}

void tz_buf_bytes_join(TzBuf *out, const TzBytes *b, const TzBuf *sep) {
  int32_t kind = tz_bytes_kind(b);
  for (int32_t i = 0; i < b->length; ++i) {
    if (i > 0) {
      if (sep) {
        tz_buf_units(out, sep->data, sep->length);
      } else {
        tz_buf_push(out, ',');
      }
    }
    buf_element(out, b, kind, i);
  }
}

void tz_bytes_join(TzStr *out, const TzBytes *b, void *sep_obj, uint64_t sep_meta,
                   int32_t has_sep) {
  TzBuf text, sep;
  tz_buf_init(&text);
  tz_buf_init(&sep);
  if (has_sep) {
    TzStr s = {sep_obj, sep_meta};
    tz_buf_str(&sep, s);
  }
  tz_buf_bytes_join(&text, b, has_sep ? &sep : NULL);
  tz_buf_free(&sep);
  *out = tz_buf_take(&text);
}

void tz_bytes_reverse(TzBytes *b) {
  size_t size = (size_t)tz_bytes_element_size(tz_bytes_kind(b));
  uint8_t tmp[8];
  for (int64_t i = 0, j = (int64_t)b->length - 1; i < j; ++i, --j) {
    memcpy(tmp, b->data + (size_t)i * size, size);
    memcpy(b->data + (size_t)i * size, b->data + (size_t)j * size, size);
    memcpy(b->data + (size_t)j * size, tmp, size);
  }
}

// The order of sort() without a function: numbers ascending, -0 before +0,
// NaN last.
static int less(double x, double y) {
  if (isnan(x)) {
    return 0;
  }
  if (isnan(y)) {
    return 1;
  }
  if (x == 0 && y == 0) {
    return signbit(x) && !signbit(y);
  }

  return x < y;
}

static void merge_sort(double *items, double *tmp, size_t n) {
  if (n < 2) {
    return;
  }
  size_t half = n / 2;
  merge_sort(items, tmp, half);
  merge_sort(items + half, tmp, n - half);
  size_t i = 0, j = half, k = 0;
  while (i < half && j < n) {
    tmp[k++] = less(items[j], items[i]) ? items[j++] : items[i++];
  }
  while (i < half) {
    tmp[k++] = items[i++];
  }
  while (j < n) {
    tmp[k++] = items[j++];
  }
  memcpy(items, tmp, n * sizeof(double));
}

void tz_bytes_sort(TzBytes *b) {
  int32_t kind = tz_bytes_kind(b);
  size_t n = (size_t)b->length;
  if (n < 2) {
    return;
  }
  if (kind == TZ_BYTES_U8 || kind == TZ_BYTES_U8C || kind == TZ_BYTES_BUFFER) {
    // Counting sort: bytes need no comparisons.
    size_t counts[256] = {0};
    for (size_t i = 0; i < n; ++i) {
      ++counts[b->data[i]];
    }
    size_t at = 0;
    for (int v = 0; v < 256; ++v) {
      memset(b->data + at, v, counts[v]);
      at += counts[v];
    }
    return;
  }
  double *items = (double *)tz_alloc(n * sizeof(double));
  double *tmp = (double *)tz_alloc(n * sizeof(double));
  for (size_t i = 0; i < n; ++i) {
    items[i] = get(b->data, kind, (int64_t)i);
  }
  merge_sort(items, tmp, n);
  for (size_t i = 0; i < n; ++i) {
    put(b->data, kind, (int64_t)i, items[i]);
  }
  tz_free(items);
  tz_free(tmp);
}

// sort(compare): the values in the representation of the language (i32,
// i64 or double, `size` bytes), which the comparison function the compiler
// made reads (see CodeGen::sortComparator).
typedef int32_t (*TzCompare)(void *ctx, const void *x, const void *y);

static void merge_sort_with(char *items, char *tmp, size_t n, size_t size, TzCompare compare,
                            void *ctx) {
  if (n < 2) {
    return;
  }
  size_t half = n / 2;
  merge_sort_with(items, tmp, half, size, compare, ctx);
  merge_sort_with(items + half * size, tmp, n - half, size, compare, ctx);
  size_t i = 0, j = half, k = 0;
  while (i < half && j < n) {
    // Stable: an element of the right half goes first only if it is less.
    if (compare(ctx, items + j * size, items + i * size) < 0) {
      memcpy(tmp + k++ * size, items + j++ * size, size);
    } else {
      memcpy(tmp + k++ * size, items + i++ * size, size);
    }
  }
  while (i < half) {
    memcpy(tmp + k++ * size, items + i++ * size, size);
  }
  while (j < n) {
    memcpy(tmp + k++ * size, items + j++ * size, size);
  }
  memcpy(items, tmp, n * size);
}

void tz_bytes_sort_with(TzBytes *b, TzCompare compare, void *ctx) {
  int32_t kind = tz_bytes_kind(b);
  size_t n = (size_t)b->length;
  if (n < 2) {
    return;
  }
  size_t size = is_float(kind) ? sizeof(double) : kind == TZ_BYTES_U32 ? sizeof(int64_t) : 4;
  char *items = (char *)tz_alloc(n * size);
  char *tmp = (char *)tz_alloc(n * size);
  for (size_t i = 0; i < n; ++i) {
    double v = get(b->data, kind, (int64_t)i);
    if (size == 4) {
      int32_t x = (int32_t)v;
      memcpy(items + i * 4, &x, 4);
    } else if (!is_float(kind)) {
      int64_t x = (int64_t)v;
      memcpy(items + i * 8, &x, 8);
    } else {
      memcpy(items + i * 8, &v, 8);
    }
  }
  merge_sort_with(items, tmp, n, size, compare, ctx);
  for (size_t i = 0; i < n; ++i) {
    double v;
    if (size == 4) {
      int32_t x;
      memcpy(&x, items + i * 4, 4);
      v = x;
    } else if (!is_float(kind)) {
      int64_t x;
      memcpy(&x, items + i * 8, 8);
      v = (double)x;
    } else {
      memcpy(&v, items + i * 8, 8);
    }
    put(b->data, kind, (int64_t)i, v);
  }
  tz_free(items);
  tz_free(tmp);
}

void tz_bytes_copy_within(TzBytes *b, int64_t target, int64_t start, int64_t end, int32_t has_end) {
  int64_t n = b->length;
  int64_t to = relative(target, n), from = relative(start, n);
  int64_t last = has_end ? relative(end, n) : n;
  int64_t count = last - from;
  if (count > n - to) {
    count = n - to;
  }
  if (count <= 0) {
    return;
  }
  size_t size = (size_t)tz_bytes_element_size(tz_bytes_kind(b));
  memmove(b->data + (size_t)to * size, b->data + (size_t)from * size, (size_t)count * size);
}

TzBytes *tz_bytes_to_reversed(TzBytes *b) {
  TzBytes *copy = tz_bytes_slice(b, 0, 0, 0);
  tz_bytes_reverse(copy);

  return copy;
}

TzBytes *tz_bytes_to_sorted(TzBytes *b) {
  TzBytes *copy = tz_bytes_slice(b, 0, 0, 0);
  tz_bytes_sort(copy);

  return copy;
}

// `with(index, value)`: a copy with one element replaced.
TzBytes *tz_bytes_with(TzBytes *b, int64_t index, double value) {
  int64_t at = index < 0 ? index + b->length : index;
  if (at < 0 || at >= b->length) {
    tz_throw(TZ_RANGE_ERROR, "Invalid typed array index");
    return NULL;
  }
  TzBytes *copy = tz_bytes_slice(b, 0, 0, 0);
  put(copy->data, tz_bytes_kind(b), at, value);

  return copy;
}

//===----------------------------------------------------------------------===//
// Printing
//===----------------------------------------------------------------------===//

void tz_buf_bytes_hex(TzBuf *out, const uint8_t *data, int64_t length, int64_t max,
                      const char *more) {
  static const char digits[] = "0123456789abcdef";
  int64_t shown = length < max ? length : max;
  for (int64_t i = 0; i < shown; ++i) {
    if (i > 0) {
      tz_buf_push(out, ' ');
    }
    tz_buf_push(out, (uint16_t)digits[data[i] >> 4]);
    tz_buf_push(out, (uint16_t)digits[data[i] & 15]);
  }
  if (length > max) {
    char text[64];
    int64_t rest = length - max;
    int n = snprintf(text, sizeof text, " ... %lld more %s%s", (long long)rest, more,
                     rest > 1 ? "s" : "");
    tz_buf_ascii_n(out, text, (size_t)n);
  }
}
