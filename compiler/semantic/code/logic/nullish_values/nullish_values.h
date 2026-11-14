#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/logic/code_logic_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>

namespace tz {

class NullishValues {
public:
  explicit NullishValues(SemanticState &owner);

  void startNullish(const StartNullish &task);
  void finishNullish(const FinishNullish &task);
  void afterNullishLeft(const AfterNullishLeft &task);
  void startNullishRight(const StartNullishRight &task);

private:
  using ResultNullishResolved = Result<const Type *, CodeLogicRegistryCheckNullishError>;

  const Type *failedNullishType(const Type *leftType, const Type *rightType) const;
  std::string operandTypesText(llvm::StringRef op, const Type *lhs, const Type *rhs) const;
  ResultNullishResolved checkNullishType(const Type *leftType, const Type *rightType) const;

  SemanticState &state;
};

}
