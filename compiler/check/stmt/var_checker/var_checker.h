#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// let and const, and the variables declared without a type.
class VarChecker {
public:
  explicit VarChecker(Sema &sema);

  void checkVarDecl(VarDeclStmt *decl);
  // `let x` without a type or a value, and `let x = undefined`: reported when
  // the scope of x ends, with a type made from the values assigned to x up
  // to there as the hint.
  void noteUntyped(VarSymbol *var, bool initUndefined);
  void assignedUntyped(const VarSymbol *var, const Type *type);
  void reportUntyped();

private:
  Sema &sema_;
};

} // namespace tz
