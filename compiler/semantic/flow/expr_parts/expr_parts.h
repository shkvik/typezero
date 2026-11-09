#pragma once

#include "compiler/program/ast/ast.h"
#include <vector>

namespace tz {

class ExprParts {
public:
  static std::vector<const Expr *> andParts(const Expr *cond);
  static std::vector<const Expr *> sameOperatorParts(const Expr *expr, BinaryOp op);
  static std::vector<const Expr *> logicParts(const Expr *expr);
  static std::vector<const Expr *> choiceBranches(const Expr *expr);
  static std::vector<const Expr *> operatorTree(const Expr *expr);
  static std::vector<const Expr *> literalLeaves(const Expr *expr);

private:
  static const Expr *withoutImplicitCasts(const Expr *expr);
};

}
