// Declarations shared between the files of the runtime. Programs call the
// runtime through the functions declared in src/codegen/codegen.cpp
// (CodeGen::declareRuntime); keep both in sync.

#ifndef TZRT_H
#define TZRT_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define TZ_NORETURN __declspec(noreturn)
#else
#define TZ_NORETURN __attribute__((noreturn))
#endif

// TZ_HOT marks the fast paths that programs call in their loops. tz copies
// those a program calls into the program, so that the optimizer can inline
// them there (see linkRuntime in src/driver/driver.cpp); every other call
// goes to tzrt.lib. Two rules follow:
//
//   - State is never `static`: a copied function must reach the one variable
//     in tzrt.lib, not a copy of its own. The driver rejects static state.
//   - A TZ_HOT function stays small: it calls its slow path as a separate
//     TZ_NOINLINE function that is not `static`, which stays in tzrt.lib.
//     The driver does not copy a function that would bring more than about
//     a hundred instructions along.
//
// TZ_COLD: rarely called; the optimizer keeps calls to it out of the hot path.
#if defined(__clang__) || defined(__GNUC__)
#define TZ_HOT __attribute__((hot))
#define TZ_COLD __attribute__((cold))
#define TZ_NOINLINE __attribute__((noinline))
#else
#define TZ_HOT
#define TZ_COLD
#define TZ_NOINLINE __declspec(noinline)
#endif

// Buffered stdout.
void tz_out_write(const char *data, size_t len);

// Formats a double like JavaScript's Number#toString; returns the length.
// `out` must hold at least 32 bytes.
size_t tz_format_f64(double value, char *out);

TZ_COLD TZ_NORETURN void tz_panic(const char *message, int64_t len);
TZ_COLD TZ_NORETURN void tz_panicf(const char *format, ...);

// The allocator (alloc.c). tz_alloc panics when memory runs out; tz_free
// takes any block from tz_alloc, or NULL.
void *tz_alloc(size_t size);
void tz_free(void *p);

//===----------------------------------------------------------------------===//
// Values of any type
//===----------------------------------------------------------------------===//

// Describes a type to the runtime; the compiler emits one constant per type
// it needs (CodeGen::typeDescriptor). `size` is the size of a value in
// memory. `inner` is the element type of an array, or the value type of
// `T | undefined`, which is laid out as { T value; uint8_t has }, except
// for an object, whose `T | undefined` is the reference, NULL for undefined.
enum {
  TZ_T_INT = 1,
  TZ_T_LONG = 2,
  TZ_T_DOUBLE = 3,
  TZ_T_BOOL = 4,
  TZ_T_STRING = 5,
  TZ_T_ARRAY = 6,
  TZ_T_OPTIONAL = 7,
  TZ_T_UNDEFINED = 8,
  TZ_T_FUNCTION = 9,
  TZ_T_OBJECT = 10,
};

typedef struct TzType {
  uint32_t kind;
  uint32_t size;
  const struct TzType *inner;
} TzType;

// Whether a `T | undefined` at `value` holds a value.
static inline int tz_optional_has(const TzType *type, const void *value) {
  if (type->inner->kind == TZ_T_OBJECT)
    return *(void *const *)value != NULL;
  return ((const uint8_t *)value)[type->inner->size] != 0;
}

//===----------------------------------------------------------------------===//
// Functions and the objects of closures (closure.c)
//===----------------------------------------------------------------------===//

// The environments of closures and the boxes of the variables they share
// start with this header. The compiler emits a `drop` function for each kind
// of them, which releases what the object holds and frees it.
typedef struct TzObject {
  int64_t rc;
  void (*drop)(struct TzObject *);
} TzObject;

// A function value: what it is, and its environment (NULL if it captures
// nothing).
typedef struct {
  const struct TzFunctionInfo *info;
  TzObject *env;
} TzFunction;

typedef struct TzFunctionInfo {
  void *code;
  struct {
    void *obj;
    uint64_t meta;
  } name; // a TzStr, empty for an anonymous function
} TzFunctionInfo;

int64_t tz_obj_live_objects(void);
void *tz_obj_alloc(int64_t size);
void tz_obj_free(void *object);

static inline void tz_obj_retain(TzObject *o) {
  if (o)
    ++o->rc;
}

static inline void tz_obj_release(TzObject *o) {
  if (o && --o->rc == 0)
    o->drop(o);
}

//===----------------------------------------------------------------------===//
// Objects of the language
//===----------------------------------------------------------------------===//

// An object is allocated as the objects of closures are, but starts with
// { rc, layout }: a constant the compiler emits for each layout and each
// order of the keys (CodeGen::layoutDescriptor). The fields follow, at the
// offsets the layout gives, each stored as a value of its type.
typedef struct TzField {
  const char *name; // UTF-8
  uint32_t name_length;
  uint32_t offset; // from the start of the object
  const TzType *type;
} TzField;

typedef struct TzLayout {
  // Releases what the fields hold, and frees the object.
  void (*drop)(void *object);
  const TzField *fields; // in the order of the keys, for printing
  uint32_t count;
} TzLayout;

typedef struct TzInstance {
  int64_t rc;
  const TzLayout *layout;
} TzInstance;

static inline void tz_instance_release(TzInstance *o) {
  if (o && --o->rc == 0)
    o->layout->drop(o);
}

//===----------------------------------------------------------------------===//
// Strings (string.c)
//===----------------------------------------------------------------------===//

typedef struct {
  void *obj;
  uint64_t meta;
} TzStr;

int64_t tz_str_live_objects(void);
void tz_str_retain_value(TzStr s);
void tz_str_release_value(TzStr s);
// A new string from UTF-16 code units, stored one byte per unit if possible.
TzStr tz_str_from_utf16(const uint16_t *units, size_t length);
TzStr tz_str_from_ascii(const char *text, size_t length);
int32_t tz_str_equal(void *a_obj, uint64_t a_meta, void *b_obj, uint64_t b_meta);
int32_t tz_str_compare(void *a_obj, uint64_t a_meta, void *b_obj, uint64_t b_meta);
// `items` joined by `sep`, or by "," when sep is NULL.
void tz_str_join(TzStr *out, const TzStr *items, uint32_t n, const TzStr *sep);
// Writes UTF-16 to stdout as UTF-8; lone surrogates become U+FFFD.
void tz_out_write_utf16(const uint16_t *units, size_t length);
void tz_str_from_f64(TzStr *out, double value);

// Unicode (unicode.c).
int tz_case_map(uint32_t cp, int upper, uint32_t out[3]);
int tz_is_cased(uint32_t cp);
int tz_is_case_ignorable(uint32_t cp);
int tz_is_js_space(uint32_t unit);

// Numbers in text (number.c), for Number(s), parseInt and parseFloat once
// string.c has taken off the whitespace around them: `text` holds the ASCII
// characters of the string up to its first other one, and `complete` says
// whether that was all of it.
double tz_number_from_ascii(const char *text, size_t length, int complete);
double tz_parse_int_ascii(const char *text, size_t length, int32_t radix);
double tz_parse_float_ascii(const char *text, size_t length);

// A growable buffer of UTF-16 code units, for building strings in C.
typedef struct {
  uint16_t *data;
  size_t length, capacity;
} TzBuf;

void tz_buf_init(TzBuf *b);
void tz_buf_free(TzBuf *b);
void tz_buf_push(TzBuf *b, uint16_t unit);
void tz_buf_ascii(TzBuf *b, const char *text);
void tz_buf_ascii_n(TzBuf *b, const char *text, size_t length);
void tz_buf_units(TzBuf *b, const uint16_t *units, size_t length);
void tz_buf_str(TzBuf *b, TzStr s);
// Moves the contents into a new string and empties the buffer.
TzStr tz_buf_take(TzBuf *b);

//===----------------------------------------------------------------------===//
// Arrays (array.c)
//===----------------------------------------------------------------------===//

// An array: a reference-counted header with the elements stored unboxed,
// either right after the header or in a separate buffer once the array grew.
// `data` may point past the start of the storage after shift().
typedef struct TzArray {
  int64_t rc;
  const TzType *type; // of the elements
  char *data;         // the first element
  uint32_t length;
  uint32_t capacity;  // elements that fit from `data` on
  char *alloc;        // the separate buffer, or NULL for inline elements
} TzArray;

int64_t tz_arr_live_objects(void);
TzArray *tz_arr_new(const TzType *type, int64_t length, int64_t capacity);
// Appends the value at `value`, which the array takes over.
void tz_arr_push_value(TzArray *a, const void *value);
void tz_value_retain(const TzType *type, void *value);
void tz_value_release(const TzType *type, void *value);
int tz_is_managed(const TzType *type);
// JavaScript's String(value), appended to `out`. Inside join(), undefined
// elements become empty instead.
void tz_buf_value(TzBuf *out, const TzType *type, const void *value, int in_join);
// Appends the elements of `array` joined by `sep` (NULL for ",").
void tz_buf_join(TzBuf *out, const TzArray *array, const TzBuf *sep);

#endif
