#include "compiler/semantic/generics_v2/classes/generics_classes_registry.h"

namespace tz {

GenericsClassesRegistry::GenericsClassesRegistry(SemanticState &owner)
    : genericNews(owner),
      classInstances(owner),
      instanceFilling(owner),
      instanceMembers(owner) {}

void GenericsClassesRegistry::checkGenericNew(const CheckGenericNew &task) {
  return this->genericNews.checkGenericNew(task);
}

void GenericsClassesRegistry::afterEmitterNew(const AfterEmitterNew &task) {
  return this->genericNews.afterEmitterNew(task);
}

void GenericsClassesRegistry::nextExplicitNewArgument(const NextExplicitNewArgument &task) {
  return this->genericNews.nextExplicitNewArgument(task);
}

void GenericsClassesRegistry::afterExplicitNewArgument(const AfterExplicitNewArgument &task) {
  return this->genericNews.afterExplicitNewArgument(task);
}

void GenericsClassesRegistry::afterExplicitInstance(const AfterExplicitInstance &task) {
  return this->genericNews.afterExplicitInstance(task);
}

void GenericsClassesRegistry::afterGenericNewBinding(const AfterGenericNewBinding &task) {
  return this->genericNews.afterGenericNewBinding(task);
}

void GenericsClassesRegistry::afterGenericNewInstance(const AfterGenericNewInstance &task) {
  return this->genericNews.afterGenericNewInstance(task);
}

void GenericsClassesRegistry::instantiateClassType(const InstantiateClassType &task) {
  return this->classInstances.instantiateClassType(task);
}

void GenericsClassesRegistry::afterInstanceTypeParams(const AfterInstanceTypeParams &task) {
  return this->classInstances.afterInstanceTypeParams(task);
}

void GenericsClassesRegistry::nextInstanceDefault(const NextInstanceDefault &task) {
  return this->classInstances.nextInstanceDefault(task);
}

void GenericsClassesRegistry::afterInstanceDefault(const AfterInstanceDefault &task) {
  return this->classInstances.afterInstanceDefault(task);
}

void GenericsClassesRegistry::nextInstanceArgument(const NextInstanceArgument &task) {
  return this->classInstances.nextInstanceArgument(task);
}

void GenericsClassesRegistry::afterInstanceArgument(const AfterInstanceArgument &task) {
  return this->classInstances.afterInstanceArgument(task);
}

void GenericsClassesRegistry::finishClassInstance(const FinishClassInstance &task) {
  return this->classInstances.finishClassInstance(task);
}

void GenericsClassesRegistry::fillUnfilledInstances() {
  return this->instanceFilling.fillUnfilledInstances();
}

void GenericsClassesRegistry::walkOrPostpone(const WalkOrPostpone &task) {
  return this->classInstances.walkOrPostpone(task);
}

void GenericsClassesRegistry::afterInstanceWalk(const AfterInstanceWalk &task) {
  return this->classInstances.afterInstanceWalk(task);
}

void GenericsClassesRegistry::fillInstance(const FillInstance &task) {
  return this->instanceFilling.fillInstance(task);
}

void GenericsClassesRegistry::leaveFilling() {
  return this->instanceFilling.leaveFilling();
}

void GenericsClassesRegistry::linkInstanceBases(const LinkInstanceBases &task) {
  return this->instanceFilling.linkInstanceBases(task);
}

void GenericsClassesRegistry::afterInstanceParent(const AfterInstanceParent &task) {
  return this->instanceFilling.afterInstanceParent(task);
}

void GenericsClassesRegistry::nextInstanceInterface(const NextInstanceInterface &task) {
  return this->instanceFilling.nextInstanceInterface(task);
}

void GenericsClassesRegistry::afterInstanceInterface(const AfterInstanceInterface &task) {
  return this->instanceFilling.afterInstanceInterface(task);
}

void GenericsClassesRegistry::nextFilledField(const NextFilledField &task) {
  return this->instanceFilling.nextFilledField(task);
}

void GenericsClassesRegistry::afterFilledField(const AfterFilledField &task) {
  return this->instanceFilling.afterFilledField(task);
}

void GenericsClassesRegistry::declareInstanceMembers(const DeclareInstanceMembers &task) {
  return this->instanceMembers.declareInstanceMembers(task);
}

void GenericsClassesRegistry::nextInstanceMember(const NextInstanceMember &task) {
  return this->instanceMembers.nextInstanceMember(task);
}

void GenericsClassesRegistry::afterInstanceInit(const AfterInstanceInit &task) {
  return this->instanceMembers.afterInstanceInit(task);
}

void GenericsClassesRegistry::nextIteratedParam(const NextIteratedParam &task) {
  return this->instanceMembers.nextIteratedParam(task);
}

void GenericsClassesRegistry::afterIteratedParam(const AfterIteratedParam &task) {
  return this->instanceMembers.afterIteratedParam(task);
}

void GenericsClassesRegistry::nextMemberType(const NextMemberType &task) {
  return this->instanceMembers.nextMemberType(task);
}

void GenericsClassesRegistry::afterMemberInstanceType(const AfterMemberInstanceType &task) {
  return this->instanceMembers.afterMemberInstanceType(task);
}

void GenericsClassesRegistry::afterMemberType(const AfterMemberType &task) {
  return this->instanceMembers.afterMemberType(task);
}

void GenericsClassesRegistry::afterMemberSignature(const AfterMemberSignature &task) {
  return this->instanceMembers.afterMemberSignature(task);
}

void GenericsClassesRegistry::walkInstanceVariants(const WalkInstanceVariants &task) {
  return this->instanceMembers.walkInstanceVariants(task);
}

void GenericsClassesRegistry::afterInstanceVariants(const AfterInstanceVariants &task) {
  return this->instanceMembers.afterInstanceVariants(task);
}

void GenericsClassesRegistry::linkInstance(const LinkInstance &task) {
  return this->classInstances.linkInstance(task);
}

void GenericsClassesRegistry::markOverridden(const MarkOverridden &task) {
  return this->classInstances.markOverridden(task);
}

}
