#pragma once

#include "compiler/semantic/generics_v2/classes/class_instances/class_instances.h"
#include "compiler/semantic/generics_v2/classes/generic_news/generic_news.h"
#include "compiler/semantic/generics_v2/classes/instance_filling/instance_filling.h"
#include "compiler/semantic/generics_v2/classes/instance_members/instance_members.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class GenericsClassesRegistry {
public:
  explicit GenericsClassesRegistry(SemanticState &owner);

  void leaveFilling();
  void fillUnfilledInstances();
  void fillInstance(const FillInstance &task);
  void linkInstance(const LinkInstance &task);
  void markOverridden(const MarkOverridden &task);
  void nextMemberType(const NextMemberType &task);
  void walkOrPostpone(const WalkOrPostpone &task);
  void afterEmitterNew(const AfterEmitterNew &task);
  void afterMemberType(const AfterMemberType &task);
  void checkGenericNew(const CheckGenericNew &task);
  void nextFilledField(const NextFilledField &task);
  void afterFilledField(const AfterFilledField &task);
  void afterInstanceInit(const AfterInstanceInit &task);
  void afterInstanceWalk(const AfterInstanceWalk &task);
  void linkInstanceBases(const LinkInstanceBases &task);
  void nextIteratedParam(const NextIteratedParam &task);
  void afterIteratedParam(const AfterIteratedParam &task);
  void nextInstanceMember(const NextInstanceMember &task);
  void afterInstanceParent(const AfterInstanceParent &task);
  void finishClassInstance(const FinishClassInstance &task);
  void nextInstanceDefault(const NextInstanceDefault &task);
  void afterInstanceDefault(const AfterInstanceDefault &task);
  void afterMemberSignature(const AfterMemberSignature &task);
  void instantiateClassType(const InstantiateClassType &task);
  void nextInstanceArgument(const NextInstanceArgument &task);
  void walkInstanceVariants(const WalkInstanceVariants &task);
  void afterExplicitInstance(const AfterExplicitInstance &task);
  void afterInstanceArgument(const AfterInstanceArgument &task);
  void afterInstanceVariants(const AfterInstanceVariants &task);
  void nextInstanceInterface(const NextInstanceInterface &task);
  void afterGenericNewBinding(const AfterGenericNewBinding &task);
  void afterInstanceInterface(const AfterInstanceInterface &task);
  void declareInstanceMembers(const DeclareInstanceMembers &task);
  void afterGenericNewInstance(const AfterGenericNewInstance &task);
  void afterInstanceTypeParams(const AfterInstanceTypeParams &task);
  void afterMemberInstanceType(const AfterMemberInstanceType &task);
  void nextExplicitNewArgument(const NextExplicitNewArgument &task);
  void afterExplicitNewArgument(const AfterExplicitNewArgument &task);

private:
  GenericNews genericNews;
  ClassInstances classInstances;
  InstanceFilling instanceFilling;
  InstanceMembers instanceMembers;
};

}
