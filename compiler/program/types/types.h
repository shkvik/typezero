#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tz {

class Type;
struct ClassDecl;

// Computations over types of generic code (docs/generics.md, section 10),
// which wait until a copy gives their operands types: `keyof T`, `T[K]`,
// `Exclude<T, U>`, `Extract<T, U>`, a mapped type `{ [K in keyof T]: ... }`,
// `ReturnType<F>`, `Parameters<F>`, `NonNullable<T>`, a conditional type
// `T extends U ? A : B`, a template literal type `` `on${K}` ``, the
// intrinsic `Uppercase<S>`, `Lowercase<S>`, `Capitalize<S>` and
// `Uncapitalize<S>`, and a tuple with such a computation spread into it,
// `[...Tuple<A>, ...Tuple<B>]` (its operands are pairs of whether the
// element is spread, as a boolean literal type, and the element).
enum class TypeOp : uint8_t {
  None,
  KeyOf,
  Index,
  Exclude,
  Extract,
  Mapped,
  ReturnType,
  Parameters,
  NonNullable,
  Conditional,
  Template,
  Uppercase,
  Lowercase,
  Capitalize,
  Uncapitalize,
  Spread,
};

// The parts of a union that lie apart in memory (docs/objects.md, section
// 7). A value of a union whose members take more than one slot holds the
// number of its slot as a tag, the same number for a slot in every union:
// converting between unions keeps the tag. The runtime reads it too (see
// TZ_T_UNION in runtime/core/tzrt/tzrt.h).
enum class UnionSlot : uint8_t {
  None = 0, // undefined, in `A | B | undefined`
  String,
  Atom, // string literals
  Int,
  Long,
  Double,
  Bool,
  Decimal,
  Object, // objects of any types, told apart by their descriptors
  Array,
  Function,
  Iterator,
  Param, // a type parameter, in generic code only
  Other, // Json, Result, void: not part of a union
};

// The objects of Intl (docs/date.md, docs/intl.md) that the runtime makes:
// each kind is an object type of its own, without fields the program sees.
// Segments is what Intl.Segmenter's segment() gives.
enum class IntlKind : uint8_t {
  None,
  DateTimeFormat,
  NumberFormat,
  Collator,
  PluralRules,
  RelativeTimeFormat,
  ListFormat,
  DisplayNames,
  Segmenter,
  Segments,
};

// A field of an object type. An optional field (`x?: T`) has the type
// `T | undefined`: a field that is not there and one that holds undefined are
// the same thing, so `x: T | undefined` declares the same field.
struct Field {
  std::string name;
  const Type *type = nullptr;
  bool readonly = false;
};

// A method of an interface with several signatures (docs/overloads.md,
// section 4), `add(v: Vec): Vec; add(k: double): Vec`: no field holds it,
// so only a class implements it, each signature with a variant of its own.
struct Overload {
  std::string name;
  std::vector<const Type *> signatures; // function types
};

// The types of the language. Every type exists once (primitives are
// singletons, composite types are interned), so types are compared by pointer.
//
// Object types are exact and structural: the same fields of the same types
// make the same type, whatever it is called (an interface, an alias or a
// literal). An object type that refers to itself, directly or through other
// declared types, is the one exception: it is identified by its declaration.
//
// A tuple, `[string, int]`, is an object whose fields are named "0", "1"...:
// a reference, like an array in JavaScript, but of a fixed length, with a
// type for each place.
//
// A class is an object type too, but it is told apart by name: its type is
// only itself, whatever its fields (docs/objects.md, section 2).
class Type {
public:
  enum class Kind {
    Error,     // result of an ill-typed expression; silences follow-up errors
    Void,
    Undefined, // the type of `undefined` itself
    Bool,
    Int,       // 32-bit signed integer
    Long,      // 64-bit signed integer
    Double,    // 64-bit float, also spelled `number`
    Decimal,   // exact decimal of any length (docs/decimal.md); not isNumeric()
    String,
    Optional,  // `T | undefined`
    Array,     // `T[]`
    Function,  // `(a: int, b?: string) => T`
    Object,    // `{ x: double; y?: string }`
    Param,     // a type parameter `T`, inside its generic function or alias
    // `Result<T, E>` of neverthrow, built in: Ok with a T or Err with an E.
    // A value, not a reference (see docs/errors.md).
    Result,
    // The half of a Result that cannot happen: `ok(1)` is `Result<int,
    // never>`. Values of it exist only in code that never runs, such as the
    // callback of mapErr on such a Result.
    Never,
    Iterator,  // `Generator<T, R, N>`, also spelled `IterableIterator<T>`
    // A value of JSON (docs/modules.md, section 5): null, a boolean, a
    // number, a string, an array of Json or an object of them. A reference to
    // a node of the runtime (runtime/library/json/json/json.c).
    Json,
    // A dictionary (docs/objects.md, section 9): `Record<string, V>` (also
    // written `{ [key: string]: V }`), `Map<K, V>` or `Set<T>`. A reference to
    // a hash table of the runtime (runtime/core/map/map.c), which keeps its
    // keys in the order they were added.
    Dict,
    // The type of one value (docs/objects.md, section 7): `"circle"`, `0`,
    // `true`. A string literal is held as a small number, its atom (see
    // atom()); a number or a boolean one as a value of its base type.
    Literal,
    // `A | B | ...`: two or more members, none of them undefined (`A | B |
    // undefined` is the optional of the union). How a value lies in memory
    // follows from the slots its members take (see UnionSlot).
    Union,
    // `Promise<T>` (docs/async.md): a reference to an object of the runtime
    // (runtime/core/async/async.c) that holds a T once it is fulfilled.
    Promise,
    // A value of any type, which code may use only once a check showed what
    // it holds (docs/decorators.md, section 4): undefined included, so
    // `unknown | undefined` is unknown. A reference: an object of the
    // language as it is, any other value in a box of the runtime
    // (runtime/core/unknown/unknown.c), null for undefined.
    Unknown,
    // A class itself as a value, `typeof User` (docs/decorators.md): what a
    // decorator gets. A reference to the object of the class, which lives as
    // long as the program. `Constructor<T>` is one too: some class whose
    // objects are T, known only when the program runs (section 6).
    ClassRef,
  };
  // What a dictionary is in the program: they share one hash table, and
  // differ in how code reads and writes it.
  enum class DictKind { Record, Map, Set };

  Kind kind() const { return kind_; }
  bool isError() const { return kind_ == Kind::Error; }
  bool isVoid() const { return kind_ == Kind::Void; }
  bool isUndefined() const { return kind_ == Kind::Undefined; }
  bool isBool() const { return kind_ == Kind::Bool; }
  bool isInt() const { return kind_ == Kind::Int; }
  bool isString() const { return kind_ == Kind::String; }
  bool isInteger() const { return kind_ == Kind::Int || kind_ == Kind::Long; }
  bool isFloat() const { return kind_ == Kind::Double; }
  bool isNumeric() const { return isInteger() || isFloat(); }
  bool isDecimal() const { return kind_ == Kind::Decimal; }
  bool isOptional() const { return kind_ == Kind::Optional; }
  bool isArray() const { return kind_ == Kind::Array; }
  bool isFunction() const { return kind_ == Kind::Function; }
  bool isObject() const { return kind_ == Kind::Object; }
  bool isTuple() const { return tuple_; }
  bool isParam() const { return kind_ == Kind::Param; }
  bool isResult() const { return kind_ == Kind::Result; }
  bool isNever() const { return kind_ == Kind::Never; }
  bool isClass() const { return cls_ != nullptr; }
  // The declaration of a class type.
  const ClassDecl *classDecl() const { return cls_; }
  bool isIterator() const { return kind_ == Kind::Iterator; }
  bool isJson() const { return kind_ == Kind::Json; }
  bool isDict() const { return kind_ == Kind::Dict; }
  bool isUnknown() const { return kind_ == Kind::Unknown; }
  bool isClassRef() const { return kind_ == Kind::ClassRef; }
  // The class of `typeof C`; null for `Constructor<T>`.
  const ClassDecl *classRef() const { return isClassRef() ? refClass_ : nullptr; }
  // `Constructor<T>`: a class known only when the program runs, whose
  // objects are T (`object` for any class), its `constructed()`.
  bool isConstructor() const { return isClassRef() && !refClass_; }
  const Type *constructed() const { return isConstructor() ? inner_ : nullptr; }
  bool isRecord() const { return isDict() && dictKind_ == DictKind::Record; }
  bool isMap() const { return isDict() && dictKind_ == DictKind::Map; }
  bool isSet() const { return isDict() && dictKind_ == DictKind::Set; }
  DictKind dictKind() const { return dictKind_; }
  // Dictionaries: the type of the keys (a Set's elements), and of the values
  // (void for a Set).
  const Type *keyType() const { return isDict() ? params_[0] : nullptr; }
  const Type *valueType() const { return isDict() ? inner_ : nullptr; }
  bool isLiteral() const { return kind_ == Kind::Literal; }
  bool isUnion() const { return kind_ == Kind::Union; }

  // Literal types: the type of their value (string, int, double or boolean),
  // and the value.
  const Type *literalBase() const { return isLiteral() ? inner_ : nullptr; }
  const std::u16string &literalText() const { return text_; }
  double literalNumber() const { return number_; }
  bool isStringLiteral() const { return isLiteral() && inner_->isString(); }
  // A string literal's number among the string literals of the program,
  // from 1 (0 is undefined in `"a" | undefined`): the value that holds it.
  unsigned atom() const { return atom_; }
  // Unions: the members, in the order first written.
  llvm::ArrayRef<const Type *> members() const {
    return isUnion() ? llvm::ArrayRef<const Type *>(params_) : llvm::ArrayRef<const Type *>();
  }
  // The slots the members of a union take, in the order of their numbers.
  llvm::ArrayRef<UnionSlot> slots() const { return slots_; }
  // Whether a value of the union holds a tag: its members take several
  // slots.
  bool isTagged() const { return isUnion() && slots_.size() > 1; }
  // The part of a union that takes `slot`: the member there, or the union of
  // those; null if none. For another type, itself if it takes that slot.
  const Type *slotPart(UnionSlot slot) const;
  // A number unique to the type, in the order types are made.
  unsigned id() const { return id_; }
  bool isPromise() const { return kind_ == Kind::Promise; }
  // The value of a Promise: void for a Promise<void>.
  const Type *promised() const { return isPromise() ? inner_ : nullptr; }
  // The objects setTimeout and setInterval give (Timeout), and setImmediate
  // (Immediate): objects of the runtime without fields the program sees.
  bool isTimer() const { return timer_ != 0; }
  bool isImmediate() const { return timer_ == 2; }
  // Other objects of the runtime that the program reaches only through
  // their methods and properties (docs/async.md, step 2): AbortController,
  // AbortSignal, FileHandle of node:fs/promises, Interface of node:readline
  // and of node:readline/promises (whose question() gives a Promise), and
  // the async iterators, `AsyncGenerator<T, R, N>`: what an async generator
  // gives, and setInterval of node:timers/promises.
  enum class Runtime : uint8_t {
    None,
    AbortSignal, // the runtime's side of one (runtime/library/events/signal/signal.c)
    FileHandle,
    Readline,
    ReadlinePromises,
    AsyncIterator,
  };
  Runtime runtimeKind() const { return runtime_; }
  bool isRuntimeObject() const { return runtime_ != Runtime::None; }
  bool isAsyncIterator() const { return runtime_ == Runtime::AsyncIterator; }
  // The objects of node:crypto (docs/crypto.md), which the runtime makes
  // (runtime/library/crypto/crypto/crypto.h): types of their own without fields
  // the program sees, whose methods and properties reach what the runtime keeps
  // in them.
  enum class Crypto { None, Hash, Hmac, Cipheriv, Decipheriv, KeyObject, Sign, Verify, Ecdh, X509 };
  Crypto crypto() const { return crypto_; }
  bool isCrypto() const { return crypto_ != Crypto::None; }

  // The value type of `T | undefined`, the element type of `T[]`, or the
  // return type of a function.
  const Type *inner() const { return inner_; }
  // Function types: the types callers pass (`T | undefined` for an optional
  // parameter), how many arguments a call must give, and whether the last
  // parameter is `...rest: T[]`.
  llvm::ArrayRef<const Type *> params() const { return params_; }
  size_t minArgs() const { return minArgs_; }
  bool hasRest() const { return hasRest_; }
  // `(this: unknown, ...args: A) => R`: the first parameter is the `this`
  // the function gets, which only `f.call(obj, ...)` and `f.apply(obj,
  // args)` pass (docs/decorators.md, section 2).
  bool hasThis() const { return hasThis_; }
  const Type *ret() const { return inner_; }
  // `Result<T, E>`: T and E. Either may be void (`ok()`), or never.
  const Type *okType() const { return inner_; }
  const Type *errType() const { return error_; }
  // `T` for `T | undefined`, the type itself otherwise.
  const Type *nonOptional() const { return isOptional() ? inner_ : this; }
  // Object types: the fields in the order of the declaration (or literal)
  // that first made the type.
  llvm::ArrayRef<Field> fields() const { return fields_; }
  const Field *field(llvm::StringRef name) const;
  // Interfaces: the methods with several signatures.
  llvm::ArrayRef<Overload> overloads() const { return overloads_; }
  const Overload *overload(llvm::StringRef name) const;
  // A recursive object type while its declaration is being resolved: its
  // fields are not known yet.
  bool isIncomplete() const { return incomplete_; }
  // The object type of anyObject().
  bool isAnyObject() const { return anyObject_; }
  // Regular expressions (docs/regex.md). RegExp is an object type of its
  // own, whose objects the runtime makes; its one field is lastIndex. A
  // match, RegExpMatchArray, is an object type too: the captures in the
  // hidden field "%items" (a name no program can write), then index, input
  // and groups. Its type knows the groups of the regex, when the regex is
  // known: how many there are and which hold a value in every match.
  bool isRegExp() const { return regExp_; }
  bool isMatch() const { return match_; }
  // Dates (docs/date.md): Date is an object type of its own whose one field,
  // which no program can name, is the time value. Intl.DateTimeFormat is one
  // whose objects the runtime makes.
  bool isDate() const { return date_; }
  bool isDateFormat() const { return intl_ == IntlKind::DateTimeFormat; }
  // Any object of Intl, Intl.DateTimeFormat among them.
  bool isIntl() const { return intl_ != IntlKind::None; }
  IntlKind intlKind() const { return intl_; }
  // `WeakRef<T>` (docs/objects.md, section 11): an object type of its own,
  // without fields the program sees, that refers to an object of T without
  // holding it.
  bool isWeakRef() const { return weakRef_; }
  const Type *weakTarget() const { return weakRef_ ? inner_ : nullptr; }
  // Binary data (docs/bytes.md): object types of their own, whose objects
  // the runtime makes (runtime/library/bytes/bytes/bytes.c). The typed arrays
  // are numbered by their elements as TZ_BYTES_* in runtime/core/tzrt/tzrt.h; a
  // Buffer is a Uint8Array with the methods of node.
  enum class Bytes {
    None,
    Uint8,
    Int8,
    Uint8Clamped,
    Int16,
    Uint16,
    Int32,
    Uint32,
    Float16,
    Float32,
    Float64,
    Buffer,
    ArrayBuffer,
    DataView,
    TextEncoder,
    TextDecoder,
  };
  Bytes bytes() const { return bytes_; }
  bool isBytes() const { return bytes_ != Bytes::None; }
  // A typed array or a Buffer: elements that `[i]` reads.
  bool isTypedArray() const { return bytes_ >= Bytes::Uint8 && bytes_ <= Bytes::Buffer; }
  bool isBuffer() const { return bytes_ == Bytes::Buffer; }
  // A view of bytes: a typed array, a Buffer or a DataView, which all lie in
  // memory as TzBytes does.
  bool isByteView() const { return isTypedArray() || bytes_ == Bytes::DataView; }
  // Of a typed array: what a read gives (int, long for Uint32Array, double
  // for the floating ones), the bytes of an element, and the values an
  // element holds (for the floating ones, all).
  const Type *elementType() const;
  unsigned elementSize() const;
  bool elementRange(int64_t &lo, int64_t &hi) const;
  // A match: how many groups (without the match itself), -1 if unknown.
  int matchGroups() const { return matchGroups_; }
  // Whether capture k (0 is the match) of a match holds a value in every
  // match.
  bool captureMandatory(unsigned k) const {
    return k == 0 || (k < mandatory_.size() && mandatory_[k]);
  }
  // Type parameters: what `T extends C` requires of T, null without a
  // constraint or with a union one, whose members alternatives() lists
  // (`T extends string | int`).
  const Type *constraint() const { return isParam() ? inner_ : nullptr; }
  llvm::ArrayRef<const Type *> alternatives() const {
    return isParam() ? llvm::ArrayRef<const Type *>(params_) : llvm::ArrayRef<const Type *>();
  }
  // The type parameter that a parameter `xs: Iterable<int>` makes its
  // function generic with: any value that iteration gives these values of
  // (docs/iterators.md, section 2); null for any other type.
  const Type *iterated() const { return isParam() ? iterated_ : nullptr; }
  void setIterated(const Type *element) { iterated_ = element; }
  // `A extends unknown[]`: a type parameter that the types of the
  // parameters of some function stand for, an argument list.
  bool isListParam() const { return isParam() && list_; }
  void setList() { list_ = true; }
  // A computation over types of generic code (see TypeOp), a type parameter
  // of its own until its operands are known. `T[K]` is the type of the
  // member K of T: `args[0]` of a list of types is `A[0]`, and `args[i]` is
  // `A[number]`. A mapped type `{ [K in keys]: value }` has K, the keys and
  // the value as operands, and adds (+1) or removes (-1) `?` and `readonly`
  // of the fields; with `source`, the T of `[K in keyof T]`, its fields keep
  // those of T otherwise.
  static const Type *typeOp(TypeOp op, llvm::ArrayRef<const Type *> operands);
  static const Type *indexedAccess(const Type *object, const Type *key) {
    return typeOp(TypeOp::Index, {object, key});
  }
  // A mapped type may rename its keys, `[K in keyof T as rename]`.
  static const Type *mapped(const Type *key, const Type *keys, const Type *value, int optional,
                            int readonly, const Type *source, const Type *rename = nullptr);
  TypeOp typeOp() const { return isParam() ? op_ : TypeOp::None; }
  llvm::ArrayRef<const Type *> operands() const { return operands_; }
  int mappedOptional() const { return mappedOptional_; }
  int mappedReadonly() const { return mappedReadonly_; }
  // Iterators: the values they give (T), what a generator returns when it
  // finishes (R, void for nothing), and what next(x) sends to its `yield`
  // (N, void when the values sent are dropped).
  const Type *yielded() const { return inner_; }
  const Type *returned() const { return isIterator() || isAsyncIterator() ? params_[0] : nullptr; }
  const Type *sent() const { return isIterator() || isAsyncIterator() ? params_[1] : nullptr; }
  // Object types: the types it is declared to be too (docs/objects.md,
  // section 2): for a class, the class it extends and the interfaces it
  // implements; for an interface, those it extends. An object type with the
  // same fields as an interface is that interface, so it has its bases too.
  llvm::ArrayRef<const Type *> bases() const { return bases_; }
  void addBase(const Type *base);
  // Whether some type has this one among its bases.
  bool isBase() const { return isBase_; }
  // Whether a value of this type is a `base` too: it is that type, or one of
  // its bases is, up the chain.
  bool isSubtypeOf(const Type *base) const;
  // Whether values hold references that are counted: strings, arrays,
  // functions, objects, iterators and optionals of those.
  bool isManaged() const;
  // Whether `new Array<T>(n)` can fill elements of this type with a default:
  // zero, false, the empty string or undefined.
  bool hasZeroValue() const;

  // Width in bits of a numeric type.
  unsigned bitWidth() const;
  llvm::StringRef name() const { return name_; }

  static const Type *error();
  static const Type *voidTy();
  static const Type *undefinedTy();
  static const Type *boolTy();
  static const Type *intTy();
  static const Type *longTy();
  static const Type *doubleTy();
  static const Type *decimalTy();
  static const Type *string();
  static const Type *json();
  // `type | undefined`. Optional and undefined types stay as they are.
  static const Type *optional(const Type *type);
  static const Type *array(const Type *element);
  static const Type *function(llvm::ArrayRef<const Type *> params, size_t minArgs, bool hasRest,
                              const Type *ret, bool hasThis = false);
  // The object type with these fields. `name` names it in messages if it is
  // new; otherwise it is spelled out, `{ x: int; y: int }`.
  static const Type *object(std::vector<Field> fields, llvm::StringRef name = {},
                            std::vector<Overload> overloads = {});
  // A recursive object type, while its declaration `name` is resolved: the
  // fields that mention it get this type, and defineFields completes it.
  static Type *declareObject(llvm::StringRef name);
  void defineFields(std::vector<Field> fields, std::vector<Overload> overloads = {});
  // The same for a union that refers to itself through the fields of its
  // members (`type Expr = Num | { kind: "add"; left: Expr; right: Expr }`):
  // identified by its declaration; defineMembers gives it the members of
  // `of`, a union made of them.
  static Type *declareUnion(llvm::StringRef name);
  void defineMembers(const Type *of);
  // `[elements...]`, or with `readonly` one whose elements cannot be
  // assigned to. In generic code an element may be a list of types
  // (isListParam), which is spread there: `[int, ...A]`.
  static const Type *tuple(llvm::ArrayRef<const Type *> elements, bool readonly = false);
  // The types of the parameters of a function as a tuple
  // (docs/generics.md, section 9): what `A extends unknown[]` stands for.
  // The elements from `min` on may be left out (their types are `T |
  // undefined`), and `rest`, an array, takes the arguments after them:
  // `[string, string?, ...int[]]`. With all of them needed and no rest, it
  // is the tuple of the elements.
  static const Type *argumentList(llvm::ArrayRef<const Type *> elements, size_t min, const Type *rest);
  // Of a tuple: how many elements a call must give, and the array type of the
  // arguments after them (see argumentList).
  size_t tupleMin() const { return tupleMin_; }
  const Type *tupleRest() const { return tupleRest_; }
  // A new type parameter named `name`; each declaration of one has its own.
  static Type *param(llvm::StringRef name);
  void setConstraint(const Type *constraint, std::vector<const Type *> alternatives);
  // Some object, of a type the code does not know: the type parameter of a
  // copy of a generic function that serves every object type (see
  // Sema::representation). It has no fields, and is released through the
  // descriptor the object points at.
  static const Type *anyObject();
  static const Type *never();
  static const Type *unknown();
  // `typeof C`, the class itself as a value; `name` is the class's.
  static const Type *classRef(const ClassDecl *cls, llvm::StringRef name);
  // `Constructor<instance>`; `Constructor` alone for anyObject().
  static const Type *constructorOf(const Type *instance);
  // `Result<ok, err>`; `undefined` in either half is void, as `ok()` and
  // `ok(undefined)` are the same.
  static const Type *result(const Type *ok, const Type *err);
  // The type of the instances of a class, which is only itself: never
  // interned with other object types. Its fields come with defineFields.
  static Type *classType(llvm::StringRef name, const ClassDecl *decl);
  void rename(std::string name) { name_ = std::move(name); }
  // Makes `a` the type of the class `b` was the type of, and the other way
  // round: what each is made of (its class, fields, bases) goes to the other,
  // and every type that mentions one means the other class from then on.
  // The names stay. A base that was the other type becomes that one.
  static void swapClasses(Type *a, Type *b);
  // The fields of a class type, to be found again (its class extends
  // another class now).
  void forgetFields() {
    fields_.clear();
    incomplete_ = true;
  }
  void replaceBase(const Type *from, const Type *to) {
    for (const Type *&base : bases_)
      if (base == from)
        base = to;
  }
  // The type of the instances of a generic class (`Box<T>` inside it) or of
  // a class made from one (`Box<int>`): what its type parameters stand for.
  llvm::ArrayRef<const Type *> typeArgs() const { return typeArgs_; }
  void setTypeArgs(std::vector<const Type *> args) { typeArgs_ = std::move(args); }
  // `Generator<yielded, returned, sent>`. An iterator is a reference to an
  // object of the runtime, whatever made it: a generator, the iterator of an
  // array or a helper such as map.
  static const Type *iterator(const Type *yielded, const Type *returned = voidTy(),
                              const Type *sent = voidTy());
  static const Type *regExp();
  static const Type *date();
  static const Type *dateFormat();
  // `Intl.NumberFormat` and the other kinds; dateFormat() for
  // DateTimeFormat.
  static const Type *intl(IntlKind kind);
  static const Type *weakRef(const Type *target);
  // `Record<string, value>`, `Map<key, value>`, or `Set<key>` with a void
  // value.
  static const Type *dict(DictKind kind, const Type *key, const Type *value);
  static const Type *promise(const Type *value);
  static const Type *timeout();
  static const Type *immediate();
  static const Type *cryptoType(Crypto kind);
  // Uint8Array, Buffer, ArrayBuffer... (see Bytes); Buffer is a Uint8Array.
  static const Type *bytesType(Bytes kind);
  // The objects of the runtime but the async iterators.
  static const Type *runtimeObject(Runtime kind);
  static const Type *asyncIterator(const Type *yielded, const Type *returned = voidTy(),
                                   const Type *sent = voidTy());
  // The match of a regex with `groups` groups (-1: unknown), of which those
  // with `mandatory` set (by capture, from 0) always hold a value; `named`
  // is the type of `groups` (null without names), and with `indices` (the d
  // flag) the match has them.
  static const Type *match(int groups, std::vector<bool> mandatory, const Type *named,
                           bool indices);
  // Literal types.
  static const Type *stringLiteral(const std::u16string &text);
  // A string of a pattern, `` `on${string}` `` (docs/generics.md, section
  // 10): a string in memory, and a literal is one if it matches. `texts`
  // are around the holes, one more of them; a hole is string, double, int
  // or long (`${number}` is a double).
  static const Type *stringPattern(std::vector<std::u16string> texts,
                                   std::vector<const Type *> holes);
  bool isPattern() const { return pattern_; }
  llvm::ArrayRef<std::u16string> patternTexts() const { return texts_; }
  llvm::ArrayRef<const Type *> patternHoles() const {
    return pattern_ ? llvm::ArrayRef<const Type *>(params_) : llvm::ArrayRef<const Type *>();
  }
  // `base` is int (a whole number that fits) or double.
  static const Type *numberLiteral(double value, const Type *base);
  static const Type *boolLiteral(bool value);
  // The string literals made so far, by their atoms (from 1).
  static llvm::ArrayRef<const Type *> stringLiterals();
  // The union of `types`, made the way TypeScript makes one: nested unions
  // are flattened, `never` is dropped, undefined makes the optional of the
  // rest, a literal goes into its base type (`"a" | string` is string), both
  // boolean literals make boolean, an object type goes into another member
  // it is declared to be, and one member left is that member. Unions that
  // cannot be told apart at run time come out too (see unionProblem).
  static const Type *unionOf(llvm::ArrayRef<const Type *> types);

  // The type a plain name in a type annotation refers to, or null.
  static const Type *fromName(llvm::StringRef name);

private:
  Type(Kind kind, std::string name, const Type *inner = nullptr)
      : kind_(kind), inner_(inner), name_(std::move(name)), id_(++count_) {}
  static unsigned count_;

  Kind kind_;
  const Type *inner_;
  const Type *error_ = nullptr; // the E of a Result
  std::string name_;
  std::vector<const Type *> params_;
  size_t minArgs_ = 0;
  bool hasRest_ = false;
  bool hasThis_ = false;
  const ClassDecl *refClass_ = nullptr;
  std::vector<Field> fields_;
  std::vector<Overload> overloads_;
  bool incomplete_ = false;
  bool tuple_ = false;
  bool anyObject_ = false;
  const ClassDecl *cls_ = nullptr;
  bool regExp_ = false;
  bool match_ = false;
  bool date_ = false;
  IntlKind intl_ = IntlKind::None;
  bool weakRef_ = false;
  int timer_ = 0; // 1 for Timeout, 2 for Immediate
  Crypto crypto_ = Crypto::None;
  Bytes bytes_ = Bytes::None;
  Runtime runtime_ = Runtime::None;
  int matchGroups_ = -1;
  std::vector<bool> mandatory_;
  std::vector<const Type *> bases_;
  bool isBase_ = false;
  DictKind dictKind_ = DictKind::Record;
  std::u16string text_; // of a string literal
  bool pattern_ = false;
  std::vector<std::u16string> texts_; // of a pattern
  double number_ = 0;   // of a number literal; 1 or 0 for a boolean one
  unsigned atom_ = 0;
  std::vector<UnionSlot> slots_;
  std::vector<const Type *> typeArgs_; // of a class made from a generic one
  const Type *iterated_ = nullptr;     // see iterated()
  bool list_ = false;                  // see isListParam()
  TypeOp op_ = TypeOp::None;          // see typeOp()
  std::vector<const Type *> operands_;
  int mappedOptional_ = 0, mappedReadonly_ = 0;
  size_t tupleMin_ = 0;                // see tupleMin()
  const Type *tupleRest_ = nullptr;
  unsigned id_;
};

// The slot that values of `type` take in a union.
UnionSlot unionSlot(const Type *type);

// Why values of the members of `type`, a union, cannot be told apart when
// the program runs (`int | double`: both are numbers to typeof), or why one
// cannot be a member; empty if nothing is wrong.
std::string unionProblem(const Type *type);

// Whether a member of the union `type` is a type parameter (generic code).
bool hasParamMember(const Type *type);

// `type` without the members of the union `without` (or `without` itself):
// what a narrowed value is where a check failed. Null if nothing is left.
const Type *unionWithout(const Type *type, const Type *without);

// How many steps up the declared chain of `from` (Type::bases) `to` is: 0
// for the same type, -1 if it is not up the chain.
int subtypeDistance(const Type *from, const Type *to);

// Whether `type` mentions a type parameter anywhere.
bool mentionsParam(const Type *type);

// Whether `type` mentions a type parameter other than `allowed` (the types
// `infer` declares, the key of a mapped type), as a computation that waits
// for a copy does.
bool mentionsParamBesides(const Type *type, llvm::ArrayRef<const Type *> allowed);

// `[parts...]` with those marked in `spread` spread into it, `[int, ...A]`:
// a list of types (isListParam) stays an element, spread there; a tuple
// gives its elements, and an array, last, the rest of an argument list
// (Type::argumentList). Null with `why` set if that cannot be.
const Type *spreadTuple(llvm::ArrayRef<const Type *> parts, const std::vector<bool> &spread, std::string &why);

// The parameters `params` of a function, whose last one is `...args: L` of
// an argument list L (a tuple, see Type::argumentList), with the types of L
// in its place: `(int, ...[string, bool?])` is `(int, string, bool?)`.
// False if there is no such last parameter; `minArgs` and `hasRest` are
// those of the result.
bool flattenArguments(llvm::ArrayRef<const Type *> params, size_t &minArgs, bool &hasRest,
                      std::vector<const Type *> &flat);

// `{ x: int; y?: string }`: an object type spelled out.
std::string objectTypeSpelling(llvm::ArrayRef<Field> fields);

// What `typeof` gives for a value of `type` ("number", "string"...). For
// `T | undefined` that holds a value, it is the name for T.
const char *typeOfName(const Type *type);

// The field that tells the object types `objects` apart in JSON, for
// JSON.parse<T> of a union of them (docs/narrowing.md, section 3): one that
// each of them has, of a literal type, with another value in each, as
// `kind: "circle"`. Empty if there is none.
std::string jsonTagField(llvm::ArrayRef<const Type *> objects);

// The parts of `text` the holes of a template literal type match, the
// template having `texts` around its holes, as TypeScript splits a string
// literal: each hole but the last takes the text up to where the text after
// it is found first (one character if that text is empty), the last one the
// rest. False if the texts are not there.
bool splitByTemplate(const std::u16string &text, llvm::ArrayRef<std::u16string> texts,
                     std::vector<std::u16string> &parts);

// Whether `${number}` of a template literal type matches `text`: what
// JavaScript's Number(text) reads as a finite number, as TypeScript checks;
// `value` is that number.
bool numberString(const std::u16string &text, double &value);

// Whether the part of a string that a hole of a pattern took fits the hole.
bool holeAccepts(const Type *hole, const std::u16string &part);

// Whether the string literal `text` is a string of `pattern`
// (Type::stringPattern).
bool matchesPattern(const std::u16string &text, const Type *pattern);

} // namespace tz
