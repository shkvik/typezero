#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class Scopes {
public:
  explicit Scopes(Checker &checker);

  void declare(llvm::StringRef name, llvm::SMLoc loc, Checker::ScopeEntry entry);
  Checker::ScopeEntry lookup(llvm::StringRef name) const;
  VarSymbol *declareVar(llvm::StringRef name, llvm::SMLoc loc, const Type *type, bool isConst);
  const Type *varType(VarSymbol *var);
  void checkVisible(NameExpr *name, const VarSymbol *var);

private:
  Checker &checker_;
};

}
