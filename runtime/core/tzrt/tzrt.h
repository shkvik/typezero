#ifndef TZRT_H
#define TZRT_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define TZ_NORETURN __declspec(noreturn)
#else
#define TZ_NORETURN __attribute__((noreturn))
#endif

#ifndef TZ_PARALLEL
#define TZ_PARALLEL 0
#endif
#if TZ_PARALLEL
#define TZ_THREAD_LOCAL _Thread_local
#else
#define TZ_THREAD_LOCAL
#endif
#if defined(__clang__) || defined(__GNUC__)
#define TZ_HOT __attribute__((hot))
#define TZ_COLD __attribute__((cold))
#define TZ_NOINLINE __attribute__((noinline))
#else
#define TZ_HOT
#define TZ_COLD
#define TZ_NOINLINE __declspec(noinline)
#endif

enum {
  TZ_T_INT32 = 1,
  TZ_T_INT64 = 2,
  TZ_T_DOUBLE = 3,
  TZ_T_BOOL = 4,
  TZ_T_STRING = 5,
  TZ_T_ARRAY = 6,
  TZ_T_OPTIONAL = 7,
  TZ_T_UNDEFINED = 8,
  TZ_T_FUNCTION = 9,
  TZ_T_OBJECT = 10,
  TZ_T_DECIMAL = 11,
  TZ_T_RESULT = 12,
  TZ_T_JSON = 13,
  TZ_T_UNION = 14,
  TZ_T_ATOM = 15,
  TZ_T_UNKNOWN = 16,
  TZ_T_GETTER = 17,
  TZ_T_INT8 = 18,
  TZ_T_INT16 = 19,
  TZ_T_UINT8 = 20,
  TZ_T_UINT16 = 21,
  TZ_T_UINT32 = 22,
  TZ_T_UINT64 = 23,
};

static inline int tz_kind_integer(uint32_t kind) {
  return kind == TZ_T_INT32 || kind == TZ_T_INT64 || (kind >= TZ_T_INT8 && kind <= TZ_T_UINT64);
}

static inline int tz_kind_unsigned(uint32_t kind) {
  return kind >= TZ_T_UINT8 && kind <= TZ_T_UINT64;
}

static inline int64_t tz_load_integer(uint32_t kind, const void *p) {
  switch (kind) {
  case TZ_T_INT8:
    return *(const int8_t *)p;
  case TZ_T_INT16:
    return *(const int16_t *)p;
  case TZ_T_INT32:
    return *(const int32_t *)p;
  case TZ_T_UINT8:
    return *(const uint8_t *)p;
  case TZ_T_UINT16:
    return *(const uint16_t *)p;
  case TZ_T_UINT32:
    return *(const uint32_t *)p;
  default:
    return *(const int64_t *)p;
  }
}

static inline void tz_store_integer(uint32_t kind, void *p, int64_t v) {
  switch (kind) {
  case TZ_T_INT8:
  case TZ_T_UINT8:
    *(uint8_t *)p = (uint8_t)v;
    break;
  case TZ_T_INT16:
  case TZ_T_UINT16:
    *(uint16_t *)p = (uint16_t)v;
    break;
  case TZ_T_INT32:
  case TZ_T_UINT32:
    *(uint32_t *)p = (uint32_t)v;
    break;
  default:
    *(int64_t *)p = v;
    break;
  }
}

static inline double tz_integer_double(uint32_t kind, int64_t v) {
  return kind == TZ_T_UINT64 ? (double)(uint64_t)v : (double)v;
}

static inline int tz_integer_fits(uint32_t kind, int64_t v, int is_unsigned) {
  if (is_unsigned && v < 0) {
    return kind == TZ_T_UINT64;
  }
  if (kind == TZ_T_UINT64) {
    return is_unsigned || v >= 0;
  }
  switch (kind) {
  case TZ_T_INT8:
    return v >= -128 && v <= 127;
  case TZ_T_INT16:
    return v >= -32768 && v <= 32767;
  case TZ_T_INT32:
    return v >= -2147483647 - 1 && v <= 2147483647;
  case TZ_T_UINT8:
    return v >= 0 && v <= 255;
  case TZ_T_UINT16:
    return v >= 0 && v <= 65535;
  case TZ_T_UINT32:
    return v >= 0 && v <= 4294967295LL;
  default:
    return 1;
  }
}

static inline const char *tz_integer_name(uint32_t kind) {
  switch (kind) {
  case TZ_T_INT8:
    return "int8";
  case TZ_T_INT16:
    return "int16";
  case TZ_T_INT32:
    return "int32";
  case TZ_T_UINT8:
    return "uint8";
  case TZ_T_UINT16:
    return "uint16";
  case TZ_T_UINT32:
    return "uint32";
  case TZ_T_UINT64:
    return "uint64";
  default:
    return "int";
  }
}

typedef struct TzType {
  uint32_t kind;
  uint32_t size;
  const struct TzType *inner;
} TzType;

typedef struct TzResultType {
  TzType base;
  const TzType *error;
  uint32_t value_offset, error_offset, ok_offset, site_offset;
} TzResultType;

typedef struct TzErrSite {
  const char *text;
  uint32_t length;
} TzErrSite;

#define TZ_UNION_SLOTS 12
typedef struct TzUnionType {
  TzType base;
  const TzType *slots[TZ_UNION_SLOTS];
} TzUnionType;

static inline int tz_optional_has(const TzType *type, const void *value) {
  if (type->inner->kind == TZ_T_OBJECT || type->inner->kind == TZ_T_JSON ||
      type->inner->kind == TZ_T_UNKNOWN) {
    return *(void *const *)value != NULL;
  }
  if (type->inner->kind == TZ_T_UNION || type->inner->kind == TZ_T_ATOM) {
    return *(const int32_t *)value != 0;
  }

  return ((const uint8_t *)value)[type->inner->size] != 0;
}

typedef struct TzField {
  const char *name;
  uint32_t name_length;
  uint32_t offset;
  const TzType *type;
} TzField;

enum {
  TZ_LAYOUT_TUPLE = 1,
  TZ_LAYOUT_GENERATOR = 2,
  TZ_LAYOUT_ARRAY_ITERATOR = 4,
  TZ_LAYOUT_STRING_ITERATOR = 8,
  TZ_LAYOUT_ITERATOR_HELPER = 16,
  TZ_LAYOUT_REGEX = 32,
  TZ_LAYOUT_MATCH = 64,
  TZ_LAYOUT_GROUPS = 128,
  TZ_LAYOUT_REGEX_ITERATOR = 256,
  TZ_LAYOUT_DATE = 1024,
  TZ_LAYOUT_DATE_FORMAT = 2048,
  TZ_LAYOUT_DICT = 4096,
  TZ_LAYOUT_DICT_ITERATOR = 8192,
  TZ_LAYOUT_DICT_VALUES = 16384,
  TZ_LAYOUT_DICT_ENTRIES = 32768,
  TZ_LAYOUT_DICT_OF_SET = 65536,
  TZ_LAYOUT_PROMISE = 131072,
  TZ_LAYOUT_TIMER = 262144,
  TZ_LAYOUT_BYTES = 524288,
  TZ_LAYOUT_BOX = 1048576,
  TZ_LAYOUT_CLASS = 2097152,
  TZ_LAYOUT_BUILTIN = 4194304,
  TZ_LAYOUT_INTL = 8388608,
  TZ_LAYOUT_ASYNC_ITERATOR = 268435456,
  TZ_LAYOUT_ASYNC_GENERATOR = 536870912,
  TZ_LAYOUT_LIBRARY = 1073741824,
};

#define TZ_LAYOUT_GROUP 0x80000000u

#define TZ_LAYOUT_BYTES_SHIFT 24

#define TZ_BYTES_CLASS 0x3FFFFF00u

#define TZ_FIELD_METHOD 0xFFFFFFFFu

#define TZ_FIELD_GETTER 0xFFFFFFFEu

typedef struct TzLayout {
  void (*drop)(void *object);
  const TzField *fields;
  uint32_t count;
  uint32_t flags;
  const char *name;
  uint32_t name_length;
  uint32_t class_id;
} TzLayout;

static inline int tz_field_is_symbol(const TzLayout *layout, const TzField *field) {
  static const char prefix[] = "Symbol(";
  if (!(layout->flags & TZ_LAYOUT_LIBRARY) || field->name_length < sizeof prefix - 1) {
    return 0;
  }
  for (size_t i = 0; i < sizeof prefix - 1; ++i) {
    if (field->name[i] != prefix[i]) {
      return 0;
    }
  }

  return 1;
}

typedef struct TzInstance {
  int64_t rc;
  const TzLayout *layout;
} TzInstance;

typedef struct TzGetterType {
  TzType base;
  const TzType *result;
  void (*get)(const void *object, void *out);
} TzGetterType;

typedef void *(*TzMaker)(const void *parts);

#endif
