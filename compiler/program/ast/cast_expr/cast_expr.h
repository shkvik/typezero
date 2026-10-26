#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/type_ref/type_ref.h"

namespace tz {

class CastExpr : public Expr {
public:
  CastExpr(llvm::SMLoc loc, Expr *operand, std::optional<TypeRef> target)
      : Expr(Kind::Cast, loc), operand(operand), target(target) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Cast; }

  bool isImplicit() const { return !target; }

  Expr *operand;
  std::optional<TypeRef> target;
  bool shortCircuit = false;
  bool text = false;
  bool adaptsResult = false;
};

}
