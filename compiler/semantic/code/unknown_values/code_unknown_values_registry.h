#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/unknown_values/code_unknown_values_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeUnknownValuesRegistry {
public:
  explicit CodeUnknownValuesRegistry(SemanticState &owner);

  void checkUnknownMember(const CheckUnknownMember &task);
  void checkUnknownMethod(const CheckUnknownMethod &task);
  void finishUnknownMethod(const FinishUnknownMethod &task);
  void finishUnknownStaticCall(const FinishUnknownStaticCall &task);

private:
  using ResultValueNarrowed = Result<const Expr *, CodeUnknownValuesRegistryUnknownUseError>;
  using ResultMemberAllowed = Result<void, CodeUnknownValuesRegistryCheckUnknownMemberError>;
  using ResultMethodAllowed = Result<void, CodeUnknownValuesRegistryCheckUnknownMethodError>;

  std::string hintName(const Expr *expr) const;
  ResultMethodAllowed checkNotClassValue(const Type *receiver) const;
  ResultMethodAllowed checkNotRuntimeClass(const Type *receiver) const;
  ResultMemberAllowed checkNotConstructor(const MemberExpr *member) const;
  ResultValueNarrowed unknownUse(const Expr *object, llvm::StringRef what) const;

  SemanticState &state;
};

}
