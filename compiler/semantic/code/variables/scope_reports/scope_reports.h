#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/variables/code_variables_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>

namespace tz {

class ScopeReports {
public:
  explicit ScopeReports(SemanticState &owner);

  void closeScope();
  void reportUntyped();
  void reportUncheckedResults();

private:
  using ResultVarHasTypes = Result<const Type *, CodeVariablesRegistryVarTypedError>;
  using ResultResultCheck = Result<const VarSymbol *, CodeVariablesRegistryResultCheckedError>;

  void reportUntypedVariable(const Untyped &untyped);
  void reportUncheckedResult(const VarSymbol *variable);


  bool isUntypedHere(const Untyped &untyped) const;


  const Type *assignedType(const Untyped &untyped) const;
  const Type *commonAssigned(const Type *type, const Type *candidate) const;


  ResultResultCheck resultChecked(const VarSymbol *variable) const;
  ResultVarHasTypes varTyped(const Untyped &untyped, const Type *type) const;


  std::string typeHint(const Untyped &untyped, const Type *type) const;

  SemanticState &state;
};

}
