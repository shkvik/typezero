#pragma once

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/SMLoc.h"

#include <cstdint>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tz {

class Type;

// A variable, parameter or global. Created by Sema, shared by all its uses.
struct FunctionDecl;
struct ClassDecl;
class RegexLitExpr;

struct VarSymbol {
  llvm::StringRef name;
  llvm::SMLoc loc;
  const Type *type = nullptr;
  bool isConst = false;
  bool isGlobal = false;
  // A global whose name some function mentions, so a call may read or change
  // it. Other globals live in main's stack frame, where LLVM can keep them in
  // registers, and calls cannot touch them.
  bool usedInFunction = false;

  // Closures (see FunctionDecl::captures). The function whose frame holds the
  // variable: null for main and for globals.
  FunctionDecl *owner = nullptr;
  // The order of declarations, to tell whether a closure is created before
  // the variable is.
  unsigned declSeq = 0;
  // A function nested in some function uses the variable.
  bool captured = false;
  // Assigned after its declaration, anywhere.
  bool reassigned = false;
  // Captured by a closure created before the variable is declared.
  bool capturedEarly = false;
  // The variable a nested function declaration introduces: its value is that
  // function.
  FunctionDecl *function = nullptr;
  // A constant initialized with an arrow or a function expression (or the
  // name of a function expression inside it): calls through it are direct,
  // and the function calls itself without capturing itself.
  FunctionDecl *lambda = nullptr;
  // A variable that holds the regex of this literal all along (a constant,
  // or a `let` nothing assigns to): its flags and groups are known.
  const RegexLitExpr *regex = nullptr;
  // A constant that holds the method Reflect.get gave of an object of a
  // class known when compiling (see Builtin::ReflectMethod): Reflect.apply of
  // it may call the method itself.
  const FunctionDecl *reflectMethod = nullptr;

  // Closures copy the value of a variable that never changes after they are
  // created; any other variable they capture lives in a box on the heap,
  // shared by the function that declares it and the closures.
  bool boxed() const { return captured && (reassigned || capturedEarly); }
};

struct FuncSymbol {
  llvm::StringRef name;
  llvm::SMLoc loc;
  // The types callers pass: `T | undefined` for a parameter that is optional
  // or has a default value, as a caller may leave it out.
  std::vector<const Type *> params;
  // The arguments a call must give: those up to the last parameter that is
  // neither optional nor has a default value.
  size_t minArgs = 0;
  // The last parameter is `...rest: T[]`: the arguments from its position on
  // become the elements of one array.
  bool hasRest = false;
  // `function (this: unknown, ...)`: the first parameter is the `this` the
  // function gets (see Type::hasThis).
  bool hasThis = false;
  // Without an annotation, null until Sema infers it from the body (see
  // Sema::returnType).
  const Type *ret = nullptr;
  // Set while Sema infers the return type: a call now is recursion, and
  // returns `assumed`, the type found so far (null at first).
  bool inferring = false;
  const Type *assumed = nullptr;
  FunctionDecl *decl = nullptr;
  // An arrow passed where a callback returns void, as in forEach: what its
  // body returns is dropped.
  bool discardsResult = false;
  // Some parameter has its type from a default value that is not a literal,
  // found when first needed (Sema::resolveParams).
  bool pendingParams = false;
  // A generic function: its type parameters, which its parameter and return
  // types mention. A call gives them types and calls a copy of the function
  // made for those (see Sema::checkGenericCall).
  std::vector<const Type *> typeParams;
  std::vector<const Type *> typeDefaults; // `<T = int>`; null without one
  bool isGeneric() const { return !typeParams.empty(); }
  // `x is T`, `asserts x is T` or `asserts x` as the return type: the
  // parameter (by position; -1 for `this`, -2 for none), T (null for
  // `asserts x`), and whether it asserts (see sema_predicate.cpp).
  int predicateParam = -2;
  const Type *predicateType = nullptr;
  bool asserts = false;
};

// Base of every AST node. Nodes are owned by AstContext and linked by raw
// pointers. Subclasses use LLVM-style RTTI (`classof`), since the compiler is
// built without C++ RTTI.
class Node {
public:
  virtual ~Node() = default;
};

// A type written in the source: `int`, `string[]`, `int | undefined`,
// `Array<int>`, `{ x: int; y?: string }`, `[string, int]`.
struct TypeRef {
  enum class Kind {
    Name,     // `name`; empty after a syntax error
    Array,    // `args[0][]`
    Union,    // `args[0] | args[1] | ...`
    Generic,  // `name<args...>`
    Function, // `(a: args[0], b?: args[1]) => args.back()`
    Object,   // `{ names[0]: args[0]; names[1]?: args[1] }`
    Tuple,    // `[args[0], args[1]]`; `readonly` before it makes every element read-only
    // `"circle"`, `0`, `-1.5`, `true`: the type of one value. `name` is
    // "string", "number" or "boolean"; the value is in `text` or `number`.
    Literal,
    // `x is args[0]`, `asserts x is args[0]` and `asserts x` (no args), in
    // a return type: `name` is the parameter, or "this".
    Predicate,
    // Computations over types (docs/generics.md, section 10): `keyof
    // args[0]`; `args[0][args[1]]`; `typeof names[0].names[1]...`; and a
    // mapped type `{ [name in args[0]]: args[1] }`, whose modifiers are in
    // `mappedOptional` and `mappedReadonly`, and whose keys may be renamed,
    // `[name in args[0] as args[2]]`.
    KeyOf,
    Indexed,
    TypeOf,
    Mapped,
    // `args[0] extends args[1] ? args[2] : args[3]`; `source` is the text
    // written, which names the type in messages.
    Conditional,
    // `infer name` in the pattern of a conditional type, with its
    // constraint in args[0] if it has one (`infer N extends int`).
    Infer,
    // `` `on${args[0]}` ``: the texts around the types are in `texts`, one
    // more of them than of the types.
    Template,
    // `args[0] & args[1] & ...`
    Intersection,
  };

  Kind kind = Kind::Name;
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::vector<TypeRef> args;
  // Function types: which parameters are optional, and whether the last one
  // is a rest parameter. Object types: which fields are optional.
  std::vector<bool> optional;
  bool rest = false;
  // Object types: the names of the fields (function types: of the
  // parameters, of which `this` is the `this` the function gets), where they
  // are written, and which are `readonly`. A method, `area(): double`, is a
  // read-only field that
  // holds a function (docs/objects.md, section 5); `methods` marks those
  // written so.
  std::vector<llvm::StringRef> names;
  std::vector<llvm::SMLoc> locs;
  std::vector<bool> readonly;
  std::vector<bool> methods;
  // Tuple types: the elements spread into it, `[int, ...A]`, a list of the
  // types of the parameters of a function (docs/generics.md, section 9).
  std::vector<bool> spread;
  // Mapped types: `?` added (+1) or removed (-1, `-?`) on every field, and
  // the same for `readonly`; 0 keeps them.
  int mappedOptional = 0;
  int mappedReadonly = 0;
  // Literal types: the value (1 or 0 for a boolean), and whether a number is
  // written without a fraction. Predicates: whether it is `asserts`.
  std::u16string text;
  double number = 0;
  bool integer = false;
  bool asserts = false;
  // Template literal types: the texts (see Kind::Template).
  std::vector<std::u16string> texts;
  llvm::StringRef source; // of a conditional type
};

// `T extends number = int`: a type parameter of a generic function or type
// alias.
struct TypeParamDecl {
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::optional<TypeRef> constraint;
  std::optional<TypeRef> defaultType;
};

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

enum class UnaryOp {
  Neg,
  Plus,
  Not,
  BitNot,
  TypeOf,
  Void,    // `void x`: evaluates x; the value is undefined
  Delete,  // `delete x`: Sema rejects it, since objects and arrays always have their fields
  NonNull, // postfix `x!`
};

enum class BinaryOp {
  Add,
  Sub,
  Mul,
  Div,
  Rem,
  Pow,
  BitAnd,
  BitOr,
  BitXor,
  Shl,
  Shr,
  UShr,
  Lt,
  Le,
  Gt,
  Ge,
  Eq,
  Ne,
  LogAnd,
  LogOr,
  Nullish,    // `a ?? b`
  In,         // `key in object`: Sema rejects it until unions come
  InstanceOf, // `x instanceof C`: compares the class number in x's descriptor
};

const char *opSpelling(UnaryOp op);
const char *opSpelling(BinaryOp op);

class Expr : public Node {
public:
  enum class Kind {
    IntLit, FloatLit, BoolLit, StringLit, Undefined, Template, ArrayLit, ObjectLit, Name, Member,
    Index, Unary, Binary, Assign, Update, Call, New, Conditional, Cast, Sequence, OptionalChain,
    Function, Yield, RegexLit, Await, Class,
  };

  Kind kind() const { return kind_; }
  llvm::SMLoc loc() const { return loc_; }

  // Set by Sema.
  const Type *type = nullptr;

protected:
  Expr(Kind kind, llvm::SMLoc loc) : kind_(kind), loc_(loc) {}

private:
  Kind kind_;
  llvm::SMLoc loc_;
};

class IntLitExpr : public Expr {
public:
  IntLitExpr(llvm::SMLoc loc, uint64_t value) : Expr(Kind::IntLit, loc), value(value) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::IntLit; }

  // The magnitude; a leading minus is a separate UnaryExpr.
  uint64_t value;
  // The literal as written, from which a decimal reads its exact value;
  // empty for one the compiler made.
  llvm::StringRef text;
};

class FloatLitExpr : public Expr {
public:
  FloatLitExpr(llvm::SMLoc loc, double value) : Expr(Kind::FloatLit, loc), value(value) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::FloatLit; }

  double value;
  llvm::StringRef text; // as in IntLitExpr
};

class BoolLitExpr : public Expr {
public:
  BoolLitExpr(llvm::SMLoc loc, bool value) : Expr(Kind::BoolLit, loc), value(value) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::BoolLit; }

  bool value;
};

class StringLitExpr : public Expr {
public:
  StringLitExpr(llvm::SMLoc loc, std::u16string value)
      : Expr(Kind::StringLit, loc), value(std::move(value)) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::StringLit; }

  // UTF-16 code units, as JavaScript sees the string.
  std::u16string value;
};

// `/pattern/flags`. Sema compiles it (docs/regex.md): the program, a
// constant in the executable, says what its groups are.
class RegexLitExpr : public Expr {
public:
  RegexLitExpr(llvm::SMLoc loc, std::u16string pattern, std::u16string flags)
      : Expr(Kind::RegexLit, loc), pattern(std::move(pattern)), flags(std::move(flags)) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::RegexLit; }

  // As written: the escapes are the regex's own.
  std::u16string pattern;
  std::u16string flags;
  // The length of the literal in the source, from loc(); 0 for one that
  // `new RegExp("...")` stands for.
  size_t written = 0;
  // Set by Sema (see runtime/regex.h); empty after an error.
  std::vector<uint32_t> program;
};

class UndefinedExpr : public Expr {
public:
  explicit UndefinedExpr(llvm::SMLoc loc) : Expr(Kind::Undefined, loc) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Undefined; }
};

// `a${x}b${y}c`: quasis are the texts "a", "b", "c" around the expressions.
class TemplateExpr : public Expr {
public:
  explicit TemplateExpr(llvm::SMLoc loc) : Expr(Kind::Template, loc) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Template; }

  std::vector<std::u16string> quasis; // always exprs.size() + 1 entries
  std::vector<Expr *> exprs;
};

// `[a, b, ...c]`
class ArrayLitExpr : public Expr {
public:
  struct Element {
    Expr *expr;
    bool spread;       // `...expr`
    bool hole = false; // `[a, , b]`: allowed only in destructuring; expr is undefined
  };

  explicit ArrayLitExpr(llvm::SMLoc loc) : Expr(Kind::ArrayLit, loc) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::ArrayLit; }

  std::vector<Element> elements;
};

class IndexExpr;

// `{ x: 1, y, ...rest, [key]: v }`: an object with these fields, in this
// order; `y` alone is `y: y`. A spread copies the fields of another object,
// and a later field replaces an earlier one of the same name.
class ObjectLitExpr : public Expr {
public:
  struct Property {
    llvm::StringRef name; // set by Sema for a key in brackets; empty for a spread
    llvm::SMLoc loc;
    Expr *value; // the object spread, for a spread
    bool spread = false;
    // `[key]: value`: a key that must be a constant (see Sema::constantKey).
    Expr *key = nullptr;
    // `{ x = 1 }`, which only a destructuring pattern may contain: value is
    // the default.
    bool defaulted = false;
    // A spread that leaves out some fields: the rest of an object pattern,
    // `{ a, [k]: b, ...rest }`. The fields named, and the reads by a key in
    // brackets, whose fields Sema finds (IndexExpr::field).
    std::vector<llvm::StringRef> omit;
    std::vector<const IndexExpr *> omitKeys;
    // Set by Sema in a literal with spreads: a later property replaces this
    // value, which is only evaluated; or of a spread, the fields that a later
    // property replaces, which are not copied.
    bool replaced = false;
    std::vector<llvm::StringRef> replacedFields;
    // `f() { ... }`: a method, whose value is the FunctionExpr of its body.
    bool method = false;
  };

  explicit ObjectLitExpr(llvm::SMLoc loc) : Expr(Kind::ObjectLit, loc) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::ObjectLit; }

  std::vector<Property> props;
  // Set by Sema for a literal with methods: the class of its own that the
  // object is an instance of (see ClassDecl::literal).
  ClassDecl *cls = nullptr;
  bool hasSpread() const {
    return llvm::any_of(props, [](const Property &p) { return p.spread; });
  }
};

class NameExpr : public Expr {
public:
  NameExpr(llvm::SMLoc loc, llvm::StringRef name) : Expr(Kind::Name, loc), name(name) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Name; }

  llvm::StringRef name;
  // `first<int>`: the type arguments of a generic function, as a value or
  // as the callee of a call.
  std::vector<TypeRef> typeArgs;
  VarSymbol *var = nullptr; // set by Sema
  // Set by Sema for a top-level function used as a value.
  FuncSymbol *func = nullptr;
  // Set by Sema for a class used as a value (docs/decorators.md): the
  // object of the class.
  ClassDecl *cls = nullptr;
};

class MemberExpr : public Expr {
public:
  MemberExpr(llvm::SMLoc loc, Expr *object, llvm::StringRef name)
      : Expr(Kind::Member, loc), object(object), name(name) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Member; }

  Expr *object;
  llvm::StringRef name;
  bool optional = false; // `object?.name`
  // Set by Sema for a method read as a value: a function bound to the object.
  // With `virtualCall`, the method of the object's class at the slot of
  // `method`, as for CallExpr.
  FuncSymbol *method = nullptr;
  // `JSON.parse<User>(text)`: the type arguments of the callee of a call.
  std::vector<TypeRef> typeArgs;
  bool virtualCall = false;
  // A method of an interface with several signatures, as a value: which one.
  int signature = -1;
};

struct EnumDecl;

// `object[index]`
class IndexExpr : public Expr {
public:
  IndexExpr(llvm::SMLoc loc, Expr *object, Expr *index)
      : Expr(Kind::Index, loc), object(object), index(index) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Index; }

  Expr *object;
  Expr *index;
  bool optional = false; // `object?.[index]`
  // Set by Sema. A read that Sema could not prove in range checks the index
  // and yields `T | undefined`. A write may append: Sema proved only
  // `index <= length`.
  bool mayBeOutOfRange = false;
  bool mayAppend = false;
  // Set by Sema for `Color[value]`: the name of an enum member.
  EnumDecl *enumDecl = nullptr;
  // Set by Sema for `o[key]` of an object or a tuple with a constant key: the
  // field it reads, which Sema reads by name instead.
  llvm::StringRef field;
  // Set by Sema for `o[k]` of an object whose key is of a union of string
  // literals (`Record<"up" | "down", int>`), each naming a field of one
  // type: the field is chosen when the program runs.
  bool keyed = false;
};

class UnaryExpr : public Expr {
public:
  UnaryExpr(llvm::SMLoc loc, UnaryOp op, Expr *operand)
      : Expr(Kind::Unary, loc), op(op), operand(operand) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Unary; }

  UnaryOp op;
  Expr *operand;
};

class BinaryExpr : public Expr {
public:
  BinaryExpr(llvm::SMLoc loc, BinaryOp op, Expr *lhs, Expr *rhs)
      : Expr(Kind::Binary, loc), op(op), lhs(lhs), rhs(rhs) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Binary; }

  BinaryOp op;
  Expr *lhs;
  Expr *rhs;
  // The source text of the whole expression (loc() is the operator), for
  // hints that rewrite it. Empty for one Sema made.
  llvm::StringRef source;
};

// `target = value`, or a compound assignment such as `target += value`.
class AssignExpr : public Expr {
public:
  AssignExpr(llvm::SMLoc loc, std::optional<BinaryOp> op, Expr *target, Expr *value)
      : Expr(Kind::Assign, loc), op(op), target(target), value(value) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Assign; }

  std::optional<BinaryOp> op;
  Expr *target;
  Expr *value;
  // As in BinaryExpr.
  llvm::StringRef source;
};

// `++x`, `x++`, `--x`, `x--`.
class UpdateExpr : public Expr {
public:
  UpdateExpr(llvm::SMLoc loc, bool increment, bool prefix, Expr *target)
      : Expr(Kind::Update, loc), increment(increment), prefix(prefix), target(target) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Update; }

  bool increment;
  bool prefix;
  Expr *target;
};

// Functions and methods implemented by the compiler and the runtime.
enum class Builtin {
  None,
  ConsoleLog,   // also console.info and console.debug
  ConsoleError, // console.error and console.warn: as console.log, to stderr
  // process, Date.now() and performance.now() (sema_process.cpp). Reads of
  // properties such as `process.argv` become calls without arguments.
  ProcessArgv,
  ProcessEnv,         // process.env.NAME or process.env[name]: the name
  ProcessExit,        // process.exit(code?): `int | undefined`
  ProcessExitCode,    // reading process.exitCode
  ProcessSetExitCode, // `process.exitCode = code`: `int | undefined`
  ProcessCwd,
  ProcessExecPath,
  ProcessPlatform,
  ProcessStdoutWrite, // process.stdout.write(text)
  ProcessStderrWrite,
  ProcessIsTTY,       // process.stdout.isTTY or process.stderr.isTTY: the fd
  DateNow,
  PerformanceNow,
  StringOf,      // String(x)
  NumberOf,      // Number(x)
  ParseInt,      // parseInt and Number.parseInt
  ParseFloat,    // parseFloat and Number.parseFloat
  IsNaN,         // isNaN and Number.isNaN
  IsFinite,      // isFinite and Number.isFinite
  IsInteger,     // Number.isInteger
  IsSafeInteger, // Number.isSafeInteger
  FromCharCode,  // String.fromCharCode
  FromCodePoint, // String.fromCodePoint
  // encodeURIComponent, encodeURI, decodeURIComponent, decodeURI.
  UriEncodeComponent,
  UriEncode,
  UriDecodeComponent,
  UriDecode,
  // x.toFixed(digits) of a number.
  NumToFixed,
  // x.toString(radix) of a number.
  NumToRadix,
  // Methods of string; the receiver is the object of the callee MemberExpr.
  StrCharCodeAt,
  StrCharAt,
  StrCodePointAt,
  StrAt,
  StrSlice,
  StrSubstring,
  StrIndexOf,
  StrLastIndexOf,
  StrIncludes,
  StrStartsWith,
  StrEndsWith,
  StrRepeat,
  StrToString,
  StrConcat,
  StrSplit,
  StrTrim,
  StrTrimStart,
  StrTrimEnd,
  StrPadStart,
  StrPadEnd,
  StrToUpperCase,
  StrToLowerCase,
  StrReplace,
  StrReplaceAll,
  // Methods of arrays.
  ArrPush,
  ArrPop,
  ArrShift,
  ArrUnshift,
  ArrAt,
  ArrSlice,
  ArrConcat,
  ArrIndexOf,
  ArrLastIndexOf,
  ArrIncludes,
  ArrJoin,
  ArrReverse,
  ArrFill,
  ArrSort,
  ArrToString,
  ArrSplice,
  // Methods of arrays that take a callback: the first argument.
  ArrForEach,
  ArrMap,
  ArrFilter,
  ArrFlatMap,
  ArrReduce,
  ArrReduceRight,
  ArrFind,
  ArrFindIndex,
  ArrFindLast,
  ArrFindLastIndex,
  ArrSome,
  ArrEvery,
  ArrSortWith, // sort(compare)
  // Object.keys, Object.values and Object.entries of an object: the first
  // argument.
  ObjectKeys,
  ObjectValues,
  ObjectEntries,
  // Iterators (docs/iterators.md). keys(), values() and entries() of an
  // array; Iterator.from(x) and Array.from(x, f?) of an array, a string or an
  // iterator, the first argument.
  ArrKeys,
  ArrValues,
  ArrEntries,
  IterFrom,
  ArrayFrom,
  // `it.next(x?)` of an iterator.
  IterNext,
  // The helpers of iterators that give another iterator: a stage of a lazy
  // chain. The receiver is the source; the argument a callback or a count.
  IterMap,
  IterFilter,
  IterTake,
  IterDrop,
  IterFlatMap,
  // The helpers that consume an iterator.
  IterToArray,
  IterForEach,
  IterReduce,
  IterSome,
  IterEvery,
  IterFind,
  // The functions of Math. The first four round, and give an integer where
  // one is expected (see Sema::checkMathCall).
  MathFloor,
  MathCeil,
  MathRound,
  MathTrunc,
  MathAbs,
  MathSign,
  MathMin,
  MathMax,
  MathPow,
  MathSqrt,
  MathCbrt,
  MathExp,
  MathExpm1,
  MathLog,
  MathLog1p,
  MathLog2,
  MathLog10,
  MathSin,
  MathCos,
  MathTan,
  MathAsin,
  MathAcos,
  MathAtan,
  MathAtan2,
  MathSinh,
  MathCosh,
  MathTanh,
  MathAsinh,
  MathAcosh,
  MathAtanh,
  MathHypot,
  MathFround,
  MathClz32,
  MathImul,
  MathRandom,
  // decimal (sema_decimal.cpp): decimal(x), decimal.max, min and sum, and
  // the methods of a decimal, whose receiver is the object of the callee
  // MemberExpr.
  DecimalOf,
  DecMax,
  DecMin,
  DecSum,
  DecDiv,
  DecRound,
  DecToFixed,
  DecToFormat,
  DecToString,
  DecToExponential,
  DecToPrecision,
  DecSqrt,
  DecAbs,
  DecIsInteger,
  DecDecimalPlaces,
  DecShiftedBy,
  DecToFraction,
  // Result of neverthrow (sema_result.cpp). ok(v) and err(e) take their
  // value; safeTry the generator, which CodeGen calls at once.
  ResultOk,
  ResultErr,
  SafeTry,
  ResultCombine,              // Result.combine(results)
  ResultCombineWithAllErrors, // Result.combineWithAllErrors(results)
  // Methods of Result; the receiver is the object of the callee MemberExpr.
  ResultIsOk,
  ResultIsErr,
  ResultMap,
  ResultMapErr,
  ResultAndThen,
  ResultOrElse,
  ResultMatch,
  ResultUnwrapOr,
  ResultAndTee,
  ResultOrTee,
  ResultAndThrough,
  // Result.fromThrowable(f, toError?): a function that calls f and gives
  // what it returns as an Ok, and what it throws as an Err.
  ResultFromThrowable,
  // _unsafeUnwrap() and _unsafeUnwrapErr(), which throw an Error when the
  // Result holds the other half.
  ResultUnsafeUnwrap,
  ResultUnsafeUnwrapErr,
  // Errors (docs/errors.md): reading `e.stack`, and `e.toString()`; the
  // receiver is the object of the callee MemberExpr.
  ErrorStack,
  ErrorToString,
  // Regular expressions (sema_regex.cpp): `new RegExp` of a pattern known
  // only when the program runs, RegExp.escape, the methods and properties of
  // a regex, whose receiver is the object of the callee MemberExpr (reads of
  // properties become calls), and the methods of strings that take a regex
  // as the first argument.
  RegExpNew,      // pattern, flags or nothing
  RegExpEscape,
  RegExpTest,
  RegExpExec,
  RegExpToString, // also String(re) and `${re}`, of the first argument
  RegExpSource,
  RegExpFlags,
  RegExpFlag,     // re.global and the like: the flag is the int argument
  StrMatch,       // without g: the first match
  StrMatchGlobal, // with g: the matched texts
  StrMatchAll,
  StrSearch,
  StrReplaceRegex,   // replace or replaceAll with a regex and a string
  StrReplaceRegexFn, // with a function
  StrSplitRegex,
  // JSON and Json values (sema_json.cpp). Reading and writing keys of a Json
  // are member and index expressions of its type.
  JsonParse,     // JSON.parse(text): a Json
  JsonParseAs,   // JSON.parse<T>(text): a T, checked
  FromJson,      // fromJson<T>(j)
  JsonStringify, // JSON.stringify(value, replacer, space): replacer is dropped
  EmbedJson,     // embedJson(path): the arguments are the file's text and its path
  ArrayIsArray,  // Array.isArray(x)
  // Dates (sema_date.cpp, docs/date.md). The numbers of the constructors and
  // of the functions of Date are doubles. The receiver of a method is the
  // object of the callee MemberExpr; a getter or a setter takes the field
  // first, as an int literal (TZ_DATE_* of runtime/tzrt.h).
  WeakRefNew,        // new WeakRef(object)
  WeakRefDeref,      // w.deref(): the object, or undefined once it died
  DateNew,           // new Date() (now) and new Date(ms)
  DateCopy,          // new Date(date): the date
  DateFromString,    // new Date(s): the string
  DateFromFields,    // new Date(y, m, d?, h?, min?, s?, ms?), in local time
  DateUTC,           // Date.UTC(y, m?, ...): a number
  DateParse,         // Date.parse(s): a number
  DateText,          // Date(): the text of now, as toString() gives it
  DateGetTime,       // getTime() and valueOf(); also +d, Number(d), `a - b`, `a < b`
  DateGetUTC,        // getUTCFullYear() and the others: the field
  DateGetLocal,      // getFullYear(), ..., getTimezoneOffset(): the field
  DateSetTime,       // setTime(ms)
  DateSetUTC,        // setUTCHours(h, m?, ...) and the others: the field, the values
  DateSetLocal,
  DateToISOString,
  DateToJSON,
  DateToUTCString,
  DateToLocalString, // toString(), toDateString(), toTimeString(): the mode
  DateToLocale,      // toLocaleString() and the like: the kind, the locale?, the options?
  // Intl.DateTimeFormat (docs/date.md, section 3): new takes the locale and
  // the options, as toLocaleString does; format(d) and formatToParts(d) the
  // date, of the formatter that is the receiver.
  DtfNew,
  DtfFormat,
  DtfFormatToParts,
  DtfResolvedOptions,
  // The rest of Intl (sema_intl.cpp, docs/intl.md). The receiver of a method
  // is the object of the callee MemberExpr. The locale and the options, each
  // optional, are the last arguments where there are any.
  IntlNew,              // the kind (TZ_INTL_* of runtime/intl.h, an int literal), locale?, options?
  IntlSupportedLocales, // the name of the method (a string literal), locales, options?
  IntlCanonicalLocales, // locales?: a string, a string[] or undefined
  IntlResolvedOptions,
  IntlBound,            // `nf.format` and the like as a value: a function bound to the receiver
  NumToLocale,          // x.toLocaleString(locale?, options?) of a number or a decimal
  NfFormat,             // the number: an int, a long, a double or a decimal
  NfFormatToParts,
  CollatorCompare,      // a, b
  StrLocaleCompare,     // that, locale?, options?
  StrNormalize,         // form?
  PrSelect,             // the number
  RtfFormat,            // value (a double), unit
  RtfFormatToParts,
  LfFormat,             // the list
  LfFormatToParts,
  DnOf,                 // the code: string | undefined
  SegSegment,           // the text
  SegmentsArray,        // the segments being iterated, as an array
  SegmentsContaining,   // index?
  // The built-in modules (sema_node.cpp, docs/io.md). The functions of
  // node:path take the style first, an int literal: 1 for win32, 0 for posix.
  PathJoin,    // style, the paths as one array
  PathResolve, // style, the paths as one array
  PathNormalize,
  PathIsAbsolute,
  PathDirname,
  PathBasename, // style, path, suffix or nothing
  PathExtname,
  PathRelative,
  PathParse,
  PathFormat, // style, the object
  PathToNamespacedPath,
  // node:os. OsString and OsNumber take which one (an int literal, see
  // runtime/os.c).
  OsString,
  OsNumber,
  OsAvailableParallelism,
  OsCpus,
  OsLoadavg,
  OsUserInfo,
  // readLine(question?) of tz:io.
  ReadLine,
  // node:fs. The path (or the fd, an int, of readFileSync, writeFileSync and
  // appendFileSync) first, then the options Sema took from the call, in the
  // order of the runtime function (see codegen_node.cpp).
  FsReadFile,   // path or fd, flag
  FsWriteFile,  // path or fd, data, flag, mode
  FsExists,     // path
  FsAccess,     // path, mode
  FsChmod,      // path, mode
  FsTruncate,   // path, length
  FsStat,       // path, follow links (boolean), throwIfNoEntry (boolean)
  FsReaddir,    // path, recursive; a Dirent[] or a string[]
  FsMkdir,      // path, recursive, mode
  FsMkdtemp,    // prefix
  FsRmdir,      // path
  FsUnlink,     // path
  FsRm,         // path, recursive, force
  FsRename,     // from, to
  FsCopyFile,   // from, to, mode
  FsRealpath,   // path
  // isFile() and the other tests of Stats and Dirent: the receiver is the
  // object of the callee MemberExpr, the kind of file (S_IFREG...) the int
  // argument.
  FsIsKind,
  // `st.mtime` and the other dates of a Stats: the date of the field
  // `mtimeMs`, rounded to the millisecond, as Node makes it. The receiver is
  // the object of the callee MemberExpr.
  FsStatsDate,
  // node:fs/promises (docs/io.md, step 4): those of node:fs above, with the
  // same arguments, that give a Promise; readFile, writeFile and appendFile
  // take an AbortSignal (or undefined) last.
  FspReadFile,
  FspWriteFile,
  FspAccess,
  FspChmod,
  FspTruncate,
  FspStat,
  FspReaddir,
  FspMkdir,
  FspMkdtemp,
  FspRmdir,
  FspUnlink,
  FspRm,
  FspRename,
  FspCopyFile,
  FspRealpath,
  FspOpen, // path, flags, mode: a FileHandle
  // The methods of a FileHandle, whose receiver is the object of the callee
  // MemberExpr; `fh.fd` is a call too.
  FhFd,
  FhClose,
  FhReadFile,  // signal
  FhWriteFile, // data, signal; appendFile too
  FhWrite,     // data, position (-1 for none)
  FhStat,
  FhTruncate, // length
  FhSync,     // sync() and datasync()
  // node:readline and node:readline/promises over stdin. createInterface
  // takes whether the questions are written (to stdout, 1, or stderr, 2;
  // 0 for none) and the signal; the methods take their receiver from the
  // callee MemberExpr.
  RlCreate,
  RlQuestion,         // query, signal: a Promise<string>
  RlQuestionCallback, // query, callback
  RlClose,
  // The synchronous functions of node:child_process (sema_child.cpp,
  // docs/process.md). Sema gives each call the same arguments, in the order
  // of ChildArg: the program and its arguments, the options, then int
  // literals for what decides the types (stdio, encoding).
  ChildSpawnSync,
  ChildExecFileSync,
  ChildExecSync,
  // Dictionaries (sema_dict.cpp): Record, Map and Set. The receiver is the
  // object of the callee MemberExpr; reading and writing the keys of a
  // Record, `delete r[k]` and `k in r` become such calls too.
  MapGet,           // m.get(k) and r[k]: the value, or undefined
  MapSet,           // m.set(k, v): the map
  MapHas,           // m.has(k), s.has(v) and `k in r`
  MapDelete,        // m.delete(k), s.delete(v) and `delete r[k]`
  MapClear,
  MapSize,          // m.size and s.size
  SetAdd,           // s.add(v): the set
  RecordSet,        // `r[k] = v`: the value
  RecordNullishSet, // `r[k] ??= v`: the value the key has after it
  MapForEach,       // forEach(f): f(value, key, map), or for a Set f(v, v, set)
  // The iterators of a Map: keys, values and [key, value] entries; of a
  // Set, its elements (values() and keys()) and [v, v] entries.
  MapKeys,
  MapValues,
  MapEntries,
  SetValues,
  SetEntries,
  // The methods of ES2025 that make a Set of two (the other one, a Set or a
  // Map, is the argument), and those that compare them.
  SetUnion,
  SetIntersection,
  SetDifference,
  SetSymmetricDifference,
  SetIsSubsetOf,
  SetIsSupersetOf,
  SetIsDisjointFrom,
  // A new dictionary of the call's type: `new Map(entries)`, `new Set(values)`
  // and Object.fromEntries(entries), from what the argument gives, if any;
  // `{ ...r }` copies.
  DictNew,
  DictGroupBy,      // Map.groupBy(items, f) and Object.groupBy(items, f)
  EnvMap,           // process.env as a whole, a Record<string, string>
  // Promises (sema_async.cpp, docs/async.md). `new Promise(executor)`; the
  // methods, whose receiver is the object of the callee MemberExpr; the
  // functions of Promise, which take the value or the iterable of promises.
  PromiseNew,
  PromiseThen,    // then(onFulfilled, onRejected?): both may be undefined
  PromiseCatch,   // catch(onRejected)
  PromiseFinally, // finally(onFinally)
  PromiseResolve, // Promise.resolve(value?)
  PromiseReject,  // Promise.reject(error)
  PromiseAll,
  PromiseAllSettled,
  PromiseRace,
  PromiseAny,
  PromiseWithResolvers,
  // Timers and the queues: the callback first, then the delay.
  SetTimeout,
  SetInterval,
  SetImmediate,
  ClearTimeout,   // clearTimeout and clearInterval
  ClearImmediate,
  QueueMicrotask,
  NextTick,       // process.nextTick(callback)
  // Methods of a Timeout or an Immediate: ref(), unref(), hasRef(),
  // refresh().
  TimerRef,
  TimerUnref,
  TimerHasRef,
  TimerRefresh,
  // ResultAsync of neverthrow (docs/errors.md): a Promise of a Result. The
  // functions take the Promise and the function that makes the error; the
  // methods, whose receiver is the object of the callee MemberExpr, their
  // callbacks.
  ResultAsyncFromPromise,     // fromPromise(promise, toError)
  ResultAsyncFromSafePromise, // fromSafePromise(promise)
  ResultAsyncFromThrowable,   // ResultAsync.fromThrowable(f, toError?): a function
  ResultAsyncMap,
  ResultAsyncMapErr,
  ResultAsyncAndThen,
  ResultAsyncOrElse,
  ResultAsyncMatch,
  ResultAsyncUnwrapOr,
  ResultAsyncAndTee,
  ResultAsyncOrTee,
  // The parts CodeGen adds: the Promise a callback gave settles the
  // ResultAsync as an Ok or an Err of its value; after the callback of a
  // tee, the Result kept goes on.
  ResultAsyncWrapOk,
  ResultAsyncWrapErr,
  ResultAsyncTeeDone,
  // node:timers/promises (docs/async.md, step 2): setTimeout(delay, value,
  // signal, ref) and setImmediate(value, signal, ref) give a Promise of the
  // value, setInterval(delay, value, signal, ref) an async iterator of it;
  // scheduler.wait(delay, signal) and scheduler.yield().
  TimersSetTimeout,
  TimersSetImmediate,
  TimersSetInterval,
  // Binary data (sema_bytes.cpp, docs/bytes.md). The kind of what a call
  // makes is that of its type; the receiver of a method is the object of the
  // callee MemberExpr. Positions and counts are longs, and an optional one
  // left out is not in args (codegen_bytes.cpp counts them); a choice among
  // variants is an int literal first.
  BytesNew,        // new Uint8Array(n), Buffer.alloc(n) and allocUnsafe(n): n, zeroed (bool)
  BytesFromArray,  // new Uint8Array(array or iterator), Buffer.from(array): the source
  BytesFromBytes,  // new Uint16Array(typedArray), Buffer.from(bytes): a copy, converted
  BytesOf,         // new Uint8Array([1, 2]) and Uint8Array.of(1, 2): the values
  BytesView,       // new Uint8Array(buffer, offset?, length?), Buffer.from(buffer, ...)
  BytesByteLength, // of a typed array, an ArrayBuffer or a DataView
  BytesByteOffset,
  BytesBuffer,     // `.buffer` of a typed array or a DataView
  BytesAt,
  BytesIndexOf,    // mode (0 indexOf, 1 lastIndexOf, 2 includes), value, from?
  BytesJoin,       // separator?; also toString() and String(u)
  BytesReverse,
  BytesSort,
  BytesFill,       // value, start?, end?
  BytesSet,        // source (array or typed array), offset?
  BytesSubarray,   // start?, end?
  BytesSlice,      // start?, end?
  BytesCopyWithin, // target, start, end?
  BytesToReversed,
  BytesToSorted,
  BytesWith,       // index, value
  ArrayBufferNew,  // length
  ArrayBufferSlice,
  ArrayBufferIsView,
  DataViewNew,     // buffer, offset?, length?
  DataViewGet,     // kind (TZ_BYTES_*), offset, littleEndian?
  DataViewSet,     // kind, offset, value, littleEndian?
  BufferFromString,  // string, encoding (an int literal, or a string)
  BufferAllocFill,   // size, fill (number, string or bytes), encoding?
  BufferConcat,      // list, totalLength?
  BufferByteLength,  // string, encoding
  BufferCompare,     // Buffer.compare(a, b)
  BufferCompareTo,   // buf.compare(target, targetStart?, targetEnd?, sourceStart?, sourceEnd?)
  BufferEquals,      // other
  BufferIsBuffer,    // x, whose type tells
  BufferIsEncoding,  // name
  BufferToString,    // encoding?, start?, end?
  BufferWrite,       // string, offset?, length?, encoding?
  BufferIndexOf,     // mode, value (number, string or bytes), byteOffset?, encoding?
  BufferCopy,        // target, targetStart?, sourceStart?, sourceEnd?
  BufferFill,        // value (number, string or bytes), offset?, end?, encoding?
  BufferRead,        // kind (TZ_BYTES_*), big-endian (bool), offset?
  BufferWriteNumber, // kind, big-endian, value, offset?
  BufferReadVar,     // signed (bool), big-endian, offset, byteLength
  BufferWriteVar,    // signed, big-endian, value, offset, byteLength
  BufferSwap,        // size (2, 4 or 8)
  BufferToJSON,
  TextEncoderNew,
  TextEncode,        // string?
  TextEncodeInto,    // string, destination
  TextDecoderNew,    // label?, options?: the label is an int (the encoding Sema found) or a string
  TextDecode,        // input?
  TextDecoderInfo,   // 0 encoding, 1 fatal, 2 ignoreBOM
  EmbedBytes,        // embedBytes(path): the bytes (one per code unit) and the path
  // node:crypto (sema_crypto.cpp, docs/crypto.md): a call of a function of
  // runtime/crypto*.c, whose number (CryptoFn of sema/crypto_api.h) is the
  // first argument, an int literal, and the receiver of a method the second.
  Crypto,
  // generateKeyPairSync: the call that makes the private key, then for the
  // public and the private key the format of its encoding (-1 for a
  // KeyObject), the options given, the type, the cipher and the passphrase.
  CryptoKeyPair,
  // `t.join(sep?)` of a tuple, as a list of parameters (docs/generics.md,
  // section 9) is one: the receiver is the object of the callee MemberExpr.
  TupleJoin,
  // Metadata (docs/decorators.md, section 3): Reflect.defineMetadata and
  // the others take the key (a string; an empty one for the key
  // CallExpr::metaKey), for define the value as an unknown, the target as an
  // object, and the property as a `string | undefined`.
  MetaDefine,
  MetaGet,
  MetaGetOwn,
  MetaHas,
  MetaHasOwn,
  MetaDelete,
  // The intrinsics of the built-in modules written in tz (src/lib, see
  // sema_events.cpp) that need the compiler, as they take a type:
  // `__tz_listener<F>(u)`, the function an unknown holds, as an F;
  // `__tz_as<C>(o)`, the object o as an instance of the class C, or undefined
  // if it is not one. The others are declared there, `function __tz_x(...)`
  // without a body, calls of the runtime's tz_x (sema_library.cpp). Then
  // what Sema makes of the `signal` of a call (signalOption): the runtime's
  // side of the AbortSignal, or undefined.
  LibListener,
  LibAs,
  // node:stream (src/sema/sema_stream.cpp): `__tz_chunk<T>(u)`, the T the
  // unknown u holds; `__tz_isChunk<T>(u)`, whether it holds one;
  // `__tz_chunkKind<T>()` and `__tz_typeName<T>()`, constants of each copy;
  // `__tz_overrides(o, "m")`, whether the class of the object o replaces the
  // method m of the class the code knows it by.
  LibChunk,
  LibIsChunk,
  LibChunkKind,
  LibTypeName,
  LibOverrides,
  // The I/O of the streams of node:fs and process (src/lib/fs_stream.ts),
  // each a Promise but the last: `__tz_fsOpen(path, flags, mode)` of the
  // descriptor, `__tz_fsRead(fd, buffer, offset, length, position)` and
  // `__tz_fsWrite(...)` of the bytes moved, `__tz_fsClose(fd, fsync)`,
  // `__tz_stdinChunk(max)` of the next Buffer of stdin (undefined at its
  // end), and `__tz_stdWrite(fd, buffer)`, which writes to stdout or stderr.
  LibFsOpen,
  LibFsRead,
  LibFsWrite,
  LibFsClose,
  LibStdinChunk,
  LibStdWrite,
  // The parallel runtime (runtime/parallel.c), for code of the built-in
  // modules that the analysis of src/sema/parallel.h does not walk:
  // `await __tz_ahead(p)`, an await after which the piece may run ahead of
  // its turn, which the code itself guarantees, and `__tz_order()`, where it
  // waits for its turn before it touches shared data.
  LibAhead,
  LibOrder,
  LibNativeSignal,
  // Reflection (docs/decorators.md, section 6), through the descriptions of
  // the classes: the name of a class known only when the program runs (its
  // object); Reflect.get(object, name), Reflect.apply(f, this, args) and
  // Reflect.construct(cls, args), with unknowns for values; fromJson(cls,
  // j), the object a class makes from a Json through its constructor.
  ClassName,
  ReflectGet,
  ReflectApply,
  ReflectConstruct,
  FromJsonClass,
  // Reflect.get(object, "m") of an object of a class known when compiling,
  // whose method `method` no class down its chain declares again: the
  // function that takes its `this`, as an unknown, without the description of
  // the class. The object is evaluated and dropped.
  ReflectMethod,
};

inline bool isLibBuiltin(Builtin builtin) {
  return builtin >= Builtin::LibListener && builtin <= Builtin::LibNativeSignal;
}

inline bool isCryptoBuiltin(Builtin builtin) {
  return builtin == Builtin::Crypto || builtin == Builtin::CryptoKeyPair;
}

inline bool isMetaBuiltin(Builtin builtin) {
  return builtin >= Builtin::MetaDefine && builtin <= Builtin::MetaDelete;
}

inline bool isDictBuiltin(Builtin builtin) {
  return builtin >= Builtin::MapGet && builtin <= Builtin::EnvMap;
}

// The iterators of dictionaries.
inline bool isDictIterator(Builtin builtin) {
  return builtin >= Builtin::MapKeys && builtin <= Builtin::SetEntries;
}

inline bool isNodeBuiltin(Builtin builtin) {
  return builtin >= Builtin::PathJoin && builtin <= Builtin::RlClose;
}

// The functions of node:fs/promises and the methods of FileHandle, which
// give a Promise.
inline bool isFspBuiltin(Builtin builtin) {
  return builtin >= Builtin::FspReadFile && builtin <= Builtin::FhSync;
}

// spawnSync, execFileSync and execSync of node:child_process.
inline bool isChildBuiltin(Builtin builtin) {
  return builtin >= Builtin::ChildSpawnSync && builtin <= Builtin::ChildExecSync;
}

// The arguments Sema gives a call of one of them. The program (the command
// of execSync) is a string and its arguments a string[]; each option is
// `T | undefined`, undefined when the call does not give it: input a string
// or a view of bytes, killSignal a string or an int, shell a boolean or a
// string. The int literals last: what each of stdin, stdout and stderr is
// (ChildPipe, ChildIgnore, or ChildInherit plus the fd the child inherits),
// whether the call gave `stdio`, and the encoding of the output (ChildBytes
// for Buffers, or one plus the number of a Buffer encoding).
enum ChildArg : unsigned {
  ChildFile,
  ChildArgs,
  ChildCwd,
  ChildEnv,
  ChildInput,
  ChildTimeout,
  ChildKillSignal,
  ChildMaxBuffer,
  ChildShell,
  ChildWindowsHide,
  ChildVerbatim,
  ChildDetached,
  ChildArgv0,
  ChildUid,
  ChildGid,
  ChildStdin,
  ChildStdout,
  ChildStderr,
  ChildStdioGiven,
  ChildEncoding,
  ChildArgCount
};
inline constexpr unsigned ChildPipe = 0, ChildIgnore = 1, ChildInherit = 2, ChildBytes = 0;

// The functions of node:fs, which throw errors of the system.
inline bool isFsBuiltin(Builtin builtin) {
  return builtin >= Builtin::FsReadFile && builtin <= Builtin::FsRealpath &&
         builtin != Builtin::FsExists;
}

inline bool isPromiseBuiltin(Builtin builtin) {
  return builtin >= Builtin::PromiseNew && builtin <= Builtin::PromiseWithResolvers;
}

inline bool isAsyncBuiltin(Builtin builtin) {
  return builtin >= Builtin::PromiseNew && builtin <= Builtin::TimersSetInterval;
}

inline bool isResultAsyncMethod(Builtin builtin) {
  return builtin >= Builtin::ResultAsyncMap && builtin <= Builtin::ResultAsyncOrTee;
}

inline bool isJsonBuiltin(Builtin builtin) {
  return builtin >= Builtin::JsonParse && builtin <= Builtin::ArrayIsArray;
}

inline bool isDateBuiltin(Builtin builtin) {
  return builtin >= Builtin::DateNew && builtin <= Builtin::DtfResolvedOptions;
}

inline bool isBytesBuiltin(Builtin builtin) {
  return builtin >= Builtin::BytesNew && builtin <= Builtin::EmbedBytes;
}

inline bool isIntlBuiltin(Builtin builtin) {
  return builtin >= Builtin::IntlNew && builtin <= Builtin::SegmentsContaining;
}

inline bool isMathBuiltin(Builtin builtin) {
  return builtin >= Builtin::MathFloor && builtin <= Builtin::MathRandom;
}

inline bool isDecimalBuiltin(Builtin builtin) {
  return builtin >= Builtin::DecimalOf && builtin <= Builtin::DecToFraction;
}

inline bool isResultBuiltin(Builtin builtin) {
  return builtin >= Builtin::ResultOk && builtin <= Builtin::ResultUnsafeUnwrapErr;
}

// The methods of Result that call a function given to them.
inline bool isResultCallback(Builtin builtin) {
  return builtin >= Builtin::ResultMap && builtin <= Builtin::ResultAndThrough &&
         builtin != Builtin::ResultUnwrapOr;
}

inline bool isRegexBuiltin(Builtin builtin) {
  return builtin >= Builtin::RegExpNew && builtin <= Builtin::StrSplitRegex;
}

inline bool isProcessBuiltin(Builtin builtin) {
  return builtin >= Builtin::ProcessArgv && builtin <= Builtin::PerformanceNow;
}

inline bool isRoundingBuiltin(Builtin builtin) {
  return builtin >= Builtin::MathFloor && builtin <= Builtin::MathTrunc;
}

// A helper that makes a stage of a lazy chain: map, filter, take, drop,
// flatMap.
inline bool isIteratorStage(Builtin builtin) {
  return builtin >= Builtin::IterMap && builtin <= Builtin::IterFlatMap;
}

// A helper that consumes an iterator: toArray, forEach, reduce, some, every,
// find.
inline bool isIteratorSink(Builtin builtin) {
  return builtin >= Builtin::IterToArray && builtin <= Builtin::IterFind;
}

class CallExpr : public Expr {
public:
  CallExpr(llvm::SMLoc loc, Expr *callee, std::vector<Expr *> args)
      : Expr(Kind::Call, loc), callee(callee), args(std::move(args)) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Call; }

  Expr *callee;
  std::vector<Expr *> args;
  // `f(...a)`: the arguments that are spread, by position; empty if none are.
  std::vector<bool> spread;
  bool spreads(size_t i) const { return i < spread.size() && spread[i]; }
  bool hasSpread() const { return llvm::is_contained(spread, true); }
  bool optional = false; // `callee?.(args)`
  // A tagged template, tag`a${x}b`: the first argument is the array of the
  // texts, ["a", "b"], and the values follow.
  bool tagged = false;
  // Set by Sema: exactly one of these describes the callee.
  FuncSymbol *func = nullptr;
  Builtin builtin = Builtin::None;
  // Set by Sema for charCodeAt, at, pop and shift: the position may be out of
  // range (or the array empty), so the result type admits that case.
  bool mayBeOutOfRange = false;
  // Set by Sema for a call of a method of a class: the variant called. The
  // object it is called on is the object of the callee MemberExpr.
  FuncSymbol *method = nullptr;
  // The method is overridden in some class the object may belong to: the
  // call goes through the method table of the object's class, at the slot of
  // `method` (FunctionDecl::slot). Otherwise the call is direct.
  bool virtualCall = false;
  // A method of an interface with several signatures: which one is called
  // (see Overload).
  int signature = -1;
  // `super(args)` in a constructor, which runs the constructor `method` of
  // the class extended (null if no class up the chain has one) and then the
  // initializers of the fields; or `super.m(args)`, a direct call of the
  // method `method` of that class.
  bool superCall = false;
  // Set by Sema for a stage of a lazy chain (`xs.values().map(f)`) that a
  // consumer takes apart where it is written: the chain runs as one loop
  // there, and its callbacks are called only during it (see
  // Sema::markFused).
  bool fused = false;
  // Set by Sema for a call that takes a regex known when compiling (a
  // literal, or a variable that holds one): where its groups are.
  const RegexLitExpr *regex = nullptr;
  // The Object.keys that `for...in` walks: of a Json that is null or
  // undefined, no keys rather than a TypeError, as in JavaScript.
  bool forIn = false;
  // Set by Sema for a method called on a union of objects that no type of
  // them all declares (see Sema::checkUnionMethodCall): for each member, the
  // method that runs, through the method table with `virtualCall`; or null
  // for a function in a field.
  struct UnionMethod {
    const Type *member;
    FuncSymbol *method;
    bool virtualCall;
  };
  std::vector<UnionMethod> unionMethods;
  // Metadata (Builtin::MetaDefine...): the object key, a declaration Sema
  // knows (a function, a class, a constant), or null for a string key; and
  // a record `design:*` the compiler writes.
  const void *metaKey = nullptr;
  bool designRecord = false;
};

// `new Array<T>(n)`, `new Array<T>()`, or `new C(args)` of a class.
class NewExpr : public Expr {
public:
  NewExpr(llvm::SMLoc loc, llvm::StringRef name, std::vector<TypeRef> typeArgs,
          std::vector<Expr *> args)
      : Expr(Kind::New, loc), name(name), typeArgs(std::move(typeArgs)), args(std::move(args)) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::New; }

  llvm::StringRef name;
  // `new Intl.DateTimeFormat()`: "DateTimeFormat", with "Intl" in name.
  llvm::StringRef member;
  std::vector<TypeRef> typeArgs;
  std::vector<Expr *> args;
  // Set by Sema for a class: the class, and the call of the constructor
  // variant chosen (its `func`, null when the class has no constructor),
  // which holds the checked arguments.
  ClassDecl *cls = nullptr;
  CallExpr *ctor = nullptr;
  // `new route.cls()`, `new (Mixin(User))()`: a class as a value
  // (docs/decorators.md, sections 5 and 6), evaluated first. The parser sets
  // it for anything but a name, Sema for a name of a variable too. Of a
  // class known when compiling it is evaluated and dropped, and `cls` is
  // made; of a `Constructor<T>` (`reflect` set) the class makes the object
  // through its description, from `args` as unknowns.
  Expr *callee = nullptr;
  bool reflect = false;
};

class ConditionalExpr : public Expr {
public:
  ConditionalExpr(llvm::SMLoc loc, Expr *cond, Expr *thenExpr, Expr *elseExpr)
      : Expr(Kind::Conditional, loc), cond(cond), thenExpr(thenExpr), elseExpr(elseExpr) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Conditional; }

  Expr *cond;
  Expr *thenExpr;
  Expr *elseExpr;
};

// A conversion inserted by Sema: a numeric widening, ToInt32 for an operand
// of a bitwise operator, or turning a value into a string for `+` and
// template literals. `target` is set only for `x as T` in the source, which
// Sema rejects (see Sema::checkCast).
class CastExpr : public Expr {
public:
  CastExpr(llvm::SMLoc loc, Expr *operand, std::optional<TypeRef> target)
      : Expr(Kind::Cast, loc), operand(operand), target(target) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Cast; }

  bool isImplicit() const { return !target; }

  Expr *operand;
  std::optional<TypeRef> target;
  // A link of an optional chain, `operand?.x`: from `T | undefined` to T,
  // where undefined ends the whole chain (see OptionalChainExpr).
  bool shortCircuit = false;
  // To a string, the text of a value of a union (`${x}`, String(x)), which
  // may hold another member than the string: not a narrowing.
  bool text = false;
  // A function value given a type whose result its own converts to, `(r:
  // Request) => Promise<Response>` where `(r: Request) => Response |
  // Promise<Response>` is expected: a function that calls it and converts
  // what it gives (see Sema::adaptResult).
  bool adaptsResult = false;
};

// `a, b, c`: evaluates each in order; the value is the last one's.
class SequenceExpr : public Expr {
public:
  SequenceExpr(llvm::SMLoc loc, std::vector<Expr *> exprs)
      : Expr(Kind::Sequence, loc), exprs(std::move(exprs)) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Sequence; }

  std::vector<Expr *> exprs;
};

// `a?.b.c`, `a?.[i]`, `s?.trim()`: the member accesses, indexing and calls
// after `?.` form the chain, and if the object before any `?.` in it is
// undefined, the whole chain is undefined and the rest is not evaluated. Sema
// marks those objects with CastExpr::shortCircuit.
class OptionalChainExpr : public Expr {
public:
  OptionalChainExpr(llvm::SMLoc loc, Expr *chain) : Expr(Kind::OptionalChain, loc), chain(chain) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::OptionalChain; }

  Expr *chain;
  // Set by Sema: some object in the chain may be undefined.
  bool mayShortCircuit = false;
};

// An arrow function or a function expression: a function value, which
// captures the variables around it that its body uses (see
// FunctionDecl::captures).
class FunctionExpr : public Expr {
public:
  FunctionExpr(llvm::SMLoc loc, FunctionDecl *fn) : Expr(Kind::Function, loc), fn(fn) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Function; }

  FunctionDecl *fn;
};

// `class extends Base { ... }` as an expression (docs/decorators.md, section
// 5): its value is the class, which Sema makes when it checks the
// expression, once for each copy of a generic function that has it. Its
// static fields get their values each time it is evaluated.
class ClassExpr : public Expr {
public:
  ClassExpr(llvm::SMLoc loc, ClassDecl *decl) : Expr(Kind::Class, loc), decl(decl) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Class; }

  ClassDecl *decl;
};

// `yield value` (or a bare `yield`) in a generator: gives the value to
// whoever iterates, and is what `next(x)` sends back, if anything.
// `yield* iterable` gives each value of an array, a string or another
// iterator in turn, and is what that iterator returns.
class YieldExpr : public Expr {
public:
  YieldExpr(llvm::SMLoc loc, Expr *value, bool delegate)
      : Expr(Kind::Yield, loc), value(value), delegate(delegate) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Yield; }

  Expr *value; // null for a bare `yield`
  bool delegate;
  // Set by Sema. The statement drops the value of the `yield`, so what
  // next() sends does not matter. For `yield*`, the type of the values the
  // iterable gives, which each become the generator's.
  bool discarded = false;
  const Type *element = nullptr;
};

// `await value` in an async function or in top-level code (docs/async.md):
// the code waits for the Promise `value` to settle and goes on with its
// value, or throws its error; a value that is no Promise it just gives back,
// after a turn of the microtask queue, as JavaScript does.
class AwaitExpr : public Expr {
public:
  AwaitExpr(llvm::SMLoc loc, Expr *value) : Expr(Kind::Await, loc), value(value) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Await; }

  Expr *value;
};

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

class Stmt : public Node {
public:
  enum class Kind {
    Block, VarDecl, Function, Expr, If, While, DoWhile, For, ForOf, Switch, Labeled, Return,
    Break, Continue, Empty, TypeAlias, Enum, Debugger, Class, Throw, Try,
  };

  Kind kind() const { return kind_; }
  llvm::SMLoc loc() const { return loc_; }

protected:
  Stmt(Kind kind, llvm::SMLoc loc) : kind_(kind), loc_(loc) {}

private:
  Kind kind_;
  llvm::SMLoc loc_;
};

struct NestedGroup;

struct BlockStmt : Stmt {
  explicit BlockStmt(llvm::SMLoc loc) : Stmt(Kind::Block, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Block; }

  std::vector<Stmt *> body;
  // Set by Sema: the functions declared in this block, if any.
  NestedGroup *group = nullptr;
};

struct VarDeclarator {
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::optional<TypeRef> typeAnn;
  Expr *init = nullptr;
  VarSymbol *sym = nullptr; // set by Sema
  // A hidden variable holding the array a pattern destructures (see Parser),
  // or with `objectPattern` the object.
  bool destructured = false;
  bool objectPattern = false;
  // Set by Sema for `const f = genericFunction`: f is another name for the
  // function, not a variable, and sym stays null.
  bool alias = false;
  // An array pattern: how many elements it names, and whether it ends with
  // `...rest`. Destructuring an iterator takes only those from it.
  size_t patternLength = 0;
  bool patternRest = false;
};

// `let a = 1, b = 2` or `const c: double = 3`.
struct VarDeclStmt : Stmt {
  VarDeclStmt(llvm::SMLoc loc, bool isConst) : Stmt(Kind::VarDecl, loc), isConst(isConst) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::VarDecl; }

  bool isConst;
  std::vector<VarDeclarator> decls;
};

// Who may use a member of a class; checked when compiling. `#x` is private.
enum class Access { Public, Protected, Private };

// `@Log` or `@Table("users")` before a class, a member of one or a parameter
// of a method or a constructor (docs/decorators.md).
struct Decorator {
  llvm::SMLoc loc; // of the `@`
  Expr *expr;      // `Log`, `Table("users")`, `(make())`
};

struct Param {
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::optional<TypeRef> type;
  bool optional = false;        // `name?: T`
  bool rest = false;            // `...name: T[]`, the last parameter
  Expr *defaultValue = nullptr; // `name: T = value`
  // A parameter property, `constructor(public x: int)`: a field of the class
  // too, which the constructor sets first (the parser adds `this.x = x`).
  bool property = false;
  bool readonly = false;
  Access access = Access::Public;
  // A parameter written as a destructuring pattern, `{ x, y }: Point`, has a
  // hidden name, and the body starts by destructuring it; this is the
  // pattern as written, for messages.
  llvm::StringRef pattern;
  std::vector<Decorator> decorators;
  llvm::StringRef label() const { return pattern.empty() ? name : pattern; }
  // Set by Sema: the type of the parameter inside the function, and its
  // variable.
  const Type *declaredType = nullptr;
  VarSymbol *sym = nullptr;
};

// A function: a declaration, or the function of a FunctionExpr. A function
// nested in another one (or in main's blocks) is a closure: its value carries
// an environment with the variables of the functions around it that it uses.
struct FunctionDecl : Stmt {
  FunctionDecl(llvm::SMLoc loc, llvm::StringRef name) : Stmt(Kind::Function, loc), name(name) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Function; }

  llvm::StringRef name;
  // `function f<T, U>(...)`: a generic function.
  std::vector<TypeParamDecl> typeParams;
  // Set by Sema: what the type parameters stand for while the body is
  // checked. For the generic function itself they are opaque type parameters
  // (Type::Kind::Param); for a copy of it made for some types (an instance,
  // see Sema::instantiate), those types.
  std::vector<const Type *> typeArgs;
  bool isInstance = false;
  std::vector<Param> params;
  std::optional<TypeRef> returnType;
  BlockStmt *body = nullptr;
  // The source text of the declaration, for Sema to parse a fresh copy.
  llvm::StringRef source;
  bool isExpression = false; // an arrow or a function expression
  bool isArrow = false;
  // `function*`: a call creates a generator, an iterator that runs the body
  // up to each `yield` as it is asked for the next value.
  bool isGenerator = false;
  // `async function`, `async () => ...`, `async m() {}`: a call runs the body
  // up to its first `await` and gives a Promise of what it returns.
  bool isAsync = false;
  // Its code awaits `__tz_ahead(p)` (Builtin::LibAhead): a piece of it may
  // run ahead of its turn.
  bool awaitsAhead = false;
  // Set by Sema for the `function*` given to safeTry, which is no generator:
  // it runs once, at the call, and `yield* r` in it takes the value of the
  // Result r or returns its Err (see Sema::checkSafeTry). Sema clears
  // isGenerator then.
  bool safeTry = false;
  // A generic arrow or function expression that a top-level constant names
  // (`const id = <T>(x: T) => x`): Sema makes it a top-level function, like a
  // declaration; it is no longer an expression.
  bool lifted = false;
  // A function expression's own name (`function f() {}`), which its body
  // sees.
  llvm::StringRef ownName;
  // An arrow whose body is an expression: the parser makes it `{ return e }`.
  bool concise = false;
  FuncSymbol *sym = nullptr; // set by Sema
  bool checked = false;      // set by Sema once the body is checked

  // Set by Sema for closures. `parent` is the function around this one (null
  // for main). `captures` are the variables of the functions around it that
  // the body uses, in the order of the environment's fields; the functions
  // declared in one block share an environment (`group`).
  FunctionDecl *parent = nullptr;
  std::vector<VarSymbol *> captures;
  NestedGroup *group = nullptr;
  // The other functions of its group that it uses.
  std::vector<FunctionDecl *> siblings;
  VarSymbol *var = nullptr; // a nested declaration's variable
  // When the closure is created, in the order of declarations (see VarSymbol).
  unsigned creationSeq = 0;
  // Passed straight to a built-in that only calls it during the call, such as
  // map: the environment can live on the stack.
  bool nonEscaping = false;
  bool isClosure() const { return parent || isExpression || group; }

  // Methods (sema_class.cpp): the class, or the object literal (see
  // ClassDecl::literal), the method belongs to. A method that is not static
  // gets the object as `this`, a variable Sema declares in its body.
  ClassDecl *cls = nullptr;
  bool isStatic = false;
  bool isConstructor = false;
  Access access = Access::Public;
  VarSymbol *thisVar = nullptr;
  // Overloads: the position of this variant among the methods of its name.
  unsigned variant = 0;
  bool takesThis() const { return cls && !isStatic; }
  // Inheritance (sema_inherit.cpp). `abstract m(): T` has no body; `override`
  // says that the method replaces one of the class extended.
  bool isAbstract = false;
  bool isOverride = false;
  // Set by Sema: the variant of a class up the chain that this one replaces
  // (the same name and parameters); whether a class down the chain replaces
  // this one; and the slot of the method table that a call through the class
  // goes through, -1 for a method that nothing replaces, which every call
  // calls directly.
  FunctionDecl *overrides = nullptr;
  bool overridden = false;
  int slot = -1;
  // A class that a class expression makes may override it, when the program
  // is checked: every call goes through the method table (see
  // Sema::markLateOverrides).
  bool openToLate = false;
  // A method's decorators (docs/decorators.md), top to bottom. Set by Sema
  // when one of them takes the method's descriptor and may replace what the
  // method does: the hidden global that holds that, a function of the type
  // of the descriptor's `value`, which every call of the method goes through
  // (see codegen_decorate.cpp).
  std::vector<Decorator> decorators;
  VarSymbol *impl = nullptr;
  // A method of an instance of a generic class (see ClassDecl::generic):
  // the method of the generic class it is a copy of, whose signature, with
  // the instance's types, is its own.
  FunctionDecl *generic = nullptr;
  // A rest parameter `...args: L` of an argument list L (a tuple, what `A
  // extends unknown[]` stands for in a copy) is the parameters of L, from
  // `listStart` on, with hidden names; `listName` names them together in
  // the body (see Sema::expandArgumentList). Empty without one.
  llvm::StringRef listName;
  size_t listStart = 0;
  const Type *listType = nullptr;
  // In a built-in module of tz written in tz (Module::builtin): `function
  // __tz_name(...): T` without a body is a call of the runtime's tz_name,
  // which the parser gives an empty body (see Sema::checkExternal and
  // CodeGen::emitExternal). `get x()` and `set x(v)` of a class there are
  // methods named x that reading and assigning `o.x` call, as JavaScript's
  // accessors (see Sema::checkAccessorRead).
  bool external = false;
  enum class Accessor { None, Getter, Setter };
  Accessor accessor = Accessor::None;
  // `@__tz_hidden get x()`: a getter that does not print, as node's classes
  // do not show every property they have.
  bool hiddenGetter = false;
};

// The functions declared in one block. They are created together, before the
// first statement of the block that mentions one of them, and share one
// environment: they call each other (and themselves) without capturing each
// other, so no reference cycle forms.
struct NestedGroup {
  std::vector<FunctionDecl *> members;
  std::vector<VarSymbol *> captures;
  FunctionDecl *owner = nullptr; // the function whose block declares them
  // The statement of the block before which they are created; none if
  // nothing mentions them.
  size_t firstUse = ~size_t(0);
  unsigned creationSeq = 0;
  // Used by Sema while it checks the block: where the block's scope and
  // function are, the names the block declares and the names the functions
  // assign to.
  size_t scopeDepth = 0, functionDepth = 0;
  std::vector<llvm::StringRef> declaredNames, assignedNames;
  bool finished = false;
  // Where the block's own code uses the functions, to find those that run
  // before a variable they use is declared.
  struct Use {
    FunctionDecl *fn;
    unsigned seq;
    llvm::SMLoc loc;
  };
  std::vector<Use> uses;
};

struct ExprStmt : Stmt {
  ExprStmt(llvm::SMLoc loc, Expr *expr) : Stmt(Kind::Expr, loc), expr(expr) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Expr; }

  Expr *expr;
};

struct IfStmt : Stmt {
  IfStmt(llvm::SMLoc loc, Expr *cond, Stmt *thenStmt, Stmt *elseStmt)
      : Stmt(Kind::If, loc), cond(cond), thenStmt(thenStmt), elseStmt(elseStmt) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::If; }

  Expr *cond;
  Stmt *thenStmt;
  Stmt *elseStmt; // may be null
};

struct WhileStmt : Stmt {
  WhileStmt(llvm::SMLoc loc, Expr *cond, Stmt *body) : Stmt(Kind::While, loc), cond(cond), body(body) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::While; }

  Expr *cond;
  Stmt *body;
};

struct DoWhileStmt : Stmt {
  DoWhileStmt(llvm::SMLoc loc, Stmt *body, Expr *cond)
      : Stmt(Kind::DoWhile, loc), body(body), cond(cond) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::DoWhile; }

  Stmt *body;
  Expr *cond;
};

struct ForStmt : Stmt {
  explicit ForStmt(llvm::SMLoc loc) : Stmt(Kind::For, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::For; }

  Stmt *init = nullptr; // VarDeclStmt or ExprStmt
  Expr *cond = nullptr;
  Expr *step = nullptr;
  Stmt *body = nullptr;
};

// `for (const x of iterable) body`: over the elements of an array, the code
// points of a string, or the values of an iterator. With `keys`,
// `for (const k in object) body`: Sema makes the iterable the array of the
// object's keys.
struct ForOfStmt : Stmt {
  explicit ForOfStmt(llvm::SMLoc loc) : Stmt(Kind::ForOf, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::ForOf; }

  bool isConst = false;
  bool keys = false;
  // `for await (const x of iterable)`: over the values of an async iterator,
  // or the elements of an array or an iterator, each awaited.
  bool isAwait = false;
  VarDeclarator binding; // no initializer
  Expr *iterable = nullptr;
  Stmt *body = nullptr;
  // Set by Sema: the loop variable may borrow each element from the array,
  // without reference counting.
  bool borrowElements = false;
};

// `case test:` (or `default:`, which has no test) and the statements after it.
struct SwitchCase {
  llvm::SMLoc loc;
  Expr *test = nullptr;
  std::vector<Stmt *> body;
  // Set by Sema: the type in which the discriminant and the test compare, as
  // with `==`. Null when the test is `undefined`.
  const Type *compareType = nullptr;
};

// `switch (discriminant) { case a: ... default: ... }`. The discriminant is
// evaluated once and compared with the tests in order, with `==`; control
// falls through from one case into the next.
struct SwitchStmt : Stmt {
  SwitchStmt(llvm::SMLoc loc, Expr *discriminant)
      : Stmt(Kind::Switch, loc), discriminant(discriminant) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Switch; }

  Expr *discriminant;
  std::vector<SwitchCase> cases;
  // Set by Sema: the cases handle every value of the literal type the switch
  // tests, so one of them always runs, as with a default.
  bool exhaustive = false;
};

// `label: body`, a target of `break label` and, for a loop, `continue label`.
struct LabeledStmt : Stmt {
  LabeledStmt(llvm::SMLoc loc, llvm::StringRef label, Stmt *body)
      : Stmt(Kind::Labeled, loc), label(label), body(body) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Labeled; }

  llvm::StringRef label;
  Stmt *body;
};

struct ReturnStmt : Stmt {
  ReturnStmt(llvm::SMLoc loc, Expr *value) : Stmt(Kind::Return, loc), value(value) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Return; }

  Expr *value; // may be null
};

struct BreakStmt : Stmt {
  BreakStmt(llvm::SMLoc loc, llvm::StringRef label) : Stmt(Kind::Break, loc), label(label) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Break; }

  llvm::StringRef label; // empty without one
};

struct ContinueStmt : Stmt {
  ContinueStmt(llvm::SMLoc loc, llvm::StringRef label) : Stmt(Kind::Continue, loc), label(label) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Continue; }

  llvm::StringRef label; // empty without one
};

struct EmptyStmt : Stmt {
  explicit EmptyStmt(llvm::SMLoc loc) : Stmt(Kind::Empty, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Empty; }
};

// `throw value`: only an Error (or an instance of a class derived from it)
// can be thrown (docs/errors.md).
struct ThrowStmt : Stmt {
  ThrowStmt(llvm::SMLoc loc, Expr *value) : Stmt(Kind::Throw, loc), value(value) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Throw; }

  Expr *value;
};

// `try { block } catch (e) { handler } finally { finalizer }`. Either clause
// may be left out, not both; `catch { ... }` has no variable (`param.name` is
// empty). The variable is an Error, whatever was thrown.
struct TryStmt : Stmt {
  explicit TryStmt(llvm::SMLoc loc) : Stmt(Kind::Try, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Try; }

  BlockStmt *block = nullptr;
  BlockStmt *handler = nullptr;   // null without `catch`
  BlockStmt *finalizer = nullptr; // null without `finally`
  VarDeclarator param;            // `catch (e)`, or `catch (e: unknown)`; sym set by Sema
};

// `debugger`: stops the program if a debugger is attached, and does nothing
// otherwise.
struct DebuggerStmt : Stmt {
  explicit DebuggerStmt(llvm::SMLoc loc) : Stmt(Kind::Debugger, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Debugger; }
};

// `type Name = T`: another name for a type, visible in the whole block that
// declares it. `interface Name { ... }` is the same for an object type.
struct TypeAliasDecl : Stmt {
  TypeAliasDecl(llvm::SMLoc loc, llvm::StringRef name, TypeRef type)
      : Stmt(Kind::TypeAlias, loc), name(name), type(std::move(type)) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::TypeAlias; }

  llvm::StringRef name;
  // `type Pair<T> = ...`: a generic alias, which names a type for each list
  // of type arguments (see Sema::instantiateAlias).
  std::vector<TypeParamDecl> typeParams;
  TypeRef type;
  // `interface User extends Named, Dated { ... }`: the interfaces whose
  // fields User has too, and which it may be used as.
  std::vector<TypeRef> bases;
  // Set by Sema.
  const Type *resolved = nullptr;
  bool resolving = false; // catches an alias that refers to itself
  // An object type, or a union of them, may refer to itself through its
  // fields: while it is resolved, they get this type (see Type::declareObject
  // and Type::declareUnion).
  Type *self = nullptr;
  bool recursive = false; // some field did
};

// A field of a class: `x: int = 0`, `static count = 0`, `readonly #id: string`,
// or a parameter property.
struct ClassField {
  llvm::StringRef name; // `#x` keeps its `#`
  llvm::SMLoc loc;
  std::optional<TypeRef> type;
  Expr *init = nullptr;
  bool optional = false; // `x?: T`
  bool definite = false; // `x!: T`, which Sema rejects
  bool readonly = false;
  bool isStatic = false;
  bool parameter = false; // declared by a constructor parameter
  bool isOverride = false; // `override x = 1`, a field of the class extended
  Access access = Access::Public;
  // Set by Sema: the type, and for a static field its variable, a global.
  const Type *resolved = nullptr;
  VarSymbol *var = nullptr;
  // Set by Sema: a field that a class up the chain declares already; this
  // declaration only gives it another initial value (see sema_inherit.cpp).
  bool redeclared = false;
  // An arrow that nothing assigns to the field again: compiled as a method of
  // that name (see Sema::resolveClass).
  FunctionDecl *method = nullptr;
  std::vector<Decorator> decorators;
  // `accessor x = 1`, an ordinary field: reading and writing it runs no code.
  bool accessor = false;
  // `@__tz_hidden x`, in a built-in module of tz (see Module::builtin): a
  // field that is no key, as a property node computes in a getter is not
  // one, so that the object prints as node prints it.
  bool hidden = false;
};

// `class Name { members }` (docs/objects.md, section 6), only at the top level
// of a file; also the class Sema makes for an object literal with methods.
struct ClassDecl : Stmt {
  ClassDecl(llvm::SMLoc loc, llvm::StringRef name) : Stmt(Kind::Class, loc), name(name) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Class; }

  // A field of the instances that this class declares.
  const ClassField *field(llvm::StringRef member) const {
    for (const ClassField &f : fields)
      if (f.name == member && !f.isStatic)
        return &f;
    return nullptr;
  }
  // Whether `ancestor` is this class or a class up its chain.
  bool derivesFrom(const ClassDecl *ancestor) const {
    for (const ClassDecl *c = this; c; c = c->parent)
      if (c == ancestor)
        return true;
    return false;
  }

  llvm::StringRef name; // empty for the class of a literal
  // `abstract class Name extends Base implements A, B`.
  bool isAbstract = false;
  std::optional<TypeRef> base;
  // `extends Timestamped(User)`: a base that is a value, an expression
  // whose type must be a class known when compiling (docs/decorators.md,
  // section 5). A class expression has its base here even when it is a
  // name, `class extends Base`, since that is usually a parameter.
  Expr *baseExpr = nullptr;
  std::vector<TypeRef> interfaces;
  // Set by Sema (sema_inherit.cpp): the class extended and the classes that
  // extend this one; the interfaces it implements, as types; the last number
  // among those of the classes down its chain, which follow its own; and
  // the methods its method table holds, by slot (null for an abstract one).
  ClassDecl *parent = nullptr;
  std::vector<ClassDecl *> children;
  std::vector<const Type *> implemented;
  unsigned lastId = 0;
  std::vector<FunctionDecl *> vtable;
  bool resolvingBase = false;
  // Parameter properties first, then the fields in the order declared: the
  // order node prints them in.
  std::vector<ClassField> fields;
  std::vector<FunctionDecl *> methods; // static or not, in the order declared
  std::vector<FunctionDecl *> constructors;
  // Set by Sema. The type of the instances, unique to the class; the method
  // that runs the initializers of the fields before any constructor, if a
  // field has one; the number of the class, for `instanceof`.
  const Type *type = nullptr;
  FunctionDecl *init = nullptr;
  unsigned id = 0;
  bool resolved = false;
  // Top-level code has run the declaration, which sets the static fields.
  bool declared = false;
  // The class of an object literal with methods. Its methods are closures,
  // whose captured variables the object itself holds; they share `group`.
  ObjectLitExpr *literal = nullptr;
  NestedGroup *group = nullptr;
  // A class of the language itself, which no file declares: Error and the
  // kinds derived from it (see Sema::declareErrorClass). CodeGen emits the
  // constructor of Error, which the kinds take.
  bool builtin = false;
  // Declared by a built-in module of tz written in tz (see Module::builtin),
  // such as EventEmitter of node:events, or Response of the globals of fetch.
  bool library = false;
  // `@Table("users") class User`, top to bottom (docs/decorators.md). Set by
  // Sema: the statements that apply the decorators of the class and of its
  // members, which run when the declaration does, after the static fields
  // get their values.
  std::vector<Decorator> decorators;
  std::vector<Stmt *> decorations;
  // Set by Sema: code uses the class as a value, whose object then holds the
  // static fields (they print with it, as in node).
  bool usedAsValue = false;
  // Classes as values (sema_classvalue.cpp). A class that a class
  // expression makes: Sema makes it while it checks code, and its static
  // fields get their values where the expression is evaluated. `outer` is
  // the copy of a generic function whose body has it, whose type parameters
  // it sees; `locals` the names of the variables of the functions around it,
  // which it cannot use.
  bool expression = false;
  const FunctionDecl *outer = nullptr;
  std::vector<llvm::StringRef> locals;
  // `@WithId class Order`: the class the decorator gave in its place, which
  // the name means from where the declaration runs on (`replaced`).
  ClassDecl *replacement = nullptr;
  bool replaced = false;
  // Reflection reaches the class (docs/decorators.md, section 6): its
  // description is in the exe, with the constructor when it becomes a
  // `Constructor<T>`, and its methods when its objects may be looked into
  // by name.
  bool reflectNew = false;
  bool reflectMethods = false;
  // fromJson(cls, j) may make its objects: from a Json, through its
  // constructor, then the fields named here (public, writable, of types a
  // Json gives) that the Json has. `reflectJson` when the class may be known
  // only when the program runs.
  bool reflectJson = false;
  std::vector<std::string> jsonParams, jsonFields; // the keys of the parameters, and the fields

  // `class Box<T> { ... }`: a generic class (docs/generics.md, section 5). It
  // is a pattern for a class for each list of type arguments, an instance
  // (`Box<int>`), which Sema makes from `source`, the text of the
  // declaration parsed again (see Sema::instantiateClass). Only its static
  // members, which no type parameter may appear in, belong to the generic
  // class itself.
  std::vector<TypeParamDecl> typeParams;
  llvm::StringRef source;
  // Set by Sema. What the type parameters stand for in the code of this
  // class: for the generic class itself, opaque type parameters
  // (Type::Kind::Param), with which its bodies are checked once; for an
  // instance, the types it is made for. An instance knows its generic
  // class, which knows its instances.
  std::vector<const Type *> typeArgs;
  ClassDecl *generic = nullptr;
  std::vector<ClassDecl *> instances;
  // An instance for type arguments that mention type parameters, which
  // generic code uses: it is checked with them, and not emitted.
  bool checkOnly = false;
  // The copies of its generic methods that calls need (see
  // Sema::instantiate), emitted with its other methods.
  std::vector<FunctionDecl *> methodInstances;
  // The generic class itself, rather than an instance of it.
  bool isGeneric() const { return !typeParams.empty() && !generic; }
  // A class whose code CodeGen emits: not a generic class (only its static
  // methods are) and not an instance that only generic code checks.
  bool isEmitted() const { return !isGeneric() && !checkOnly; }
  // The class the static members of this one belong to.
  const ClassDecl *staticOwner() const { return generic ? generic : this; }
  ClassDecl *staticOwner() { return generic ? generic : this; }
};

// Whether `cls` is Error or a class derived from it, built in or not.
inline bool derivesFromError(const ClassDecl *cls) {
  for (; cls; cls = cls->parent)
    if (cls->builtin && cls->name == "Error")
      return true;
  return false;
}

// The kinds of errors the language has built in (docs/errors.md): Error and
// the classes JavaScript derives from it, each a class that extends Error,
// in the order TZ_ERROR_* in runtime/tzrt.h numbers them.
inline constexpr llvm::StringLiteral kErrorKinds[] = {"Error", "TypeError", "RangeError",
                                                      "SyntaxError", "URIError"};

// The position of `name` in kErrorKinds, or -1.
inline int errorKindIndex(llvm::StringRef name) {
  for (size_t i = 0; i < std::size(kErrorKinds); ++i)
    if (kErrorKinds[i] == name)
      return int(i);
  return -1;
}

struct EnumMember {
  llvm::StringRef name;
  llvm::SMLoc loc;
  Expr *init = nullptr;
  // Set by Sema: the value, a number or a string.
  bool isString = false;
  int32_t number = 0;
  std::u16string text;
};

// `enum Name { A, B = 5, C }`. The members are constants: numbers counting up
// from 0 or from the last initializer, or strings. As a type, the enum is
// `int`, or `string` when its members are strings.
struct EnumDecl : Stmt {
  EnumDecl(llvm::SMLoc loc, llvm::StringRef name) : Stmt(Kind::Enum, loc), name(name) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Enum; }

  const EnumMember *find(llvm::StringRef member) const {
    for (const EnumMember &m : members)
      if (m.name == member)
        return &m;
    return nullptr;
  }

  llvm::StringRef name;
  std::vector<EnumMember> members;
  // Set by Sema.
  bool isString = false;
  bool resolved = false;
  bool resolving = false;
};

//===----------------------------------------------------------------------===//
// Modules
//===----------------------------------------------------------------------===//

struct Module;

// `a` or `a as b` in the braces of an import or an export.
struct ModuleName {
  llvm::StringRef name; // in the file it comes from
  llvm::SMLoc loc;
  llvm::StringRef as; // the name it gets; the same as `name` without `as`
  llvm::SMLoc asLoc;
};

// `import { a, b as c } from "./x"`. The parser keeps imports and exports
// apart from the statements: they only bind names.
struct ImportDecl {
  llvm::SMLoc loc;
  std::vector<ModuleName> names;
  std::string path; // as written
  llvm::SMLoc pathLoc;
  Module *from = nullptr; // set by the loader
  // `import fs from "node:fs"` or `import * as fs from "node:fs"` of a
  // built-in module: errors, which Sema reports with the names the file uses
  // of it (the parser reports those of the program's files).
  llvm::StringRef whole;
  llvm::SMLoc wholeLoc;
  bool wholeIsDefault = false;
  // Added by the loader: the globals of the web platform the file names
  // (tz:web), which JavaScript has without an import.
  bool implicit = false;
};

// Whether an import names a module built into tz (docs/io.md): "node:fs" and
// the same name without "node:", "tz:io"... (not "neverthrow", which only
// gives names of the language). The names are those of Node 24's
// require("module").builtinModules.
inline bool isBuiltinModule(llvm::StringRef path) {
  static constexpr llvm::StringLiteral bare[] = {
      "assert", "assert/strict", "async_hooks", "buffer", "child_process", "cluster", "console",
      "constants", "crypto", "dgram", "diagnostics_channel", "dns", "dns/promises", "domain",
      "events", "fs", "fs/promises", "http", "http2", "https", "inspector",
      "inspector/promises", "module", "net", "os", "path", "path/posix", "path/win32",
      "perf_hooks", "process", "punycode", "querystring", "readline", "readline/promises",
      "repl", "stream", "stream/consumers", "stream/promises", "stream/web", "string_decoder",
      "sys", "timers", "timers/promises", "tls", "trace_events", "tty", "url", "util",
      "util/types", "v8", "vm", "wasi", "worker_threads", "zlib"};
  if (path.starts_with("node:") || path.starts_with("tz:"))
    return true;
  for (llvm::StringLiteral name : bare)
    if (name == path)
      return true;
  return false;
}

// `export { a, b as c }`, `export { a } from "./x"` or `export * from "./x"`.
// `export` before a declaration is an export of the names it declares.
struct ExportDecl {
  llvm::SMLoc loc;
  std::vector<ModuleName> names;
  bool all = false;  // `export *`
  std::string path;  // empty when the names are this file's own
  llvm::SMLoc pathLoc;
  Module *from = nullptr; // set by the loader
};

// One parsed .ts file, or a whole program: the loader joins the statements
// of all its files in the order they load. Top-level statements other than
// function declarations form the program's entry point.
struct SourceFile {
  std::vector<Stmt *> items;
  std::vector<ImportDecl> imports;
  std::vector<ExportDecl> exports;
  // A program: its files in the order they load, the entry last.
  std::vector<Module *> modules;
  // The names of the fields of the object types and literals of the program:
  // `o.name(...)` with any other name cannot call a function in a field.
  llvm::StringSet<> fieldNames;
  // The strings of the constants declared with a string literal, by name
  // (in any scope), and of the members of string enums, as "Enum.Member":
  // the fields `o[key] = v` may assign to, which Sema must know before it
  // checks the program (see Sema::keyNames).
  llvm::StringMap<std::vector<std::string>> stringConstants;
  // Set by Sema: the program uses promises, timers or the queues, so main
  // runs the event loop after the top-level code (docs/async.md); and its
  // top-level code awaits, so it is a coroutine itself.
  bool usesAsync = false;
  bool topLevelAwait = false;
  // Set by Sema: the program compares functions (`==`, indexOf...), so every
  // evaluation of a function expression must give a distinct value.
  bool comparesFunctions = false;
  // Set by Sema: the program may use https (a fetch of a URL not known to be
  // http://, serve with `tls`), so it gets TLS (docs/net.md, section 4).
  bool usesTls = false;
  // Set by Sema: the functions of imported files that the program never
  // uses, which are left out of it.
  llvm::SmallPtrSet<const FunctionDecl *, 8> unusedFunctions;
  // Set by Sema: the copies of the generic functions that the program calls,
  // one for each representation of their type arguments in memory. CodeGen
  // emits them as top-level functions, and no generic function itself.
  std::vector<FunctionDecl *> instances;
  // Set by Sema: the object types declared to be others too (Type::bases):
  // classes that extend a class or implement interfaces, and interfaces that
  // extend interfaces. CodeGen lays out and describes their objects so that
  // code that knows them by a base type works on them.
  std::vector<const Type *> subtypes;
  // Set by Sema: the object types whose objects code sees as another type
  // with the same fields, some of them read-only there: CodeGen treats the
  // second as an interface that the first implements.
  std::vector<std::pair<const Type *, const Type *>> views;
};

// A file of a program.
struct Module {
  // Where the file is: for the entry as given, for the others found from it.
  std::string path;
  // The file relative to the entry's directory, with '/': "geometry/index.ts".
  std::string name;
  SourceFile file;
  bool isEntry = false;
  // A built-in module of tz written in tz (src/lib), which the loader reads
  // from next to the compiler: node:events, tz:http, and the modules of the
  // globals of the web platform such as EventTarget, URL and fetch. Only its
  // code may declare and call the intrinsics `__tz_*`.
  bool builtin = false;
  unsigned order = 0; // in the order files load
  // The files that were loading when this one started to, from the entry
  // on, this one last: the chain an import cycle shows.
  std::vector<Module *> loadPath;
};

// Owns all AST nodes and symbols of a compilation.
class AstContext {
public:
  template <typename T, typename... Args>
  T *make(Args &&...args) {
    auto node = std::make_unique<T>(std::forward<Args>(args)...);
    T *raw = node.get();
    nodes_.push_back(std::move(node));
    return raw;
  }

  VarSymbol *newVar() { return &vars_.emplace_back(); }
  FuncSymbol *newFunc() { return &funcs_.emplace_back(); }
  NestedGroup *newGroup() { return &groups_.emplace_back(); }
  Module *newModule() { return &modules_.emplace_back(); }
  // A name the source cannot contain, for a hidden variable the parser
  // introduces (destructuring keeps the value there).
  llvm::StringRef hiddenName() {
    return names_.emplace_back("%" + std::to_string(names_.size()));
  }
  // A name that is not in the source as it is, such as a field name written
  // as a string literal.
  llvm::StringRef keepName(std::string name) { return names_.emplace_back(std::move(name)); }

private:
  std::vector<std::unique_ptr<Node>> nodes_;
  std::deque<std::string> names_;
  std::deque<VarSymbol> vars_;
  std::deque<FuncSymbol> funcs_;
  std::deque<NestedGroup> groups_;
  std::deque<Module> modules_;
};

} // namespace tz
