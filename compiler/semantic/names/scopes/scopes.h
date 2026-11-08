#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/names/scopes/scopes_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class Scopes {
public:
  using ResultEntryDeclared = Result<ScopeEntry, ScopesDeclareError>;
  using ResultNameIsVisible = Result<const VarSymbol *, ScopesCheckVisibleError>;

  explicit Scopes(SemanticState &owner);

  const Type *varType(VarSymbol *var);
  ScopeEntry lookup(llvm::StringRef name) const;
  void closeScope();
  ResultNameIsVisible checkVisible(NameExpr *name, const VarSymbol *var) const;
  ResultEntryDeclared declare(llvm::StringRef name, ScopeEntry entry);
  VarSymbol *declareVar(llvm::StringRef name, llvm::SMLoc loc, const Type *type, bool isConst);

private:
  SemanticState &state;
};

}
