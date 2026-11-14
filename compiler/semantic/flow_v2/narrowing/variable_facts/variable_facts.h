#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"

namespace tz {

class SemanticState;

class VariableFacts {
public:
  explicit VariableFacts(SemanticState &owner);

  void addVariableInstanceOfFacts(const BinaryExpr *binary, Flow &whenTrue) const;
  void addVariableTypeOfFacts(const BinaryExpr *binary, Flow &whenTrue, Flow &whenFalse) const;

private:
  void addSideTypeOfFacts(const Expr *side, const Expr *other, Flow &whenEqual,
                          Flow &whenUnequal) const;
  void addParamTypeOfFacts(const VarSymbol *variable, const std::u16string &text, Flow &holds,
                           Flow &differs) const;


  bool canBe(const Type *param, bool (Type::*test)() const) const;
  bool isSubclassOf(const Type *classType, const Type *own) const;
  bool areAllNamed(llvm::ArrayRef<const Type *> types, llvm::StringRef name) const;


  const Type *instanceType(const Type *type) const;
  const Type *numberNamed(const Type *param, llvm::StringRef name) const;
  const Type *otherAlternatives(const Type *param, const std::u16string &text) const;
  const Type *paramTypeOfNarrowed(const Type *param, const std::u16string &text) const;


  const VarSymbol *variableOf(const Expr *expr) const;
  llvm::SmallVector<const Type *, 4> alternativesNotNamed(const Type *param,
                                                          llvm::StringRef name) const;

  SemanticState &state;
};

}
