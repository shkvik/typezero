// What every module of the runtime shares: the marks of its functions (TZ_HOT and the
// like), the descriptors of types, and the layouts of objects. Each module declares the
// rest in its own header. Programs call the runtime through the functions that
// compiler/lower/runtime_functions/ declares; keep both in sync.

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
// them there (see compiler/optimize/runtime_linker/runtime_linker.cpp); every other
// call goes to tzrt.lib. Two rules follow:
//
//   - State is never `static`: a copied function must reach the one variable
//     in tzrt.lib, not a copy of its own. RuntimeLinker rejects static state.
//   - A TZ_HOT function stays small: it calls its slow path as a separate
//     TZ_NOINLINE function that is not `static`, which stays in tzrt.lib.
//     RuntimeLinker does not copy a function that would bring more than about
//     a hundred instructions along.
//
// TZ_COLD: rarely called; the optimizer keeps calls to it out of the hot path.
//
// The runtime is built twice (runtime/CMakeLists.txt): tzrt for programs
// that run on one thread, and with TZ_PARALLEL, tzrt_mt for those whose
// async code runs in parallel (docs/async.md, section 8; parallel.c), which
// tz links when the program has pieces that may
// (compiler/parallel/parallel_analysis/parallel_analysis.h). TZ_THREAD_LOCAL marks the state each
// thread of tzrt_mt has a copy of, as the error in flight; in tzrt it is an
// ordinary variable. Layouts are the same in both: tzcrypto.lib, built once,
// goes with either, and must not touch that state itself (TZ_NO_THREAD_STATE).
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

// Describes a type to the runtime; the compiler emits one constant per type
// it needs (Lowering::typeDescriptor). `size` is the size of a value in
// memory. `inner` is the element type of an array, or the value type of
// `T | undefined`, which is laid out as { T value; uint8_t has }, except
// for an object or a Json, whose `T | undefined` is the reference, NULL for
// undefined.
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
  TZ_T_DECIMAL = 11,
  TZ_T_RESULT = 12,
  TZ_T_JSON = 13,
  TZ_T_UNION = 14,
  TZ_T_ATOM = 15,
  TZ_T_UNKNOWN = 16, // a reference, NULL for undefined (see unknown.c)
  TZ_T_GETTER = 17,  // a key that a getter gives (TZ_FIELD_GETTER)
};

typedef struct TzType {
  uint32_t kind;
  uint32_t size;
  const struct TzType *inner;
} TzType;

// A Result of neverthrow (TZ_T_RESULT) is a value, not a reference: an Ok
// with a value of the type `base.inner`, or an Err with an error of the type
// `error`, and a byte that says which; the half not in use holds zero. The
// compiler places the three at the offsets given. A half of type void (as in
// `ok()`) is TZ_T_UNDEFINED of size 0.
typedef struct TzResultType {
  TzType base;
  const TzType *error;
  uint32_t value_offset, error_offset, ok_offset;
} TzResultType;

// A union whose members lie apart in memory (TZ_T_UNION, docs/objects.md
// section 7): an int32 tag, the number of the slot the value takes, then
// the value of that slot at offset 8. `slots` gives the type of each slot by
// its number (see UnionSlot in compiler/program/types/types.h); the tag of
// undefined, in `A | B | undefined`, is 0. All the objects of a union take one
// slot: an object's layout tells its type.
#define TZ_UNION_SLOTS 12
typedef struct TzUnionType {
  TzType base;
  const TzType *slots[TZ_UNION_SLOTS];
} TzUnionType;

// Whether a `T | undefined` at `value` holds a value. A union, and a string
// literal type (an int32, see TzAtomType), have zero for undefined.
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

// An object is allocated as the objects of closures are, but starts with
// { rc, layout }: a constant the compiler emits for each layout and each
// order of the keys (Lowering::layoutDescriptor). The fields follow, at the
// offsets the layout gives, each stored as a value of its type.
typedef struct TzField {
  const char *name; // UTF-8
  uint32_t name_length;
  uint32_t offset; // from the start of the object
  const TzType *type;
} TzField;

// A tuple, `[string, int]`, is an object with the fields "0", "1"... that
// prints as an array. An iterator is an object without fields followed by
// the frame of its coroutine (compiler/lower/codegen/codegen_iter.cpp); its
// flag says what it prints as. A regex prints as `/source/flags` (regex.c); a
// match as the array of its captures with its other fields after them; the
// named groups of a match, an object without a prototype in JavaScript, as
// `[Object: null prototype] { ... }`.
enum {
  TZ_LAYOUT_TUPLE = 1,
  TZ_LAYOUT_GENERATOR = 2,        // Object [Generator] {}
  TZ_LAYOUT_ARRAY_ITERATOR = 4,   // Object [Array Iterator] {}
  TZ_LAYOUT_STRING_ITERATOR = 8,  // Object [String Iterator] {}
  TZ_LAYOUT_ITERATOR_HELPER = 16, // Object [Iterator Helper] {}
  TZ_LAYOUT_REGEX = 32,
  TZ_LAYOUT_MATCH = 64,
  TZ_LAYOUT_GROUPS = 128,
  TZ_LAYOUT_REGEX_ITERATOR = 256, // Object [RegExp String Iterator] {}
  TZ_LAYOUT_ERROR = 512,          // an Error, which prints its stack (error.c)
  TZ_LAYOUT_DATE = 1024,          // its ISO text (date.c)
  TZ_LAYOUT_DATE_FORMAT = 2048,   // DateTimeFormat [Intl.DateTimeFormat] {}
  TZ_LAYOUT_DICT = 4096,          // a Record, Map or Set (TzMap, map.c)
  // The iterator of a Map or a Set, `[Map Iterator] { 'a', 'b' }`, which
  // shows what is left: the keys, or the values, or [key, value] entries.
  // Its coroutine keeps the map and the position in the promise, after the
  // fields every iterator has there (compiler/lower/codegen/codegen_iter.cpp).
  TZ_LAYOUT_DICT_ITERATOR = 8192,
  TZ_LAYOUT_DICT_VALUES = 16384,
  TZ_LAYOUT_DICT_ENTRIES = 32768,
  TZ_LAYOUT_DICT_OF_SET = 65536,
  TZ_LAYOUT_PROMISE = 131072, // Promise { value } (async.c)
  TZ_LAYOUT_TIMER = 262144,   // Timeout {} and Immediate {}, by the layout's name; so the
                              // objects of node:crypto, Hash {}
                              // (runtime/library/crypto/crypto/crypto.h)
  // An object of binary data (bytes.c): its TZ_BYTES_* kind is in the bits
  // from TZ_LAYOUT_BYTES_SHIFT on.
  TZ_LAYOUT_BYTES = 524288,
  TZ_LAYOUT_BOX = 1048576,     // the box of a value in an unknown (TzBox, unknown.c)
  TZ_LAYOUT_CLASS = 2097152,   // a class itself, as a value (TzClass, unknown.c)
  TZ_LAYOUT_BUILTIN = 4194304, // ...of the language, `[Function: String]`
  // An object of Intl (intl.h) that the runtime makes: `NumberFormat
  // [Intl.NumberFormat] {}` with the name of its layout, `{}` without one.
  TZ_LAYOUT_INTL = 8388608,
  // After the kind of binary data (bits 24 to 27).
  // An async iterator, whose layout is a TzAsyncIterLayout (async.c).
  TZ_LAYOUT_ASYNC_ITERATOR = 268435456,
  TZ_LAYOUT_ASYNC_GENERATOR = 536870912, // Object [AsyncGenerator] {}
  // A class of the built-in modules tz has written in tz (library/), which
  // prints as node prints its object (inspect.c): an emitter of node:events
  // and the classes down its chain show `_maxListeners: undefined`, and the
  // keys that node has as symbols, `Symbol(kCapture)`; the wrapper of
  // once() is node's bound onceWrapper, an AsyncListener or a ThisListener
  // the function it holds, and a ListenerList the array of its listeners.
  TZ_LAYOUT_LIBRARY = 1073741824,
};

// A dictionary whose maps are members of groups (group.c): tz_map_new makes
// them so. Bit 31, the last one free, which an enumerator of C cannot be.
#define TZ_LAYOUT_GROUP 0x80000000u

#define TZ_LAYOUT_BYTES_SHIFT 24

// The class number of an object of binary data of kind k (bytes.c) is
// TZ_BYTES_CLASS + k, which `instanceof Uint8Array` and the unions of
// objects compare; must match Lowering::kBytesIdBase.
#define TZ_BYTES_CLASS 0x3FFFFF00u

// The offset of a key that is a method (of an object literal, or an arrow in
// a field of a class): not stored in the object, it prints as
// `[Function: name]`.
#define TZ_FIELD_METHOD 0xFFFFFFFFu

// The offset of a key that a getter gives, a property of a class of tz's
// library (library/) such as `status` of Response: not stored in the object,
// it prints with the value its getter gives, as node prints the properties
// of its classes. It is no key otherwise (Object.keys, JSON.stringify), as
// a getter of a prototype is not one in JavaScript. Its type is a
// TzGetterType.
#define TZ_FIELD_GETTER 0xFFFFFFFEu

typedef struct TzLayout {
  // Releases what the fields hold, and frees the object.
  void (*drop)(void *object);
  const TzField *fields; // in the order of the keys, for printing and Object.keys
  uint32_t count;
  uint32_t flags; // TZ_LAYOUT_TUPLE
  // An instance of a class: the class's name (UTF-8), which it prints with,
  // and number, which `instanceof` compares. NULL and 0 for other objects.
  const char *name;
  uint32_t name_length;
  uint32_t class_id;
} TzLayout;

// A key of an object of TZ_LAYOUT_LIBRARY that is a symbol in node,
// `Symbol(kCapture)`: it prints, but Object.keys and JSON.stringify leave it
// out.
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

// What a key of TZ_FIELD_GETTER holds: the type of the value, and the
// getter, which writes it, owned, at `out` (of `result->size` bytes).
typedef struct TzGetterType {
  TzType base; // TZ_T_GETTER
  const TzType *result;
  void (*get)(const void *object, void *out);
} TzGetterType;

// A function of the program that makes an object of the language from parts
// the runtime gives it (Lowering::objectMaker): the parts are a struct of the
// object's fields, in an order both sides know, which the object takes over.
// The structs below are those parts; the compiler lays out the same fields
// in the same order (see makerFields in
// compiler/library/node/codegen/codegen_node.cpp).
typedef void *(*TzMaker)(const void *parts);

#endif
