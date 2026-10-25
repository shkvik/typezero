#pragma once

#include "compiler/program/ast/builtin/builtin.h"
#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/regex_lit_expr/regex_lit_expr.h"
#include "compiler/program/symbols/func_symbol/func_symbol.h"

namespace tz {

class Type;

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
  // Set by Checker: exactly one of these describes the callee.
  FuncSymbol *func = nullptr;
  Builtin builtin = Builtin::None;
  // Set by Checker for charCodeAt, at, pop and shift: the position may be out of
  // range (or the array empty), so the result type admits that case.
  bool mayBeOutOfRange = false;
  // Set by Checker for a call of a method of a class: the variant called. The
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
  // Set by Checker for a stage of a lazy chain (`xs.values().map(f)`) that a
  // consumer takes apart where it is written: the chain runs as one loop
  // there, and its callbacks are called only during it (see
  // Checker::markFused).
  bool fused = false;
  // Set by Checker for a call that takes a regex known when compiling (a
  // literal, or a variable that holds one): where its groups are.
  const RegexLitExpr *regex = nullptr;
  // The Object.keys that `for...in` walks: of a Json that is null or
  // undefined, no keys rather than a TypeError, as in JavaScript.
  bool forIn = false;
  // Set by Checker for a method called on a union of objects that no type of
  // them all declares (see Checker::checkUnionMethodCall): for each member, the
  // method that runs, through the method table with `virtualCall`; or null
  // for a function in a field.
  struct UnionMethod {
    const Type *member;
    FuncSymbol *method;
    bool virtualCall;
  };
  std::vector<UnionMethod> unionMethods;
  // Metadata (Builtin::MetaDefine...): the object key, a declaration Checker
  // knows (a function, a class, a constant), or null for a string key; and
  // a record `design:*` the compiler writes.
  const void *metaKey = nullptr;
  bool designRecord = false;
};

} // namespace tz
