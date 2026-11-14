#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics_v2/functions/function_instance_rules/function_instance_rules.h"
#include "compiler/semantic/generics_v2/functions/generics_functions_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <vector>

namespace tz {

class GenericValues {
public:
  GenericValues(SemanticState &owner, FunctionInstanceRules &sharedFunctionInstanceRules);

  void nextValueTypeArg(const NextValueTypeArg &task);
  void afterValueTypeArg(const AfterValueTypeArg &task);
  void checkGenericValue(const CheckGenericValue &task);
  void startGenericValue(const StartGenericValue &task);
  void afterGenericValueArgs(const AfterGenericValueArgs &task);
  void afterGenericValueType(const AfterGenericValueType &task);

private:
  using ResultTypeArgsCount = Result<size_t, GenericsFunctionsRegistryTypeArgCountError>;
  using ResultTypeArgsGiven = Result<const NameExpr *, GenericsFunctionsRegistryTypeArgsGivenError>;
  using ResultNoClassReturn =
      Result<const FuncSymbol *, GenericsFunctionsRegistryOwnClassReturnError>;

  void finishGenericValueArgs(GenericValueDraft draft);
  void bindFromExpected(const FuncSymbol *generic, const Type *returnType,
                        const Signature &expected, std::vector<const Type *> &bound);


  bool isConcrete(llvm::ArrayRef<const Type *> bound) const;
  bool returnsOwnClass(const FuncSymbol *generic, const Type *returnType);


  const ClassDecl *literalClassIn(const Type *type) const;
  const Type *unifiedCandidates(const CandidateInference &inference, size_t index) const;
  ResultTypeArgsCount typeArgCount(const FuncSymbol *generic, const NameExpr *name) const;
  ResultNoClassReturn ownClassReturn(const FuncSymbol *generic, const ClassDecl *classDecl) const;
  ResultTypeArgsGiven typeArgsGiven(const FuncSymbol *generic, const NameExpr *name,
                                    const Signature *expected) const;

  SemanticState &state;
  FunctionInstanceRules &functionInstanceRules;
};

}
