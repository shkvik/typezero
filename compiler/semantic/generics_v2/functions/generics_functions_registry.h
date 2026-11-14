#pragma once

#include "compiler/semantic/generics_v2/functions/function_instance_rules/function_instance_rules.h"
#include "compiler/semantic/generics_v2/functions/function_instances/function_instances.h"
#include "compiler/semantic/generics_v2/functions/generic_values/generic_values.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class GenericsFunctionsRegistry {
public:
  explicit GenericsFunctionsRegistry(SemanticState &owner);

  void afterReusedWalk(const AfterReusedWalk &task);
  void walkPreciseCopy(const WalkPreciseCopy &task);
  void nextValueTypeArg(const NextValueTypeArg &task);
  void afterValueTypeArg(const AfterValueTypeArg &task);
  void checkGenericValue(const CheckGenericValue &task);
  void startGenericValue(const StartGenericValue &task);
  void afterInstanceTypes(const AfterInstanceTypes &task);
  void nextMeaningfulParam(const NextMeaningfulParam &task);
  void afterMeaningfulParam(const AfterMeaningfulParam &task);
  void afterNewInstanceWalk(const AfterNewInstanceWalk &task);
  void afterGenericValueArgs(const AfterGenericValueArgs &task);
  void afterGenericValueType(const AfterGenericValueType &task);
  void instantiateCallTarget(const InstantiateCallTarget &task);

private:
  FunctionInstanceRules functionInstanceRules;
  GenericValues genericValues;
  FunctionInstances functionInstances;
};

}
