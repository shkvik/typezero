#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/generics_v2/functions/generics_functions_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <vector>

namespace tz {

class FunctionInstanceRules {
public:
  explicit FunctionInstanceRules(SemanticState &owner);

  void startInstantiate(InstantiateDraft draft);


  std::vector<const Type *> instanceKey(llvm::ArrayRef<const Type *> reps,
                                        llvm::ArrayRef<const Type *> exact) const;

private:
  using ResultChainIsFinite = Result<unsigned, GenericsFunctionsRegistryChainBoundedError>;

  bool isObjectLike(const Type *type) const;
  bool hasObjectLikeAlternative(const Type *param) const;
  bool isExactTypeParam(FuncSymbol *generic, size_t index);
  bool hasErrorArg(llvm::ArrayRef<const Type *> args) const;
  bool isExactParam(const FuncSymbol *generic, size_t index) const;
  bool isMentioningInObject(const Type *type, llvm::ArrayRef<const Type *> params) const;


  const Type *representation(const Type *type) const;
  const Type *resultRepresentation(std::vector<const Type *> &answers) const;
  const Type *nodeRepresentation(const Type *type, std::vector<const Type *> &answers) const;
  const Type *unionRepresentation(const Type *type, std::vector<const Type *> &answers) const;


  unsigned chainLength(const FuncSymbol *generic) const;
  std::vector<bool> exactParams(const FuncSymbol *generic) const;
  NodeChildren<const Type *> representationParts(const Type *type) const;
  std::vector<const Type *> instanceReps(FuncSymbol *generic, llvm::ArrayRef<const Type *> args);
  ResultChainIsFinite chainBounded(const FuncSymbol *generic,
                                   llvm::ArrayRef<const Type *> args) const;

  SemanticState &state;
};

}
