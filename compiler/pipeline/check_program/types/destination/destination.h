#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class Destination {
public:
  explicit Destination(CheckState &owner);

  const Type *literalDestination(const VarDeclarator &d, llvm::ArrayRef<VarDeclarator> later);
  static bool literalShaped(const Expr *expr);
  bool literalFits(const Expr *expr, const Type *type) const;
  const Type *declaredFunctionType(const FunctionDecl *fn);

private:
  CheckState &state;
};

}
