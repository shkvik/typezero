#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class VarChecker {
public:
  explicit VarChecker(Checker &checker);

  void checkVarDecl(VarDeclStmt *decl);
  void noteUntyped(VarSymbol *var, bool initUndefined);
  void assignedUntyped(const VarSymbol *var, const Type *type);
  void reportUntyped();

private:
  Checker &checker_;
};

}
