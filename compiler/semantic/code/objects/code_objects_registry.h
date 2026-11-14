#pragma once

#include "compiler/semantic/code/objects/keyed_fields/keyed_fields.h"
#include "compiler/semantic/code/objects/object_fields/object_fields.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeObjectsRegistry {
public:
  explicit CodeObjectsRegistry(SemanticState &owner);

  void checkField(const CheckField &task);
  void afterTupleKey(const AfterTupleKey &task);
  void checkFieldCall(const CheckFieldCall &task);
  void afterObjectKey(const AfterObjectKey &task);
  void objectKeyReady(const ObjectKeyReady &task);
  void checkEnumObject(const CheckEnumObject &task);
  void checkKeyedField(const CheckKeyedField &task);
  void afterFieldCallee(const AfterFieldCallee &task);
  void rememberEnumObject(const RememberEnumObject &task);
  void checkObjectFieldTarget(const CheckObjectFieldTarget &task);
  void afterEnumObjectResolved(const AfterEnumObjectResolved &task);

private:
  KeyedFields keyedFields;
  ObjectFields objectFields;
};

}
