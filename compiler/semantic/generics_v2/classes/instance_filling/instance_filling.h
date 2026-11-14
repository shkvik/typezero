#pragma once

#include "compiler/semantic/generics_v2/classes/generics_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class InstanceFilling {
public:
  explicit InstanceFilling(SemanticState &owner);

  void leaveFilling();
  void fillUnfilledInstances();
  void fillInstance(const FillInstance &task);
  void nextFilledField(const NextFilledField &task);
  void afterFilledField(const AfterFilledField &task);
  void linkInstanceBases(const LinkInstanceBases &task);
  void afterInstanceParent(const AfterInstanceParent &task);
  void nextInstanceInterface(const NextInstanceInterface &task);
  void afterInstanceInterface(const AfterInstanceInterface &task);

private:
  void addInventorySubtype(const Type *type);
  void startFilledField(ClassDecl *instance, size_t index);
  void addInstanceBase(ClassDecl *instance, const Type *base);

  SemanticState &state;
};

}
