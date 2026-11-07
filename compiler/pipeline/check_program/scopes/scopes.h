#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/scopes/scopes_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class Scopes {
public:
  using ResultEntryDeclared = Result<ScopeEntry, ScopesDeclareError>;
  using ResultNameIsVisible = Result<const VarSymbol *, ScopesCheckVisibleError>;

  explicit Scopes(CheckState &owner);

  const Type *varType(VarSymbol *var);
  ScopeEntry lookup(llvm::StringRef name) const;
  void closeScope();
  ResultNameIsVisible checkVisible(NameExpr *name, const VarSymbol *var) const;
  ResultEntryDeclared declare(llvm::StringRef name, ScopeEntry entry);
  VarSymbol *declareVar(llvm::StringRef name, llvm::SMLoc loc, const Type *type, bool isConst);

private:
  CheckState &state;
};

}
