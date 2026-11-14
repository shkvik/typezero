#pragma once

#include "compiler/semantic/generics_v2/inference/binding_rules/binding_rules.h"
#include "compiler/semantic/generics_v2/inference/generics_inference_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>

namespace tz {

class ArgumentPatterns {
public:
  ArgumentPatterns(SemanticState &owner, BindingRules &sharedBindingRules);

  void inferFromArguments(const InferFromArguments &task);
  void afterFittingDefault(const AfterFittingDefault &task);
  void nextFittingArgument(const NextFittingArgument &task);
  void inferArgumentPatterns(const InferArgumentPatterns &task);
  void afterFittingConstraint(const AfterFittingConstraint &task);

private:
  void fitArgument(InferredArgumentsDraft draft);
  void inferPatterns(CandidateInference &inference, const FuncSymbol *generic,
                     llvm::ArrayRef<Expr *> args) const;


  const Type *argumentPattern(const FuncSymbol *generic, size_t index) const;
  std::optional<const Type *> argumentCandidate(const CandidateInference &inference,
                                                size_t index) const;

  SemanticState &state;
  BindingRules &bindingRules;
};

}
