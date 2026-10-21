#pragma once

#include "llvm/ADT/StringRef.h"
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
};

struct FunctionDecl;

struct FuncSymbol {
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::vector<const Type *> params;
  const Type *ret = nullptr;
  FunctionDecl *decl = nullptr;
};

// Base of every AST node. Nodes are owned by AstContext and linked by raw
// pointers. Subclasses use LLVM-style RTTI (`classof`), since the compiler is
// built without C++ RTTI.
class Node {
public:
  virtual ~Node() = default;
};

// A type written in the source: `int`, `string[]`, `int | undefined`,
// `Array<int>`.
struct TypeRef {
  enum class Kind {
    Name,    // `name`; empty after a syntax error
    Array,   // `args[0][]`
    Union,   // `args[0] | args[1] | ...`
    Generic, // `name<args...>`
  };

  Kind kind = Kind::Name;
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::vector<TypeRef> args;
};

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

enum class UnaryOp { Neg, Plus, Not, BitNot };

enum class BinaryOp {
  Add, Sub, Mul, Div, Rem,
  BitAnd, BitOr, BitXor, Shl, Shr, UShr,
  Lt, Le, Gt, Ge, Eq, Ne,
  LogAnd, LogOr,
  Nullish, // `a ?? b`
};

const char *opSpelling(UnaryOp op);
const char *opSpelling(BinaryOp op);

class Expr : public Node {
public:
  enum class Kind {
    IntLit, FloatLit, BoolLit, StringLit, Undefined, Template, ArrayLit, Name, Member, Index,
    Unary, Binary, Assign, Update, Call, New, Conditional, Cast,
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
    bool spread; // `...expr`
  };

  explicit ArrayLitExpr(llvm::SMLoc loc) : Expr(Kind::ArrayLit, loc) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::ArrayLit; }

  std::vector<Element> elements;
};

class NameExpr : public Expr {
public:
  NameExpr(llvm::SMLoc loc, llvm::StringRef name) : Expr(Kind::Name, loc), name(name) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Name; }

  llvm::StringRef name;
  VarSymbol *var = nullptr; // set by Sema
};

class MemberExpr : public Expr {
public:
  MemberExpr(llvm::SMLoc loc, Expr *object, llvm::StringRef name)
      : Expr(Kind::Member, loc), object(object), name(name) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Member; }

  Expr *object;
  llvm::StringRef name;
};

// `object[index]`
class IndexExpr : public Expr {
public:
  IndexExpr(llvm::SMLoc loc, Expr *object, Expr *index)
      : Expr(Kind::Index, loc), object(object), index(index) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Index; }

  Expr *object;
  Expr *index;
  // Set by Sema. A read that Sema could not prove in range checks the index
  // and yields `T | undefined`. A write may append: Sema proved only
  // `index <= length`.
  bool mayBeOutOfRange = false;
  bool mayAppend = false;
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
  StringOf, // String(x)
  // Methods of string; the receiver is the object of the callee MemberExpr.
  StrCharCodeAt,
  StrAt,
  StrSlice,
  StrSubstring,
  StrIndexOf,
  StrIncludes,
  StrStartsWith,
  StrEndsWith,
  StrRepeat,
  StrToString,
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
};

class CallExpr : public Expr {
public:
  CallExpr(llvm::SMLoc loc, Expr *callee, std::vector<Expr *> args)
      : Expr(Kind::Call, loc), callee(callee), args(std::move(args)) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Call; }

  Expr *callee;
  std::vector<Expr *> args;
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

// A conversion: either `x as T` written by the user, or one inserted by Sema
// (then `target` is empty): a numeric widening, or turning a value into a
// string for `+` and template literals.
class CastExpr : public Expr {
public:
  CastExpr(llvm::SMLoc loc, Expr *operand, std::optional<TypeRef> target)
      : Expr(Kind::Cast, loc), operand(operand), target(target) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Cast; }

  bool isImplicit() const { return !target; }

  Expr *operand;
  std::optional<TypeRef> target;
};

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

class Stmt : public Node {
public:
  enum class Kind {
    Block, VarDecl, Function, Expr, If, While, DoWhile, For, ForOf, Return, Break, Continue,
    Empty,
  };

  Kind kind() const { return kind_; }
  llvm::SMLoc loc() const { return loc_; }

protected:
  Stmt(Kind kind, llvm::SMLoc loc) : kind_(kind), loc_(loc) {}

private:
  Kind kind_;
  llvm::SMLoc loc_;
};

struct BlockStmt : Stmt {
  explicit BlockStmt(llvm::SMLoc loc) : Stmt(Kind::Block, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Block; }

  std::vector<Stmt *> body;
};

struct VarDeclarator {
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::optional<TypeRef> typeAnn;
  Expr *init = nullptr;
  VarSymbol *sym = nullptr; // set by Sema
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
  VarSymbol *sym = nullptr; // set by Sema
};

struct FunctionDecl : Stmt {
  FunctionDecl(llvm::SMLoc loc, llvm::StringRef name) : Stmt(Kind::Function, loc), name(name) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Function; }

  llvm::StringRef name;
  std::vector<Param> params;
  std::optional<TypeRef> returnType;
  BlockStmt *body = nullptr;
  FuncSymbol *sym = nullptr; // set by Sema
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

struct ReturnStmt : Stmt {
  ReturnStmt(llvm::SMLoc loc, Expr *value) : Stmt(Kind::Return, loc), value(value) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Return; }

  Expr *value; // may be null
};

struct BreakStmt : Stmt {
  explicit BreakStmt(llvm::SMLoc loc) : Stmt(Kind::Break, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Break; }
};

struct ContinueStmt : Stmt {
  explicit ContinueStmt(llvm::SMLoc loc) : Stmt(Kind::Continue, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Continue; }
};

struct EmptyStmt : Stmt {
  explicit EmptyStmt(llvm::SMLoc loc) : Stmt(Kind::Empty, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Empty; }
};

// One parsed .ts file. Top-level statements other than function declarations
// form the program's entry point.
struct SourceFile {
  std::vector<Stmt *> items;
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

private:
  std::vector<std::unique_ptr<Node>> nodes_;
  std::deque<VarSymbol> vars_;
  std::deque<FuncSymbol> funcs_;
};

} // namespace tz
