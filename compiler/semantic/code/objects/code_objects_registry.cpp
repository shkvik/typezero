#include "compiler/semantic/code/objects/code_objects_registry.h"

namespace tz {

CodeObjectsRegistry::CodeObjectsRegistry(SemanticState &owner)
    : keyedFields(owner),
      objectFields(owner) {}

void CodeObjectsRegistry::checkEnumObject(const CheckEnumObject &task) {
  return this->objectFields.checkEnumObject(task);
}

void CodeObjectsRegistry::afterEnumObjectResolved(const AfterEnumObjectResolved &task) {
  return this->objectFields.afterEnumObjectResolved(task);
}

void CodeObjectsRegistry::rememberEnumObject(const RememberEnumObject &task) {
  return this->objectFields.rememberEnumObject(task);
}

void CodeObjectsRegistry::checkField(const CheckField &task) {
  return this->objectFields.checkField(task);
}

void CodeObjectsRegistry::checkFieldCall(const CheckFieldCall &task) {
  return this->objectFields.checkFieldCall(task);
}

void CodeObjectsRegistry::afterFieldCallee(const AfterFieldCallee &task) {
  return this->objectFields.afterFieldCallee(task);
}

void CodeObjectsRegistry::checkObjectFieldTarget(const CheckObjectFieldTarget &task) {
  return this->objectFields.checkObjectFieldTarget(task);
}

void CodeObjectsRegistry::checkKeyedField(const CheckKeyedField &task) {
  return this->keyedFields.checkKeyedField(task);
}

void CodeObjectsRegistry::afterTupleKey(const AfterTupleKey &task) {
  return this->keyedFields.afterTupleKey(task);
}

void CodeObjectsRegistry::afterObjectKey(const AfterObjectKey &task) {
  return this->keyedFields.afterObjectKey(task);
}

void CodeObjectsRegistry::objectKeyReady(const ObjectKeyReady &task) {
  return this->keyedFields.objectKeyReady(task);
}

}
