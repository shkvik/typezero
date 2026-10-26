#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class Destination {
public:
  explicit Destination(Checker &checker);

  const Type *literalDestination(const VarDeclarator &d, llvm::ArrayRef<VarDeclarator> later);
  static bool literalShaped(const Expr *expr);
  bool literalFits(const Expr *expr, const Type *type) const;
  const Type *declaredFunctionType(const FunctionDecl *fn);

private:
  Checker &checker_;
};

}
