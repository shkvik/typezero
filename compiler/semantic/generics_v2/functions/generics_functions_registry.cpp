#include "compiler/semantic/generics_v2/functions/generics_functions_registry.h"

namespace tz {

GenericsFunctionsRegistry::GenericsFunctionsRegistry(SemanticState &owner)
    : functionInstanceRules(owner),
      genericValues(owner, this->functionInstanceRules),
      functionInstances(owner, this->functionInstanceRules) {}

void GenericsFunctionsRegistry::checkGenericValue(const CheckGenericValue &task) {
  return this->genericValues.checkGenericValue(task);
}

void GenericsFunctionsRegistry::nextValueTypeArg(const NextValueTypeArg &task) {
  return this->genericValues.nextValueTypeArg(task);
}

void GenericsFunctionsRegistry::afterValueTypeArg(const AfterValueTypeArg &task) {
  return this->genericValues.afterValueTypeArg(task);
}

void GenericsFunctionsRegistry::afterGenericValueArgs(const AfterGenericValueArgs &task) {
  return this->genericValues.afterGenericValueArgs(task);
}

void GenericsFunctionsRegistry::afterGenericValueType(const AfterGenericValueType &task) {
  return this->genericValues.afterGenericValueType(task);
}

void GenericsFunctionsRegistry::instantiateCallTarget(const InstantiateCallTarget &task) {
  return this->functionInstances.instantiateCallTarget(task);
}

void GenericsFunctionsRegistry::afterReusedWalk(const AfterReusedWalk &task) {
  return this->functionInstances.afterReusedWalk(task);
}

void GenericsFunctionsRegistry::nextMeaningfulParam(const NextMeaningfulParam &task) {
  return this->functionInstances.nextMeaningfulParam(task);
}

void GenericsFunctionsRegistry::afterMeaningfulParam(const AfterMeaningfulParam &task) {
  return this->functionInstances.afterMeaningfulParam(task);
}

void GenericsFunctionsRegistry::walkPreciseCopy(const WalkPreciseCopy &task) {
  return this->functionInstances.walkPreciseCopy(task);
}

void GenericsFunctionsRegistry::afterInstanceTypes(const AfterInstanceTypes &task) {
  return this->functionInstances.afterInstanceTypes(task);
}

void GenericsFunctionsRegistry::afterNewInstanceWalk(const AfterNewInstanceWalk &task) {
  return this->functionInstances.afterNewInstanceWalk(task);
}

}
