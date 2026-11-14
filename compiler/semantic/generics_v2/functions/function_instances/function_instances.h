#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics_v2/functions/function_instance_rules/function_instance_rules.h"
#include "compiler/semantic/generics_v2/functions/generics_functions_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class FunctionInstances {
public:
  FunctionInstances(SemanticState &owner, FunctionInstanceRules &sharedFunctionInstanceRules);

  void afterReusedWalk(const AfterReusedWalk &task);
  void walkPreciseCopy(const WalkPreciseCopy &task);
  void afterInstanceTypes(const AfterInstanceTypes &task);
  void nextMeaningfulParam(const NextMeaningfulParam &task);
  void afterMeaningfulParam(const AfterMeaningfulParam &task);
  void afterNewInstanceWalk(const AfterNewInstanceWalk &task);
  void instantiateCallTarget(const InstantiateCallTarget &task);

private:
  using ResultParamsMeaning =
      Result<const FuncSymbol *, GenericsFunctionsRegistryParamsMeaningfulError>;

  void buildInstanceDecl(InstantiateDraft draft);
  void startInstanceDecl(InstantiateDraft draft);
  void placeInstance(const InstanceRecord &instance);
  void retarget(CallExpr *call, FuncSymbol *instance) const;
  void expandArgumentList(FunctionDecl *functionDecl, FuncSymbol *function);
  void copyParams(const FunctionDecl *functionDecl, FunctionDecl *copy) const;
  void copyMemberRole(const FunctionDecl *functionDecl, FunctionDecl *copy) const;
  void finishInstantiate(const InstantiateDraft &draft, FuncSymbol *instance) const;
  void applyExactParams(FuncSymbol *function, FunctionDecl *copy,
                        llvm::ArrayRef<const Type *> exact) const;


  bool isExpandableRest(const FunctionDecl *functionDecl, const FuncSymbol *function) const;


  Param listRestParam(const Param &rest, const Type *more);
  Param listPartParam(const Param &rest, const Type *list, size_t index);


  FunctionDecl *freshCopy(const FunctionDecl *functionDecl);
  ResultParamsMeaning paramMeaningful(const InstantiateDraft &draft, const Type *substituted) const;

  SemanticState &state;
  FunctionInstanceRules &functionInstanceRules;
};

}
