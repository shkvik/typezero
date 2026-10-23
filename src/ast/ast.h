#pragma once

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/SMLoc.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tz {

class Type;

// A variable, parameter or global. Created by Sema, shared by all its uses.
struct FunctionDecl;

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
};

// Base of every AST node. Nodes are owned by AstContext and linked by raw
// pointers. Subclasses use LLVM-style RTTI (`classof`), since the compiler is
// built without C++ RTTI.
class Node {
public:
  virtual ~Node() = default;
};

// A type written in the source: `int`, `string[]`, `int | undefined`,
// `Array<int>`, `{ x: int; y?: string }`.
struct TypeRef {
  enum class Kind {
    Name,     // `name`; empty after a syntax error
    Array,    // `args[0][]`
    Union,    // `args[0] | args[1] | ...`
    Generic,  // `name<args...>`
    Function, // `(a: args[0], b?: args[1]) => args.back()`
    Object,   // `{ names[0]: args[0]; names[1]?: args[1] }`
  };

  Kind kind = Kind::Name;
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::vector<TypeRef> args;
  // Function types: which parameters are optional, and whether the last one
  // is a rest parameter. Object types: which fields are optional.
  std::vector<bool> optional;
  bool rest = false;
  // Object types: the names of the fields, where they are written, and which
  // are `readonly`.
  std::vector<llvm::StringRef> names;
  std::vector<llvm::SMLoc> locs;
  std::vector<bool> readonly;
};

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

enum class UnaryOp {
  Neg, Plus, Not, BitNot, TypeOf,
  Void,    // `void x`: evaluates x; the value is undefined
  Delete,  // `delete x[i]`, for objects to come; Sema rejects it for now
  NonNull, // postfix `x!`
};

enum class BinaryOp {
  Add, Sub, Mul, Div, Rem, Pow,
  BitAnd, BitOr, BitXor, Shl, Shr, UShr,
  Lt, Le, Gt, Ge, Eq, Ne,
  LogAnd, LogOr,
  Nullish, // `a ?? b`
  In,      // `key in object`, for objects to come; Sema rejects it for now
};

const char *opSpelling(UnaryOp op);
const char *opSpelling(BinaryOp op);

class Expr : public Node {
public:
  enum class Kind {
    IntLit, FloatLit, BoolLit, StringLit, Undefined, Template, ArrayLit, ObjectLit, Name, Member,
    Index, Unary, Binary, Assign, Update, Call, New, Conditional, Cast, Sequence, OptionalChain,
    Function,
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
};

class FloatLitExpr : public Expr {
public:
  FloatLitExpr(llvm::SMLoc loc, double value) : Expr(Kind::FloatLit, loc), value(value) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::FloatLit; }

  double value;
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

// `{ x: 1, y }`: an object with these fields, in this order; `y` alone is
// `y: y`.
class ObjectLitExpr : public Expr {
public:
  struct Property {
    llvm::StringRef name;
    llvm::SMLoc loc;
    Expr *value;
  };

  explicit ObjectLitExpr(llvm::SMLoc loc) : Expr(Kind::ObjectLit, loc) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::ObjectLit; }

  std::vector<Property> props;
};

class NameExpr : public Expr {
public:
  NameExpr(llvm::SMLoc loc, llvm::StringRef name) : Expr(Kind::Name, loc), name(name) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Name; }

  llvm::StringRef name;
  VarSymbol *var = nullptr; // set by Sema
  // Set by Sema for a top-level function used as a value.
  FuncSymbol *func = nullptr;
};

class MemberExpr : public Expr {
public:
  MemberExpr(llvm::SMLoc loc, Expr *object, llvm::StringRef name)
      : Expr(Kind::Member, loc), object(object), name(name) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Member; }

  Expr *object;
  llvm::StringRef name;
  bool optional = false; // `object?.name`
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
  ConsoleLog,
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
  // x.toFixed(digits) of a number.
  NumToFixed,
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
};

inline bool isMathBuiltin(Builtin builtin) {
  return builtin >= Builtin::MathFloor && builtin <= Builtin::MathRandom;
}

inline bool isRoundingBuiltin(Builtin builtin) {
  return builtin >= Builtin::MathFloor && builtin <= Builtin::MathTrunc;
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
};

// `new Array<T>(n)` or `new Array<T>()`.
class NewExpr : public Expr {
public:
  NewExpr(llvm::SMLoc loc, llvm::StringRef name, std::vector<TypeRef> typeArgs,
          std::vector<Expr *> args)
      : Expr(Kind::New, loc), name(name), typeArgs(std::move(typeArgs)), args(std::move(args)) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::New; }

  llvm::StringRef name;
  std::vector<TypeRef> typeArgs;
  std::vector<Expr *> args;
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

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

class Stmt : public Node {
public:
  enum class Kind {
    Block, VarDecl, Function, Expr, If, While, DoWhile, For, ForOf, Switch, Labeled, Return,
    Break, Continue, Empty, TypeAlias, Enum, Debugger,
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
  // A hidden variable holding the array a pattern destructures (see Parser).
  bool destructured = false;
};

// `let a = 1, b = 2` or `const c: double = 3`.
struct VarDeclStmt : Stmt {
  VarDeclStmt(llvm::SMLoc loc, bool isConst) : Stmt(Kind::VarDecl, loc), isConst(isConst) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::VarDecl; }

  bool isConst;
  std::vector<VarDeclarator> decls;
};

struct Param {
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::optional<TypeRef> type;
  bool optional = false;        // `name?: T`
  bool rest = false;            // `...name: T[]`, the last parameter
  Expr *defaultValue = nullptr; // `name: T = value`
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
  std::vector<Param> params;
  std::optional<TypeRef> returnType;
  BlockStmt *body = nullptr;
  // The source text of the declaration, for Sema to parse a fresh copy.
  llvm::StringRef source;
  bool isExpression = false; // an arrow or a function expression
  bool isArrow = false;
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

// `for (const x of iterable) body`: over the elements of an array, or the
// code points of a string.
struct ForOfStmt : Stmt {
  explicit ForOfStmt(llvm::SMLoc loc) : Stmt(Kind::ForOf, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::ForOf; }

  bool isConst = false;
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
  TypeRef type;
  // Set by Sema.
  const Type *resolved = nullptr;
  bool resolving = false; // catches an alias that refers to itself
  // An object type may refer to itself through its fields: while it is
  // resolved, they get this type (see Type::declareObject).
  Type *self = nullptr;
  bool recursive = false; // some field did
};

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

// One parsed .ts file. Top-level statements other than function declarations
// form the program's entry point.
struct SourceFile {
  std::vector<Stmt *> items;
  // The names of the fields of the object types and literals of the program:
  // `o.name(...)` with any other name cannot call a function in a field.
  llvm::StringSet<> fieldNames;
  // Set by Sema: the program compares functions (`==`, indexOf...), so every
  // evaluation of a function expression must give a distinct value.
  bool comparesFunctions = false;
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
};

} // namespace tz
