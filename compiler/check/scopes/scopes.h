#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/scopes/scopes_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class Scopes {
public:
  using ResultEntryDeclared = Result<ScopeEntry, ScopesDeclareError>;
  using ResultNameIsVisible = Result<const VarSymbol *, ScopesCheckVisibleError>;

  explicit Scopes(Checker &owner);

  const Type *varType(VarSymbol *var);
  ScopeEntry lookup(llvm::StringRef name) const;
  ResultNameIsVisible checkVisible(NameExpr *name, const VarSymbol *var) const;
  ResultEntryDeclared declare(llvm::StringRef name, llvm::SMLoc loc, ScopeEntry entry);
  VarSymbol *declareVar(llvm::StringRef name, llvm::SMLoc loc, const Type *type, bool isConst);

private:
  Checker &checker;
};

}
