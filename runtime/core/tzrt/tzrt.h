// Declarations shared between the files of the runtime. Programs call the
// runtime through the functions declared in compiler/lower/codegen/codegen.cpp
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
// them there (see linkRuntime in compiler/cli/driver/driver.cpp); every other
// call goes to tzrt.lib. Two rules follow:
//
//   - State is never `static`: a copied function must reach the one variable
//     in tzrt.lib, not a copy of its own. The driver rejects static state.
//   - A TZ_HOT function stays small: it calls its slow path as a separate
//     TZ_NOINLINE function that is not `static`, which stays in tzrt.lib.
//     The driver does not copy a function that would bring more than about
//     a hundred instructions along.
//
// TZ_COLD: rarely called; the optimizer keeps calls to it out of the hot path.
//
// The runtime is built twice (runtime/CMakeLists.txt): tzrt for programs
// that run on one thread, and with TZ_PARALLEL, tzrt_mt for those whose
// async code runs in parallel (docs/async.md, section 8; parallel.c), which
// tz links when the program has pieces that may
// (compiler/check/parallel/parallel.h). TZ_THREAD_LOCAL marks the state each
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

// Buffered stdout.
void tz_out_write(const char *data, size_t len);
// Bytes as they are to stdout (fd 1) or stderr (2), in order with
// console.log: writeFileSync(1, bytes).
void tz_std_write_bytes(int32_t fd, const uint8_t *data, size_t len);

// Formats a double like JavaScript's Number#toString; returns the length.
// `out` must hold at least 32 bytes.
size_t tz_format_f64(double value, char *out);
// The shortest digits of a finite positive double (number.c), at most 17;
// returns their count, and the double is those digits times 10^*exponent.
int tz_f64_digits(double value, char *digits, int32_t *exponent);

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
      type->inner->kind == TZ_T_UNKNOWN)
    return *(void *const *)value != NULL;
  if (type->inner->kind == TZ_T_UNION || type->inner->kind == TZ_T_ATOM)
    return *(const int32_t *)value != 0;
  return ((const uint8_t *)value)[type->inner->size] != 0;
}

//===----------------------------------------------------------------------===//
// Functions and the objects of closures (closure.c)
//===----------------------------------------------------------------------===//

// The environments of closures and the boxes of the variables they share
// start with this header. The compiler emits a `drop` function for each kind
// of them, which releases what the object holds and frees it, and a constant
// descriptor that holds it. An object of the language (TzInstance) starts the
// same way, its layout with its drop function, so that the environment of a
// function value may be one: a method bound to its object.
typedef struct TzObjectDesc {
  void (*drop)(void *object);
} TzObjectDesc;

typedef struct TzObject {
  int64_t rc;
  const TzObjectDesc *desc;
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

//===----------------------------------------------------------------------===//
// Groups (group.c): cycles freed without a collector
//===----------------------------------------------------------------------===//

// Objects of the types the compiler finds may form cycles (docs/objects.md,
// section 11) are members of groups: an object of the language, an array, a
// dictionary, the environment of a closure or a box of a shared variable,
// each with a link of 16 bytes in front of it. A group has one count, of the
// references from outside it; those between its members are not counted.
// Storing a reference to a member into a member of another group merges the
// two groups. When nothing outside holds a member, the whole group is freed.
//
// A member's count is negative, which tells it from any other object: the
// count of a group lives in one member, its leader, and the others hold
// TZ_RC_MEMBER. The leader's count is TZ_RC_GROUP plus the references from
// outside; for an eternal group (one a global holds, which is never
// assigned again), TZ_RC_ETERNAL plus them: references to it are not counted
// and it merges with nothing. A group being freed has TZ_RC_DYING.
#define TZ_RC_MEMBER INT64_MIN
#define TZ_RC_DYING (-(INT64_C(1) << 62) - (INT64_C(1) << 60))
#define TZ_RC_GROUP (-(INT64_C(1) << 62))
#define TZ_RC_ETERNAL (-(INT64_C(1) << 61))

// Shared objects (docs/async.md, section 8). In the parallel runtime an
// object that other threads may reach, as soon as something they can reach
// holds it (tz_publish, parallel.c), is counted with atomic operations: its
// count is TZ_RC_SHARED plus its references, below zero, so that the fast
// paths, which count the others, leave it to the slow ones. The counts of
// groups are all below TZ_RC_SHARED: a count below it is a member's. A
// count from TZ_RC_FOREVER up is of an object never freed (on the stack, or
// one of the runtime's constants), which is far from zero whatever happens.
#define TZ_RC_SHARED (-(INT64_C(1) << 60))
#define TZ_RC_FOREVER (INT64_C(1) << 61)

static inline int tz_rc_is_member(int64_t rc) { return rc < TZ_RC_SHARED; }
static inline int tz_rc_is_shared(int64_t rc) { return rc < 0 && rc >= TZ_RC_SHARED; }

// One reference more, and one less, to an object that is never a member of
// a group (a string, a Json, a decimal...): tz_rc_dec says whether it was the
// last, and the caller frees the object. Plain counts, but for a shared
// object (only in the parallel runtime), whose count is negative and goes
// atomically: inline in tzrt_mt, through the functions below elsewhere
// (tzcrypto.lib is built once for both runtimes).
void tz_rc_shared_add(int64_t *rc, int64_t n);
int tz_rc_shared_dec(int64_t *rc);

static inline void tz_rc_add(int64_t *rc, int64_t n) {
  if (*rc < 0) {
#if TZ_PARALLEL && !defined(TZ_NO_THREAD_STATE)
    __atomic_fetch_add(rc, n, __ATOMIC_RELAXED);
#else
    tz_rc_shared_add(rc, n);
#endif
    return;
  }
  *rc += n;
}

static inline void tz_rc_inc(int64_t *rc) { tz_rc_add(rc, 1); }

static inline int tz_rc_dec(int64_t *rc) {
  if (*rc < 0) {
#if TZ_PARALLEL && !defined(TZ_NO_THREAD_STATE)
    if (__atomic_sub_fetch(rc, 1, __ATOMIC_ACQ_REL) != TZ_RC_SHARED)
      return 0;
    *rc = 0; // the last reference: the object is this thread's alone
    return 1;
#else
    return tz_rc_shared_dec(rc);
#endif
  }
  return --*rc == 0;
}

typedef struct TzGroupLink {
  void *leader;   // the member that holds the count; itself for the leader
  uintptr_t next; // the next member, round the group; TZ_GROUP_ARRAY tags an array
} TzGroupLink;

#define TZ_GROUP_ARRAY ((uintptr_t)1)

static inline TzGroupLink *tz_group_link(const void *member) {
  return (TzGroupLink *)((char *)member - sizeof(TzGroupLink));
}

// A new member of a group of its own, of `size` bytes, with a count of 1;
// the caller stores its descriptor and fields. Arrays come from
// tz_arr_new_group.
void *tz_group_alloc(int64_t size);
// The memory of a member with the TZ_GROUP_ARRAY tag or none, for the
// allocations above; not counted for the leak check.
void *tz_group_alloc_tagged(size_t size, uintptr_t tag);
// One more reference from outside to the group of `member`, or one less,
// which frees the group when it was the last. In the parallel runtime they
// also count a shared object (TZ_RC_SHARED), which has a descriptor that
// drops it: every object with a negative count comes here.
void tz_group_retain(void *member);
void tz_group_release(void *member);
// Frees the group of `leader`, whose count reached zero.
void tz_group_free(TzObject *leader);
// After `owner` took over a reference to `ref` into one of its fields
// (elements, entries...): if both are members, the reference is one inside
// a group now, and the groups merge if they were two.
void tz_group_store(void *owner, void *ref);
// `owner` lets go of its reference to the member `ref`.
void tz_group_unstore(void *owner, void *ref);
// The reference of `owner` to the member `ref` moves out of it to the code,
// which owns it then (pop, shift).
void tz_group_take(void *owner, void *ref);
// The end of the drop function of an object that may be a member: frees it,
// unless it is a member, which its group frees with the others.
void tz_group_obj_free(void *object);
// Publishing (publish.c): the group of `member` becomes published, and
// `each` gets every member of it, and whether it is an array; nothing if it
// was already.
void tz_group_publish(void *member, void (*each)(void *member, int32_t array));
int32_t tz_group_published(const void *member);
// The same for values of `type` at `value`, for every reference in them.
void tz_group_store_value(void *owner, const TzType *type, void *value);
void tz_group_take_value(void *owner, const TzType *type, void *value);
// Releases the value of `type` at `value` that `owner` held.
void tz_value_release_in(void *owner, const TzType *type, void *value);
// The groups of the references in a value that a global holds for good, and
// that value at the end of a program checked for leaks (see tz_group_end).
void tz_group_pin_value(const TzType *type, void *value);
void tz_group_end_value(const TzType *type, void *value);
// Before the leak check: frees what eternal groups left, and reports the
// groups still alive with the types of their members.
void tz_group_shutdown(void);

// Debug mode (group.c), in a program compiled with TZ_CHECK_LEAKS set: the
// place of the store into a member being made (tz_group_site), the objects
// the compiler proved are never stored, and those an owner holds alone.
struct TzFrame;
typedef struct TzGroupSite {
  const struct TzFrame *at;
  const char *field; // what the store writes, UTF-8
  uint32_t field_length;
} TzGroupSite;
extern int32_t tz_group_debugging;
extern TZ_THREAD_LOCAL const TzGroupSite *tz_group_site;
void tz_group_note_unstored(const void *object, const struct TzFrame *made);
void tz_group_forget_unstored(const void *object);
void tz_group_check_owned(const void *object, const struct TzFrame *at, const char *owner,
                          uint32_t owner_length);

static inline void tz_ref_retain(void *object) {
  TzObject *o = (TzObject *)object;
  if (o->rc >= 0)
    ++o->rc;
  else
    tz_group_retain(o);
}

static inline void tz_obj_retain(TzObject *o) {
  if (o)
    tz_ref_retain(o);
}

static inline void tz_obj_release(TzObject *o) {
  if (!o)
    return;
  if (o->rc < 0)
    tz_group_release(o);
  else if (--o->rc == 0)
    o->desc->drop(o);
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

// A tuple, `[string, int]`, is an object with the fields "0", "1"... that
// prints as an array. An iterator is an object without fields followed by
// the frame of its coroutine (compiler/lower/codegen/codegen_iter.cpp); its
// flag says what it prints as. A regex prints as `/source/flags` (regex.c); a
// match as the array of its captures with its other fields after them; the
// named groups of a match, an object without a prototype in JavaScript, as
// `[Object: null prototype] { ... }`.
enum {
  TZ_LAYOUT_TUPLE = 1,
  TZ_LAYOUT_GENERATOR = 2,       // Object [Generator] {}
  TZ_LAYOUT_ARRAY_ITERATOR = 4,  // Object [Array Iterator] {}
  TZ_LAYOUT_STRING_ITERATOR = 8, // Object [String Iterator] {}
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
  TZ_LAYOUT_PROMISE = 131072,     // Promise { value } (async.c)
  TZ_LAYOUT_TIMER = 262144,       // Timeout {} and Immediate {}, by the layout's name; so the
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
// objects compare; must match CodeGen::kBytesIdBase.
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
  if (!(layout->flags & TZ_LAYOUT_LIBRARY) || field->name_length < sizeof prefix - 1)
    return 0;
  for (size_t i = 0; i < sizeof prefix - 1; ++i)
    if (field->name[i] != prefix[i])
      return 0;
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

static inline void tz_instance_release(TzInstance *o) {
  if (!o)
    return;
  if (o->rc < 0)
    tz_group_release(o);
  else if (--o->rc == 0)
    o->layout->drop(o);
}

// Weak references (group.c): a WeakRef is an object whose layout's drop
// function is tz_weak_drop; deref gives its object, counted, or NULL once
// that died. The drop function of an object that may have weak references
// forgets them first, when tz_weak_count says there are any.
extern int64_t tz_weak_count;
void *tz_weak_new(const TzLayout *layout, void *target);
void *tz_weak_deref(void *weak);
void tz_weak_forget(void *target);
void tz_weak_drop(void *object);

//===----------------------------------------------------------------------===//
// Strings (string.c)
//===----------------------------------------------------------------------===//

typedef struct {
  void *obj;
  uint64_t meta;
} TzStr;

// A string literal type, or a union of them (TZ_T_ATOM): the value is the
// number of the literal, an int32, and `atoms` the strings by number, the
// same table for all of them (index 0, undefined, is "undefined").
typedef struct TzAtomType {
  TzType base;
  const TzStr *atoms;
} TzAtomType;

extern const TzType tz_string_type, tz_undefined_type;

// What a value of a union or of a literal type holds, for the code that
// takes values of any type: the type and the address of the member it holds,
// or of the string of the literal. Any other value is itself.
static inline const TzType *tz_resolve(const TzType *type, const void **value) {
  if (type->kind == TZ_T_ATOM) {
    *value = &((const TzAtomType *)type)->atoms[*(const int32_t *)*value];
    return &tz_string_type;
  }
  if (type->kind == TZ_T_UNION) {
    uint32_t tag = *(const uint32_t *)*value;
    *value = (const char *)*value + 8;
    type = tag ? ((const TzUnionType *)type)->slots[tag] : &tz_undefined_type;
    // A string literal of the union: its string, as above.
    if (type->kind == TZ_T_ATOM) {
      *value = &((const TzAtomType *)type)->atoms[*(const int32_t *)*value];
      return &tz_string_type;
    }
  }
  return type;
}

int64_t tz_str_live_objects(void);
void tz_str_retain_value(TzStr s);
void tz_str_release_value(TzStr s);
// A new string from UTF-16 code units, stored one byte per unit if possible.
TzStr tz_str_from_utf16(const uint16_t *units, size_t length);
TzStr tz_str_from_ascii(const char *text, size_t length);
// A new string of `length` ASCII characters for the caller to write at the
// pointer returned; NULL for a short one, which tz_str_from_ascii makes.
char *tz_str_ascii_units(size_t length, TzStr *out);
int32_t tz_str_equal(void *a_obj, uint64_t a_meta, void *b_obj, uint64_t b_meta);
int32_t tz_str_compare(void *a_obj, uint64_t a_meta, void *b_obj, uint64_t b_meta);
// `items` joined by `sep`, or by "," when sep is NULL.
void tz_str_join(TzStr *out, const TzStr *items, uint32_t n, const TzStr *sep);
// s[from..to), owned; s is borrowed.
TzStr tz_str_sub(TzStr s, uint32_t from, uint32_t to);
// The code units of `*s`, which becomes its flat form (borrowed, as `*s`
// was): one byte each, or two with `*two`. A short string keeps them in
// `*s` itself, so `*s` must stay where it is while they are read.
const void *tz_str_units(TzStr *s, uint32_t *length, int *two);
// Writes UTF-16 to stdout as UTF-8; lone surrogates become U+FFFD.
void tz_out_write_utf16(const uint16_t *units, size_t length);
// The same for a string.
void tz_print_string(void *obj, uint64_t meta);
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
// The same for one-byte code units.
void tz_buf_bytes(TzBuf *b, const uint8_t *units, size_t length);
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
// The same as a member of a group of its own (group.c), for an array of a
// type that may form cycles.
TzArray *tz_arr_new_group(const TzType *type, int64_t length, int64_t capacity);
TzArray *tz_arr_new_length_group(const TzType *type, int64_t length);
// What the group of a member array frees first: its elements (those of the
// group stay) and its buffer, not the header.
void tz_arr_drop_member(TzArray *array);
// Frees an array whose count reached zero.
void tz_arr_free(TzArray *array);
// Appends the value at `value`, which the array takes over.
void tz_arr_push_value(TzArray *a, const void *value);
// Room for at least `min` elements.
void tz_arr_grow(TzArray *a, int64_t min);
void tz_value_retain(const TzType *type, void *value);
void tz_value_release(const TzType *type, void *value);
int tz_is_managed(const TzType *type);
// JavaScript's String(value), appended to `out`. Inside join(), undefined
// elements become empty instead.
void tz_buf_value(TzBuf *out, const TzType *type, const void *value, int in_join);
// Appends the elements of `array` joined by `sep` (NULL for ",").
void tz_buf_join(TzBuf *out, const TzArray *array, const TzBuf *sep);

//===----------------------------------------------------------------------===//
// Exact decimal numbers (decimal.c)
//===----------------------------------------------------------------------===//

// A decimal value is two words, as a string is. A number of up to 18 digits
// or so is `coef * 10^-scale` in the value itself, with the scale in the low
// 32 bits of `meta` and no flags; longer ones keep their digits in a TzDecObj
// that `coef` points to (see decimal.c). Must match CodeGen
// (compiler/library/decimal/codegen/codegen_decimal.cpp).
typedef struct {
  int64_t coef;
  uint64_t meta;
} TzDec;

#define TZ_DEC_HEAP (UINT64_C(1) << 63) // coef points to a counted TzDecObj
#define TZ_DEC_LONG (UINT64_C(1) << 62) // coef points to a TzDecObj

// (-1)^neg * d * 10^exp, where d is the integer whose base 10^9 digits are
// d[0] (the lowest) to d[n - 1].
typedef struct {
  int64_t rc;
  int64_t exp;
  int32_t neg;
  uint32_t n;
  uint32_t d[];
} TzDecObj;

int64_t tz_dec_live_objects(void);
void tz_dec_retain_value(TzDec v);
void tz_dec_release_value(TzDec v);
int32_t tz_dec_equal(int64_t a, uint64_t am, int64_t b, uint64_t bm);
// The text String(v) gives, appended to `out`.
void tz_buf_decimal(TzBuf *out, TzDec v);
void tz_dec_from_string(TzDec *out, void *obj, uint64_t meta);

//===----------------------------------------------------------------------===//
// Json values (json.c)
//===----------------------------------------------------------------------===//

// A value of the type Json (docs/modules.md, section 5) is a reference to a
// counted node; `Json | undefined` is that reference, NULL for undefined.
// null, true and false are nodes of the runtime that are never freed. A Json
// array keeps its elements in a TzArray of Json, which a program sees as
// `Json[]` once it checked Array.isArray. Must match CodeGen
// (compiler/library/json/codegen/codegen_json.cpp).
enum {
  TZ_JSON_NULL = 0,
  TZ_JSON_BOOL = 1,
  TZ_JSON_NUMBER = 2,
  TZ_JSON_STRING = 3,
  TZ_JSON_ARRAY = 4,
  TZ_JSON_OBJECT = 5,
};

typedef struct TzJson TzJson;

typedef struct {
  TzStr key; // flat
  TzJson *value;
} TzJsonEntry;

struct TzJson {
  int64_t rc;
  uint32_t kind;
  uint32_t flags; // see json.c
  union {
    int32_t boolean;
    // A number keeps the text it was read from when the double cannot give
    // its digits back (see json.c), for JSON.parse<T> into a decimal or long.
    struct {
      double value;
      TzStr text;
    } number;
    TzStr string;
    TzArray *array; // of Json
    struct {
      TzJsonEntry *entries; // in the order the keys were added
      uint32_t count, capacity;
      uint32_t *index; // positions + 1 by the hash of the key, or NULL
      uint32_t index_size;
    } object;
  } u;
};

// The descriptor of Json, the elements of the arrays of the runtime.
extern const TzType tz_json_type;
int64_t tz_json_live_objects(void);
// Releases what the runtime holds (the files embedJson read), before the
// leak check.
void tz_json_shutdown(void);
// Frees a node whose count reached zero, and everything that dies with it.
void tz_json_free(TzJson *j);
static inline void tz_json_release(TzJson *j) {
  if (j && tz_rc_dec(&j->rc))
    tz_json_free(j);
}
// The positions of the entries of an object in the order JavaScript gives
// its keys: those that are array indices first, by their value, then the
// others as they were added. NULL when that is the order they are in;
// otherwise a block to free with tz_free.
uint32_t *tz_json_key_order(const TzJson *object);
// `a === b` of two Json values, NULL for undefined.
int32_t tz_json_strict_equal(const TzJson *a, const TzJson *b);

//===----------------------------------------------------------------------===//
// Dictionaries (map.c)
//===----------------------------------------------------------------------===//

// A dictionary of the language: a Record, a Map or a Set (docs/objects.md,
// section 9). All three are one hash table that keeps its entries in an
// array in the order they were added, as V8's ordered hash tables and the
// dicts of CPython do: iterating walks the array, a deleted entry stays
// there as a hole until the array is compacted, and an index of positions,
// an open-addressing table, finds a key by its hash. A map of up to
// TZ_MAP_SMALL entries has no index; it compares the hashes of its entries
// one by one.
//
// An entry is TzMapEntry followed by the key and the value (none for a Set)
// at the offsets of the map's layout, which the compiler lays out; compiled
// code reads and writes entries, `count`, `used` and `iterating` directly.
// While an iteration is in progress (`iterating` > 0) entries do not move,
// so a position stays valid, and keys added meanwhile come after it, as in
// JavaScript. Must match CodeGen (compiler/lower/codegen/codegen_dict.cpp).
//
// A map starts as every object of the language does, with its count and its
// layout, whose drop function is tz_map_drop and whose flag TZ_LAYOUT_DICT
// tells printing and JSON.stringify what it is.
enum { TZ_MAP_RECORD = 0, TZ_MAP_MAP = 1, TZ_MAP_SET = 2 };

typedef struct TzMapLayout {
  TzLayout base;
  const TzType *key;
  const TzType *value; // NULL for a Set
  uint32_t kind;       // TZ_MAP_RECORD...
  uint32_t entry_size, key_offset, value_offset;
} TzMapLayout;

#define TZ_MAP_SMALL 8

enum {
  TZ_MAP_ENV = 1,  // process.env: changes go to the environment of the process
  TZ_MAP_FOLD = 2, // the keys ignore case, as names of variables on Windows do
  TZ_MAP_BARE = 4, // a Record of Object.groupBy, an object without a prototype in JavaScript
};

typedef struct TzMap {
  int64_t rc;
  const TzMapLayout *layout;
  char *entries;           // room for `capacity`, `used` of them in use
  uint32_t *index;         // positions + 1 by hash (0 for none), or NULL
  uint32_t count;          // live entries: the size
  uint32_t used;           // live and deleted entries, in the order added
  uint32_t capacity;
  uint32_t index_mask;     // the index has index_mask + 1 slots
  uint32_t iterating;      // iterations in progress
  uint32_t flags;          // TZ_MAP_ENV...
  struct TzMap *next_dead; // see tz_map_drop
} TzMap;

typedef struct {
  uint32_t hash;
  uint32_t live; // 0 for a deleted entry
} TzMapEntry;

TzMap *tz_map_new(const TzMapLayout *layout);
void tz_map_drop(void *map);
// The entry of `key` (borrowed), or NULL.
void *tz_map_find(TzMap *m, const void *key);
// The entry of `key`; if the map lacks it, a new one with a copy of the key
// (retained) and a value of zero, and `*added` is set.
void *tz_map_add(TzMap *m, const void *key, int32_t *added);
int32_t tz_map_delete(TzMap *m, const void *key);
// The next live entry from position `*at` on, or NULL; `*at` moves past it.
void *tz_map_next(const TzMap *m, uint32_t *at);
// process.env (process.c): the variable of a live entry changed, or one was
// removed.
void tz_process_env_changed(const TzStr *name, const TzStr *value);

//===----------------------------------------------------------------------===//
// unknown, and classes as values (unknown.c)
//===----------------------------------------------------------------------===//

// A value of the type unknown (docs/decorators.md, section 4) is a reference,
// NULL for undefined: an object of the language as it is, any other value in
// a box. The box is an object of the language too, with tz_box_layout, so
// that code that sees it as an `object` releases and prints it as one. The
// value follows the header, as a value of `type` lies in memory, and the box
// owns it. Must match CodeGen (compiler/lower/codegen/codegen_unknown.cpp).
typedef struct TzBox {
  int64_t rc;
  const TzLayout *layout; // &tz_box_layout
  const TzType *type;     // of the value
} TzBox;

#define TZ_BOX_VALUE(box) ((char *)(box) + sizeof(TzBox))

extern const TzLayout tz_box_layout;

// What `typeof` says of an unknown, in the order of the strings CodeGen
// gives them: "undefined", "number", "string", "boolean", "object",
// "function".
enum {
  TZ_KIND_UNDEFINED = 0,
  TZ_KIND_NUMBER = 1,
  TZ_KIND_STRING = 2,
  TZ_KIND_BOOLEAN = 3,
  TZ_KIND_OBJECT = 4,
  TZ_KIND_FUNCTION = 5,
};

// A class as a value: an object that lives as long as the program, which the
// compiler emits for each class code uses as one. Its layout has
// TZ_LAYOUT_CLASS and the class's name; `parent` is the class it extends.
typedef struct TzClass {
  int64_t rc;
  const TzLayout *layout;
  const struct TzClass *parent;
  uint32_t id, last_id; // its number, and the last of the classes down its chain
  // What reflection knows of it (reflect.c), NULL if reflection does not
  // reach it. The static fields follow, when it is used as a value.
  const struct TzClassInfo *info;
} TzClass;

// The drop function of objects that are never freed (classes).
void tz_static_drop(void *object);
// A value of `type` at `value` as an unknown, which takes it over.
void *tz_unknown_of(const TzType *type, const void *value);
// A new box of a value of `type` at `value`, which it takes over.
void *tz_unknown_box(const TzType *type, const void *value);
// The same as a member of a group of its own, for a value that may hold one.
void *tz_unknown_box_member(const TzType *type, const void *value);
int32_t tz_unknown_kind(const void *u);
// What a check showed the unknown `u` (borrowed) holds: a number (as a
// double), a string (owned), a boolean, an array.
double tz_unknown_number(const void *u);
void tz_unknown_string(TzStr *out, const void *u);
int32_t tz_unknown_bool(const void *u);
int32_t tz_unknown_is_array(const void *u);
// The array an unknown holds as an `unknown[]` (described by `array_type`):
// itself if its elements are unknowns, else a new array of them, which the
// unknown holds from then on.
void *tz_unknown_array(const void *u, const TzType *array_type);
// `a === b` of two unknowns.
int32_t tz_unknown_equal(const void *a, const void *b);
// The value of `type` in the unknown `u`, which holds one, copied to `out`
// and retained.
void tz_unknown_unbox(const void *u, const TzType *type, void *out);

// The built-in modules written in tz (library.c, library/): what their
// intrinsics `__tz_*` do (see compiler/library/events/sema/sema_events.cpp).
void *tz_lib_bare(void *map, void *(*make)(const void *));
void tz_lib_class_name(TzStr *out, const void *o, void *(*make)(const void *));
int32_t tz_lib_pid(void *(*make)(const void *));
// util.inspect of the unknown `u` (borrowed), a string quoted (inspect.c).
void tz_lib_inspect(TzStr *out, const void *u, void *(*make)(const void *));
// `__tz_chunk<T>(u)` of an unknown that holds no T (stream.c): ends the
// program.
TZ_COLD TZ_NORETURN void tz_lib_bad_chunk(const void *u, const char *type,
                                          uint32_t type_length);
// Whether the unknown `u` (borrowed) holds binary data of `kind` (TZ_BYTES_*).
int32_t tz_lib_is_bytes(const void *u, int32_t kind);

// Metadata (meta.c): records by key (an object key's address, or NULL and
// the string), target and property (has_property unset for none). Values
// are unknowns; define takes `value` over, get gives a new reference or NULL.
void tz_meta_init(TzClass *const *classes, uint32_t count);
// `compiler` is set for a record `design:*`, which the program cannot write.
void tz_meta_define(const void *key, void *key_obj, uint64_t key_meta, void *value, void *target,
                    void *property_obj, uint64_t property_meta, int32_t has_property,
                    int32_t compiler);
void *tz_meta_get(const void *key, void *key_obj, uint64_t key_meta, const void *target,
                  void *property_obj, uint64_t property_meta, int32_t has_property, int32_t own);
int32_t tz_meta_has(const void *key, void *key_obj, uint64_t key_meta, const void *target,
                    void *property_obj, uint64_t property_meta, int32_t has_property, int32_t own);
int32_t tz_meta_delete(const void *key, void *key_obj, uint64_t key_meta, const void *target,
                       void *property_obj, uint64_t property_meta, int32_t has_property);
// Releases what the records hold, before the leak check.
void tz_meta_shutdown(void);

// Reflection (reflect.c, docs/decorators.md section 6): what the compiler
// puts in the exe about the classes reflection reaches.
//
// A method as a value of Reflect.get: a function of `type`, `(this: unknown,
// ...params) => R`, whose code checks the `this` it gets.
typedef struct TzReflectMethod {
  const char *name; // UTF-8
  uint32_t name_length;
  const TzType *type;
  const TzFunctionInfo *info;
} TzReflectMethod;

typedef struct TzClassInfo {
  // `new` with `count` arguments as unknowns (borrowed), checked against a
  // constructor: the object, or NULL after throwing. NULL for a class that
  // cannot be made (an abstract one).
  void *(*construct)(void *const *args, uint32_t count);
  // fromJson(cls, j): the object made through the constructor, or NULL
  // after throwing; NULL if the program does not ask.
  void *(*from_json)(const TzJson *json, const void *path);
  // Its methods and those up its chain it does not override.
  const TzReflectMethod *methods;
  uint32_t method_count;
} TzClassInfo;

// What calls a function value of `type` with arguments as unknowns: the
// arguments (borrowed) are checked against its parameters, `self` goes to a
// `this` parameter, and the result comes back as an unknown (owned), or
// NULL after throwing.
typedef void *(*TzInvoker)(const TzFunction *f, void *self, void *const *args, uint32_t count);
typedef struct TzInvokerEntry {
  const TzType *type;
  TzInvoker invoke;
} TzInvokerEntry;

// The objects of the classes by number, and the invokers of the function
// types that Reflect.apply may meet; main gives them first.
void tz_reflect_init(TzClass *const *classes, uint32_t count, const TzInvokerEntry *invokers,
                     uint32_t invoker_count);
// `new cls(...)` of a class known only when the program runs, with the
// arguments as unknowns (borrowed); Reflect.construct(cls, args) with an
// array of any values (borrowed).
void *tz_reflect_new(const TzClass *cls, void *const *args, uint32_t count);
void *tz_reflect_construct(const TzClass *cls, const TzArray *args);
// Reflect.construct(C, args) of a class known when compiling: its function
// `construct` (as TzClassInfo has it) makes the object from the array of
// any values (borrowed), without the description of the class.
void *tz_reflect_make(void *(*construct)(void *const *args, uint32_t count),
                      const TzArray *args);
// Reflect.get(target, key): a field as an unknown, a method as a function,
// or undefined (NULL); a target that is no object is a TypeError.
void *tz_reflect_get(const void *target, void *key_obj, uint64_t key_meta);
// Reflect.apply(f, self, args), all borrowed.
void *tz_reflect_apply(const void *f, void *self, const TzArray *args);
// The name of a class, as `cls.name` gives it.
void tz_class_name(TzStr *out, const TzClass *cls);
// fromJson(cls, j) of a class known only when the program runs.
void *tz_reflect_from_json(const TzClass *cls, const TzJson *json);
// The TypeErrors of reflection: `what` (`new User`, `User.greet`) got the
// value `got` (borrowed) as its argument `index` (from 0), which is not a
// `expected`; or `count` arguments where it takes from `min` to `max` (-1:
// any number).
TZ_COLD void tz_reflect_mismatch(const char *what, uint32_t what_length, int32_t index,
                                 const char *expected, uint32_t expected_length, const void *got);
TZ_COLD void tz_reflect_arity(const char *what, uint32_t what_length, int32_t min, int32_t max,
                              uint32_t count);
// Whether the unknown `u` (borrowed) holds a value of a kind, and the value
// (owned) at `out`: a number of an int, of a long, any number, a boolean, a
// string, or a box of exactly `type` (any Json for a Json).
int32_t tz_unknown_to_int(const void *u, int32_t *out);
int32_t tz_unknown_to_long(const void *u, int64_t *out);
int32_t tz_unknown_to_double(const void *u, double *out);
int32_t tz_unknown_to_bool(const void *u, int32_t *out);
int32_t tz_unknown_to_string(const void *u, TzStr *out);
int32_t tz_unknown_to_box(const void *u, const TzType *type, void *out);
// The layout of the object of the language an unknown holds; NULL for
// undefined and a value in a box.
const TzLayout *tz_unknown_layout(const void *u);
// What tells the variants of a method apart when the program chooses one
// (docs/overloads.md, section 6): the kind (TZ_T_*) of the value an unknown
// holds in a box, 0 for undefined and an object of the language; whether it
// holds a box of exactly `type` (of a Json, any Json), as tz_unknown_to_box
// takes it.
uint32_t tz_unknown_box_kind(const void *u);
int32_t tz_unknown_holds(const void *u, const TzType *type);

//===----------------------------------------------------------------------===//
// Regular expressions (regex.c)
//===----------------------------------------------------------------------===//

// What looks for a match of a regex: in `units` (one byte or two each, by
// `wide`) at or after `start`, or only at `start` when `sticky`. Fills `caps`
// (two slots per capture of the program, -1 for undefined) and returns 1 if
// there is one. With `max` above 1 it goes on as the loop of a global regex
// does, each match from where the last one ended (past it, when it was
// empty), and fills the slots of up to `max` matches one after the other;
// it returns how many it found, fewer than `max` when there are no more. For
// a literal it is the machine code the compiler made of it
// (compiler/library/regex/codegen/codegen_regex_native.cpp); for `new
// RegExp(s)`, the interpreter, tz_re_interpret.
typedef int32_t (*TzReMatch)(const uint32_t *program, const void *units, int32_t wide,
                             int32_t length, int32_t start, int32_t *caps, int32_t sticky,
                             int32_t max);

// A regex: an object of the language whose one field the compiler reads and
// writes is lastIndex, right after the header. A literal without g and y is
// a constant the compiler emits, never freed; the others the runtime makes.
// Must match CodeGen (compiler/library/regex/codegen/codegen_regex.cpp).
typedef struct TzRegex {
  int64_t rc;
  const TzLayout *layout; // tz_re_layout
  int32_t last_index;
  uint32_t reserved;
  const uint32_t *program; // see regex.h
  uint32_t *owned;         // the program of `new RegExp`, freed with the regex
  TzReMatch match;
} TzRegex;

extern const TzLayout tz_re_layout;

// The named group of a match that a field of `groups` holds: the field's
// offset, and a capture with that name (with several, one entry each).
typedef struct {
  uint32_t offset;
  uint32_t capture;
  uint32_t optional; // the field is `string | undefined`
  uint32_t reserved;
} TzMatchName;

// How to build a match of a regex (RegExpMatchArray), which the compiler
// lays out as an object: the array of the captures, index, input, groups
// and, with the d flag, indices. The compiler emits one for each call that
// makes matches.
typedef struct {
  const TzLayout *layout;
  const TzType *item_type;       // `string | undefined`
  const TzLayout *groups_layout; // NULL: groups is undefined
  const TzMatchName *names;
  const TzLayout *pair_layout; // of the `[int, int]` in indices
  uint32_t size;
  uint32_t items, index, input, groups, indices; // offsets; indices is 0 without d
  uint32_t groups_size;
  uint32_t name_count;
  uint32_t pair_size;
  uint32_t reserved;
} TzMatchShape;

// The elements of the indices of a match (`[int, int] | undefined`), with
// their own descriptors: node shows those arrays with a `groups` property,
// which is undefined for a regex without named groups.
extern TzType tz_re_indices_type, tz_re_named_indices_type;

// Appends `/source/flags`.
void tz_buf_regex(TzBuf *out, const TzRegex *re);

// replace() and replaceAll() with a function: the runtime finds all the
// matches first, as JavaScript does, then the compiled code calls the
// function for each and hands back what it returned. Lives on the stack of
// the compiled code, which reserves TZ_REPLACER_SIZE bytes for it.
typedef struct {
  TzRegex *re;
  TzStr input; // flat, borrowed
  const void *units;
  uint32_t length;
  int32_t two;
  int32_t *matches; // the captures of each match, two slots per capture
  uint32_t count, capacity, next, captures, copied;
  TzStr *pieces; // of the result, joined at the end
  uint32_t piece_count, piece_capacity;
} TzReplacer;

#define TZ_REPLACER_SIZE 128

//===----------------------------------------------------------------------===//
// Errors (error.c)
//===----------------------------------------------------------------------===//

// The kinds of errors the runtime throws, numbered as kErrorKinds in
// compiler/program/ast/ast.h.
enum { TZ_ERROR = 0, TZ_TYPE_ERROR = 1, TZ_RANGE_ERROR = 2, TZ_SYNTAX_ERROR = 3, TZ_URI_ERROR = 4 };

// A place an error passed on its way up: a function, and where in which file.
// The compiler emits one constant for each call and throw that may throw
// (CodeGen::frameDescriptor).
typedef struct TzFrame {
  const char *function; // UTF-8; empty for the top-level code of a file
  uint32_t function_length;
  uint32_t line;
  const char *file; // UTF-8, the full path
  uint32_t file_length;
  uint32_t column;
} TzFrame;

// What the runtime knows of the class Error, which the program declares
// (CodeGen::emitErrorSupport passes it to tz_error_init first in main): how
// to make an Error of a kind with a message, and where its fields lie in it.
typedef struct TzErrorInfo {
  void *(*make)(int32_t kind, void *message_obj, uint64_t message_meta);
  const TzType *frames_type; // of the elements of `#frames`: long
  uint32_t name_offset, message_offset, cause_offset, frames_offset;
} TzErrorInfo;

// The error in flight: the Error that `throw` (or a failing part of the
// runtime) handed over, until a `catch` takes it. A function that throws
// leaves it here and returns; its caller checks it after the call. The code
// of the program runs only while it is NULL.
//
// A function that may recurse throws RangeError when the stack is below
// tz_stack_limit at its start.
//
// Both are the thread's own, in one variable with the rest of what each
// thread has (TzThread), so that a function of the program finds them from
// one address of the thread (CodeGen::thrownSlot). The first two fields
// must match CodeGen.

// The allocator's own memory of a thread (alloc.c): its pages with room for
// each size class, its segments, spares; and the blocks other threads freed
// that it has not taken back yet, linked by their first word.
#define TZ_HEAP_BINS 44
#define TZ_HEAP_HUGE_SPARES 4
typedef struct TzHeap {
  void *bins[TZ_HEAP_BINS];
  void *segments;
  void *spare;
  void *huge_spares[TZ_HEAP_HUGE_SPARES];
  void *remote; // through atomic operations
} TzHeap;

typedef struct TzThread {
  void *thrown;
  char *stack_limit;
  // What the thread made and freed: the counts of the leak check
  // (TZ_CHECK_LEAKS), summed over the threads (tzrt.c).
  int64_t live_objects, live_arrays, live_strings, live_decimals, live_json;
  // The parallel runtime (parallel.c): whether the thread holds the turn,
  // node's order, in which only one thread at a time goes on, the main one
  // but while it lends the turn to the piece of async code of another
  // thread; that piece, which this thread runs ahead of its turn; its
  // number, 0 for the main thread.
  int32_t turn;
  int32_t index;
  struct TzJob *job;
  // A thread of the pool, which never has the turn without a job.
  int32_t worker;
  // The locks of connections the thread holds (net.c): none may be held
  // while it waits for its turn.
  int32_t conn_locks;
  // The lock a function of the runtime took for the promise it gave, which
  // the await that follows lets go of once it waits (net.c, async.c).
  void *await_lock;
  // The microtasks of a piece that runs ahead of its event, in their order
  // (parallel.c): they go on here, or to the queue once it has the turn.
  struct TzReactor *local_head, *local_tail;
  // The caches of objects that the functions of a file keep for the next
  // calls (TZ_CACHE_*): each thread its own, as the formatters of ICU are
  // not to be used by two threads at once. NULL until the first use.
  void *caches[4];
  TzHeap heap;
} TzThread;

enum { TZ_CACHE_INTL = 0, TZ_CACHE_DATE_FORMAT = 1 };
#ifndef TZ_NO_THREAD_STATE
extern TZ_THREAD_LOCAL TzThread tz_thread;
#define tz_thrown (tz_thread.thrown)
#define tz_stack_limit (tz_thread.stack_limit)
#define tz_live_objects (tz_thread.live_objects)
#define tz_live_arrays (tz_thread.live_arrays)
#define tz_live_strings (tz_thread.live_strings)
#define tz_live_decimals (tz_thread.live_decimals)
#define tz_live_json (tz_thread.live_json)
#endif
// The sum of a count of TzThread (the int64_t at `offset`) over the threads
// of the program (parallel.c).
int64_t tz_thread_sum(size_t offset);
// Adds the thread that calls it to those tz_thread_sum counts.
void tz_thread_register(void);
// The threads of the program, by number: for the end of a program checked
// for leaks, when the others wait idle (parallel.c).
int32_t tz_thread_total(void);
TzThread *tz_thread_at(int32_t i);
// The number of the thread that calls it: 0 for the main one.
int32_t tz_thread_index(void);
// The pool of the parallel runtime, as TZ_THREADS asks (parallel.c).
void tz_parallel_init(void);
// Lets other threads run (parallel.c): for a thread that spins on a lock.
void tz_thread_yield(void);
static inline void tz_cpu_relax(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
  __builtin_ia32_pause();
#endif
}

// A lock that the thread that holds it may take again, for state of the
// runtime that threads share and that is rarely touched (the zone of the
// dates, what is made at the first use). Nothing in the runtime of one
// thread. Zero is a free lock.
typedef struct {
  int32_t owner; // tz_thread.index + 1 of the thread that holds it
  int32_t depth;
} TzLock;

#if TZ_PARALLEL && !defined(TZ_NO_THREAD_STATE)
static inline void tz_lock(TzLock *l) {
  int32_t me = tz_thread.index + 1;
  if (__atomic_load_n(&l->owner, __ATOMIC_RELAXED) == me) {
    ++l->depth;
    return;
  }
  for (unsigned spin = 0;; ++spin) {
    int32_t none = 0;
    if (__atomic_compare_exchange_n(&l->owner, &none, me, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      break;
    if (spin > 64)
      tz_thread_yield();
    else
      tz_cpu_relax();
  }
  l->depth = 1;
}

static inline void tz_unlock(TzLock *l) {
  if (--l->depth == 0)
    __atomic_store_n(&l->owner, 0, __ATOMIC_RELEASE);
}
#else
static inline void tz_lock(TzLock *l) { (void)l; }
static inline void tz_unlock(TzLock *l) { (void)l; }
#endif

// Shared objects (publish.c, docs/async.md, section 8): everything a value
// of `type` at `value` holds, or an object (with a descriptor or a layout)
// holds, and they, become shared, counted atomically from then on, before
// other threads may reach them. After a store of such a value into `owner`,
// tz_store_barrier publishes it when `owner` is shared. All of it does
// nothing in the runtime of one thread.
void tz_publish_value(const TzType *type, void *value);
void tz_publish_object(void *object);
void tz_store_barrier(void *owner, const TzType *type, void *value);
void tz_store_barrier_ref(void *owner, void *object);

// Node's order (parallel.c, docs/async.md, section 3). A thread of the pool
// runs a piece of async code ahead of its turn as long as the piece touches
// only what no other piece that may run at the same time writes; before the
// first thing that is not so, it waits for its turn: tz_order_enter(),
// which the program calls before such an access (CodeGen, from the decisions
// of compiler/check/parallel/parallel.h), and TZ_ORDERED(), which the functions
// of the runtime that touch its shared state (the queues, the promises, the
// timers, the output...) begin with. On the main thread, and once the thread
// has its turn, both cost a test.
#if TZ_PARALLEL && !defined(TZ_NO_THREAD_STATE)
TZ_NOINLINE void tz_order_wait(void);
static inline void tz_order_enter_inline(void) {
  if (!tz_thread.turn)
    tz_order_wait();
}
#define TZ_ORDERED() tz_order_enter_inline()
#else
#define TZ_ORDERED() ((void)0)
#endif
void tz_order_enter(void);
// The error in flight, which the caller takes over: NULL if there is none.
void *tz_error_take(void);
// Sets tz_stack_limit, and the room the handler of a stack overflow needs,
// for the thread that calls it (tzrt.c).
void tz_thread_stack_init(void);
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

//===----------------------------------------------------------------------===//
// Dates (date.c, date_zone.c, date_format.c)
//===----------------------------------------------------------------------===//

// A Date: an object of the language whose one field is its time value, in
// milliseconds since 1970 in UTC, NaN for an invalid date. The compiler makes
// these objects and reads and writes the field. Must match CodeGen
// (compiler/library/date/codegen/codegen_date.cpp).
typedef struct TzDate {
  int64_t rc;
  const TzLayout *layout; // with TZ_LAYOUT_DATE
  double time;
} TzDate;

// How console.log shows a date.
void tz_buf_date(TzBuf *out, double t);
// toJSON(), as JSON.stringify writes a date: the ISO text, or 0 for an
// invalid date (null).
int32_t tz_date_json(TzStr *out, double t);
// toString() and the like, in local time: `mode` is TZ_DATE_TEXT_* of date.h,
// 0 for toString(), which join() writes.
void tz_date_local_string(TzStr *out, double t, int32_t mode);

// The local zone (date_zone.c): the offset of local time from UTC at the
// moment t, in milliseconds; the moment of a local time; the zone's name at
// t, as toString() writes it.
double tz_date_offset(double t);
double tz_date_local_to_utc(double local);
void tz_date_zone_name(TzBuf *out, double t);
// The same day and time in an equivalent year of 2008 to 2035 (date.c).
double tz_date_equivalent_time(double t);

//===----------------------------------------------------------------------===//
// Promises, microtasks, timers and the event loop (async.c)
//===----------------------------------------------------------------------===//

// Must match CodeGen (compiler/lower/codegen/codegen_async.cpp).
enum { TZ_PROMISE_PENDING = 0, TZ_PROMISE_FULFILLED = 1, TZ_PROMISE_REJECTED = 2 };

typedef struct TzPromise TzPromise;
typedef struct TzReactor TzReactor;

// Something that waits for its turn in the microtask queue: a reaction to a
// promise, or a job. `run` gets the promise that settled (NULL for a job);
// after it returns, the runtime releases that promise, which the reactor
// held while queued, and `owner`, the object the reactor lives in. A reactor
// is in one list at a time, linked by `next`.
//
// `ahead`: when the piece of async code it goes on with may run ahead of
// its turn on a thread of the parallel runtime, the number of the await it
// goes on from (CodeGen sets it at each await, from the decisions of
// compiler/check/parallel/parallel.h), 0 when not; once a thread took it on,
// the TzJob it runs as (parallel.c). Every other reactor has 0.
struct TzReactor {
  TzReactor *next;
  void (*run)(TzReactor *self, TzPromise *source);
  TzObject *owner;
  TzPromise *source;
  uintptr_t ahead;
};

// A promise: an object whose layout has TZ_LAYOUT_PROMISE and, unless it is
// a Promise<void>, one field, its value at TZ_PROMISE_VALUE, which gives the
// type of the value. The reactions wait in a list until it settles. The
// promise of an async function is followed by the reactor its coroutine
// awaits with and then by the frame of the coroutine.
struct TzPromise {
  int64_t rc;
  const TzLayout *layout;
  TzReactor *head, *tail;
  void *error; // the Error, once rejected
  uint32_t state;
  uint32_t flags;
};

#define TZ_PROMISE_VALUE 48

// Promise.all, allSettled, race and any (tz_promise_combine): the promise
// it gives, and where the values go: an array of `items`, or the tuple
// `tuple` (of `tuple_size` bytes) whose fields are the values in order. For
// allSettled, the results `{ status, value?, reason? }` that the array holds,
// with the type of `value` (`T | undefined`, or NULL for a Promise<void>).
enum { TZ_COMBINE_ALL = 0, TZ_COMBINE_ALL_SETTLED = 1, TZ_COMBINE_RACE = 2, TZ_COMBINE_ANY = 3 };

typedef struct TzCombine {
  const TzLayout *result;
  const TzType *items;
  const TzLayout *tuple;
  uint32_t tuple_size;
  uint32_t settled_size;
  const TzLayout *settled;
  const TzType *value_type;
  uint32_t status_offset, value_offset, reason_offset;
  // A tuple of promises whose values differ: race and any copy the value of
  // promise i into the union they give with convert[i] (NULL: as it is);
  // allSettled makes the result of promise i by each[i] (its settled_*,
  // value_type and offsets), into a tuple of them.
  void (*const *convert)(void *dst, const void *src);
  const struct TzCombine *each;
} TzCombine;

// setTimeout, setInterval and setImmediate: an object the program may keep,
// to clear it. A list of timers (or the queue of immediates) holds it while
// it waits.
typedef struct TzTimer {
  int64_t rc;
  const TzLayout *layout;
  TzFunction callback;
  struct TzTimer *prev, *next;
  struct TimerList *list;
  int64_t idle_start;
  double idle_timeout; // the delay; -1 once cleared
  double repeat;       // the delay of setInterval, else 0
  uint8_t refed, destroyed, immediate, cleared;
} TzTimer;

void tz_microtask(TzReactor *r);
void tz_run_microtasks(void);
// Its reactions to the microtask queue, as a promise that settles queues
// them (for io.c, which held them back).
void tz_promise_trigger(TzPromise *p);
// Wakes the poll phase of the loop from another thread (io.c).
void tz_io_wake_loop(void);
// The loop waits for `n` more things of the runtime's own (io.c).
void tz_work_ref_count(int32_t n);
void tz_promise_drop(void *promise);
void tz_promise_fulfill(TzPromise *p);
void tz_promise_reject(TzPromise *p, void *error);
TzPromise *tz_promise_new(const TzLayout *layout);
// `await p` (null: of a value that is no promise) in an async function: its
// reactor `r` is queued once p settles; `ahead`, the number of the await
// when the piece after it may run ahead of its turn (see TzReactor).
void tz_async_await(TzReactor *r, TzPromise *p, int32_t ahead);
// The nextTick callbacks and the microtasks, after a callback of the loop.
void tz_after_callback(void);
// A Promise<void> of the runtime's own.
extern const TzLayout tz_promise_void_layout;

// A promise of the values of an async iterator (for await, docs/iterators.md)
// fulfilled with this flag holds none: the iterator is done.
#define TZ_PROMISE_DONE 8

// An async iterator (docs/iterators.md, step 4): an object whose layout,
// flagged TZ_LAYOUT_ASYNC_ITERATOR, says how it goes on with what `for
// await` asks of it. next(it, p) settles p, a promise of the type of the
// values, with the next one, or fulfills it with TZ_PROMISE_DONE;
// close(it, p) is return(), which settles p, a Promise<void>, once the
// iterator is closed.
typedef struct TzAsyncIterLayout {
  TzLayout base;
  void (*next)(void *it, TzPromise *p);
  void (*close)(void *it, TzPromise *p);
} TzAsyncIterLayout;

// An async generator: the steps asked of it, its state, and the reactor its
// coroutine awaits with, which its frame follows. Must match CodeGen
// (compiler/lower/codegen/codegen_async_iter.cpp).
typedef struct TzAgenStep {
  struct TzAgenStep *next;
  TzPromise *promise; // held
  int32_t close;      // return() rather than next()
} TzAgenStep;

typedef struct TzAsyncGen {
  int64_t rc;
  const TzLayout *layout; // a TzAsyncIterLayout
  TzAgenStep *head, *tail;
  int32_t state;
  int32_t reserved;
  TzReactor reactor;
} TzAsyncGen;

//===----------------------------------------------------------------------===//
// Waiting for I/O: the thread pool and the poll phase of the loop (io.c)
//===----------------------------------------------------------------------===//

// What the runtime gives the thread pool. `work` runs on a thread of the
// pool: calls of the system only, into memory of its own (malloc), never the
// allocator of the runtime or an object of the program, which are not
// thread-safe; NULL for something that only waits for its turn of the loop.
// `done` runs on the main thread in the poll phase, as a callback of Node
// does: it settles what the work is for and frees it. `turns` is how many
// turns of the loop Node takes for the same thing at the least (its round
// trips to the pool, 1 for 0), which the work may set too; `done` runs no
// earlier than that many polls after the work was queued. A work that is
// `unref` does not keep the loop alive.
//
// `settles`: the promise that `done` settles, when settling it is all that
// `done` does that the program can see (tz_work_queue_settles): the parallel
// runtime may then run what waits for it ahead of the event (parallel.c).
typedef struct TzWork {
  struct TzWork *next;
  void (*work)(struct TzWork *self);
  void (*done)(struct TzWork *self);
  uint32_t turns;
  uint32_t unref;
  uint64_t queued, due; // of the poll phases
  struct TzPromise *settles;
} TzWork;

void tz_work_queue(TzWork *w);
void tz_work_queue_settles(TzWork *w, struct TzPromise *p);
// Waits of the runtime's own, outside the pool (net.c): a work that began
// waiting keeps the loop alive (unless unref) until it is due; once what it
// waited for came, tz_io_due hands it to the poll phase, which runs `done`.
// On Windows the system posts it to the completion port of the loop, which
// tz_io_port_handle gives, made if need be.
void tz_io_begin(TzWork *w);
void tz_io_due(TzWork *w);
void *tz_io_port_handle(void);
// The work stops keeping the loop alive (it may still be done), or starts
// again.
void tz_work_unref(TzWork *w);
void tz_work_ref(TzWork *w);
// For the loop (async.c): whether queued work keeps it alive, and the poll
// phase, which waits up to `timeout` ms for the pool (-1 without a limit).
int tz_io_alive(void);
void tz_io_poll(int64_t timeout);

// Timers of the runtime's own (async.c).
TzTimer *tz_timer_new(const TzFunctionInfo *info, TzObject *env, double after, int32_t repeat);
void tz_timer_ref(TzTimer *t, int32_t ref);
void tz_timer_clear(TzTimer *t);
TzTimer *tz_immediate_new(const TzFunctionInfo *info, TzObject *env);
void tz_immediate_clear(TzTimer *t);

//===----------------------------------------------------------------------===//
// The runtime's side of an AbortSignal (signal.c)
//===----------------------------------------------------------------------===//

// A listener of the runtime's own to the abort of a signal: what waits and
// may be cancelled. `run` gets it once, unlinked already. `order` places it
// among the listeners of the program (see signal.c).
typedef struct TzAbortListener {
  struct TzAbortListener *next, *prev;
  struct TzSignal *signal; // the one it listens to, or NULL
  int64_t order;
  void (*run)(struct TzAbortListener *self);
} TzAbortListener;

// What an AbortSignal of tz's library (library/events/web.tz) holds for the
// runtime: whether it is aborted, the reason, and the runtime's listeners.
typedef struct TzSignal {
  int64_t rc;
  const TzLayout *layout;
  uint8_t aborted;
  void *reason; // an unknown, NULL before the abort or for undefined
  TzAbortListener *head, *tail;
} TzSignal;

int32_t tz_signal_aborted(const void *signal);
void tz_signal_listen(TzSignal *s, TzAbortListener *l);
void tz_signal_unlisten(TzAbortListener *l);
// A new AbortError whose cause is the reason of the signal, as Node rejects
// what a signal cancelled with.
void *tz_abort_error(const void *signal);
// The reason itself, owned, as fetch rejects with it: an Error, or the
// TypeError that throwing what it is gives.
void *tz_signal_reason(const void *signal);

//===----------------------------------------------------------------------===//
// Binary data (bytes.c, buffer.c)
//===----------------------------------------------------------------------===//

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
// CodeGen (compiler/library/bytes/codegen/codegen_bytes.cpp).
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
// from here (see CodeGen::bytesData).
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
// A new Buffer with a copy of `data`: readFileSync without an encoding.
TzBytes *tz_buffer_copy_of(const uint8_t *data, int64_t length);
// Buffer.from(text, encoding), and the text that writeFileSync writes with
// an encoding other than utf8 (the encodings numbered as in buffer.c).
TzBytes *tz_buffer_from_string(void *obj, uint64_t meta, int32_t encoding);
// The text of `length` bytes in `encoding`: readFileSync with an encoding
// other than utf8.
TzStr tz_buffer_decode(const uint8_t *data, int64_t length, int32_t encoding);
// The bytes of a view (a typed array, a Buffer or a DataView), and how many:
// what writeFileSync writes of it.
static inline int64_t tz_bytes_byte_length(const TzBytes *b) {
  return (int64_t)b->length * tz_bytes_element_size(tz_bytes_kind(b));
}
// buf.toString(encoding, start?, end?), `has` saying which positions were
// given (1 start, 2 end).
void tz_buffer_to_string(TzStr *out, const TzBytes *b, int32_t encoding, int64_t start,
                         int64_t end, int32_t has);
// Appends the text of a typed array, as join() gives it.
void tz_buf_bytes_join(TzBuf *out, const TzBytes *b, const TzBuf *sep);
// Appends `<Buffer 68 65>`, and `ArrayBuffer`'s `<00 01>`: the hex of the
// first `max` bytes, and how many more there are.
void tz_buf_bytes_hex(TzBuf *out, const uint8_t *data, int64_t length, int64_t max,
                      const char *more);
// A multi-byte encoding of TextDecoder (cjk.c): which decoder of the WHATWG
// Encoding Standard, and the indexes it reads, each the code points of the
// pointers from `first` on (0 for none). A Big5 code point is U+2xxxx where
// its bit in `astral` is set. EUC-JP has jis0212 as its second index; gb18030
// (and gbk) has its ranges, pairs of a pointer and a code point. `decode` is
// tz_cjk_decode, which a program gets only with an encoding it names.
enum {
  TZ_CJK_KIND_GB18030,
  TZ_CJK_KIND_BIG5,
  TZ_CJK_KIND_EUC_JP,
  TZ_CJK_KIND_ISO_2022_JP,
  TZ_CJK_KIND_SHIFT_JIS,
  TZ_CJK_KIND_EUC_KR,
};
typedef struct TzCjkEncoding {
  const char *name;
  int32_t kind;
  uint32_t first, count;
  const uint16_t *table;
  const uint8_t *astral;
  uint32_t first2, count2;
  const uint16_t *table2;
  const uint32_t *ranges;
  uint32_t range_count;
  int32_t (*decode)(const struct TzCjkEncoding *encoding, uint32_t *state, TzBuf *out,
                    const uint8_t *data, int64_t length, int32_t stream, int32_t fatal);
} TzCjkEncoding;
// Appends the text of `length` bytes to `out`, going on from `state` (four
// words, zero at the start of a stream) and leaving it there; unless
// `stream`, the bytes end the stream. Returns 0 at the first error when
// `fatal`, 1 otherwise.
int32_t tz_cjk_decode(const TzCjkEncoding *encoding, uint32_t *state, TzBuf *out,
                      const uint8_t *data, int64_t length, int32_t stream, int32_t fatal);
// The text of a TextDecoder's encoding, its `encoding` property.
const char *tz_text_decoder_name(const void *decoder);
// Whether a TextDecoder is fatal, and whether it ignores the BOM.
int32_t tz_text_decoder_flag(const void *decoder, int32_t which);

//===----------------------------------------------------------------------===//
// The process and the clock (process.c)
//===----------------------------------------------------------------------===//

// Programs call these functions, and console.error and process.stdout.write
// those of tzrt.c, through CodeGen::processFunction
// (compiler/library/process/codegen/codegen_process.cpp), which declares each
// where it is used.

// The code the program ends with when its code runs to the end:
// process.exitCode, or 0.
int32_t tz_process_exit_status(void);
// Releases what the runtime holds, before the leak check.
void tz_process_shutdown(void);
// Notes when the program starts, for performance.now().
void tz_clock_init(void);
// process.cwd(), and process.env[name]: whether it is set.
void tz_process_cwd(TzStr *out);
int32_t tz_process_env(TzStr *out, void *obj, uint64_t meta);
void tz_str_to_lower(TzStr *out, void *obj, uint64_t meta);
// stdout and stderr (tzrt.c): process.stdout.write and the like, and the
// output buffer written out (with `all` unset, its complete characters).
int32_t tz_stdout_write(void *obj, uint64_t meta);
int32_t tz_stderr_write(void *obj, uint64_t meta);
void tz_out_flush(int all);

//===----------------------------------------------------------------------===//
// Built-in modules: node:path (path.c), node:fs and tz:io (fs.c), node:os
// (os.c)
//===----------------------------------------------------------------------===//

// A function of the program that makes an object of the language from parts
// the runtime gives it (CodeGen::objectMaker): the parts are a struct of the
// object's fields, in an order both sides know, which the object takes over.
// The structs below are those parts; the compiler lays out the same fields
// in the same order (see makerFields in
// compiler/library/node/codegen/codegen_node.cpp).
typedef void *(*TzMaker)(const void *parts);

// `string | undefined` as the compiler lays it out.
typedef struct {
  TzStr value;
  uint8_t has;
} TzOptStr;

// path.parse(): ParsedPath.
typedef struct {
  TzStr root, dir, base, ext, name;
} TzParsedPath;

// SystemError: the fields of Error it sets, then its own.
typedef struct {
  TzStr message, name;
  int32_t errno_value;
  TzStr code, syscall;
  TzOptStr path, dest;
} TzSystemErrorParts;

// Stats of statSync, in the order Node prints them.
typedef struct {
  double dev, mode, nlink, uid, gid, rdev, blksize, ino, size, blocks;
  double atime_ms, mtime_ms, ctime_ms, birthtime_ms;
} TzStatsParts;

// Dirent of readdirSync: the kind is the file type bits of a mode (S_IFREG).
typedef struct {
  TzStr name, parent_path;
  int32_t kind;
} TzDirentParts;

// CpuInfo of os.cpus(), with the fields of its `times` in place.
typedef struct {
  TzStr model;
  double speed, user, nice, sys, idle, irq;
} TzCpuParts;

// os.userInfo().
typedef struct {
  int32_t uid, gid;
  TzStr username, homedir;
  TzOptStr shell;
} TzUserInfoParts;

// For fs.c: path.resolve() and path.toNamespacedPath() of one path, and
// path.join() of two, in the system's style.
void tz_path_resolve_units(TzBuf *out, const uint16_t *units, size_t length);
void tz_path_namespaced_units(TzBuf *out, const uint16_t *units, size_t length);
void tz_path_join_units(TzBuf *out, const uint16_t *a, size_t a_length, const uint16_t *b,
                        size_t b_length);
// Releases what fs.c holds (what stdin gave), before the leak check.
void tz_fs_shutdown(void);
// Releases the objects of Intl that toLocaleString and localeCompare keep
// (intl.c), before the leak check.
void tz_intl_shutdown(void);

#endif
