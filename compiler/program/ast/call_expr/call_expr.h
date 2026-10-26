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
  std::vector<bool> spread;
  bool spreads(size_t i) const { return i < spread.size() && spread[i]; }
  bool hasSpread() const { return llvm::is_contained(spread, true); }
  bool optional = false;
  bool tagged = false;
  FuncSymbol *func = nullptr;
  Builtin builtin = Builtin::None;
  bool mayBeOutOfRange = false;
  FuncSymbol *method = nullptr;
  bool virtualCall = false;
  int signature = -1;
  bool superCall = false;
  bool fused = false;
  const RegexLitExpr *regex = nullptr;
  bool forIn = false;
  struct UnionMethod {
    const Type *member;
    FuncSymbol *method;
    bool virtualCall;
  };
  std::vector<UnionMethod> unionMethods;
  const void *metaKey = nullptr;
  bool designRecord = false;
};

}
