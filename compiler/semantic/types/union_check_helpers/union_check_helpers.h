#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/Support/ConvertUTF.h"

namespace tz {

class UnionCheckHelpers {
public:
  static const Type *writtenLiteral(const Expr *expr);

private:
  static const Type *wholeLiteral(const IntLitExpr *whole, bool isNegated);
  static const Type *fractionLiteral(const FloatLitExpr *fraction, bool isNegated);
};

}
