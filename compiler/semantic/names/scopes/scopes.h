#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/names/scopes/scopes_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class VarChecker;
class ReturnInferrer;

class Scopes {
public:
  using ResultEntryDeclared = Result<ScopeEntry, ScopesDeclareError>;
  using ResultNameIsVisible = Result<const VarSymbol *, ScopesCheckVisibleError>;

  Scopes(SemanticState &owner, SemanticParts &all);

  void openScope();
  void closeScope();
  const Type *varType(VarSymbol *variable);
  ScopeEntry lookup(llvm::StringRef name) const;
  ResultEntryDeclared declare(llvm::StringRef name, ScopeEntry entry);
  VarSymbol *declareLet(llvm::StringRef name, llvm::SMLoc loc, const Type *type);
  VarSymbol *declareConst(llvm::StringRef name, llvm::SMLoc loc, const Type *type);
  ResultNameIsVisible checkVisible(NameExpr *name, const VarSymbol *variable) const;
  VarSymbol *declareVar(llvm::StringRef name, llvm::SMLoc loc, const Type *type, bool isConst);

private:
  using ResultNameIsAllowed = Result<void, ScopesDeclareError>;

  void placeVariable(VarSymbol *variable);
  ResultNameIsAllowed checkNotDeclared(llvm::StringRef name) const;
  ResultNameIsAllowed checkNameAllowed(llvm::StringRef name) const;
  ResultNameIsAllowed checkFunctionOnce(llvm::StringRef name, const ScopeEntry &entry) const;

  SemanticState &state;
  VarChecker *varChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
};

}
