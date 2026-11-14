#pragma once

#include "compiler/semantic/code/assignments/assign_targets/assign_targets.h"
#include "compiler/semantic/code/assignments/assign_values/assign_values.h"
#include "compiler/semantic/code/assignments/assignment_rules/assignment_rules.h"
#include "compiler/semantic/code/assignments/nullish_assigns/nullish_assigns.h"
#include "compiler/semantic/code/assignments/updates_and_deletes/updates_and_deletes.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeAssignmentsRegistry {
public:
  explicit CodeAssignmentsRegistry(SemanticState &owner);

  void discardChecked();
  void checkAssign(const CheckAssign &task);
  void lengthFacts(const LengthFacts &task);
  void checkUpdate(const CheckUpdate &task);
  void compoundFacts(const CompoundFacts &task);
  void assignedField(const AssignedField &task);
  void assignedLength(const AssignedLength &task);
  void finishVarTarget(const FinishVarTarget &task);
  void answerVarTarget(const AnswerVarTarget &task);
  void afterPlainValue(const AfterPlainValue &task);
  void afterTargetAlias(const AfterTargetAlias &task);
  void afterKeyedTarget(const AfterKeyedTarget &task);
  void afterAssignValue(const AfterAssignValue &task);
  void checkDeleteValue(const CheckDeleteValue &task);
  void afterOtherDelete(const AfterOtherDelete &task);
  void afterIndexNullish(const AfterIndexNullish &task);
  void afterFieldNullish(const AfterFieldNullish &task);
  void afterAssignTarget(const AfterAssignTarget &task);
  void afterUpdateTarget(const AfterUpdateTarget &task);
  void afterMemberDelete(const AfterMemberDelete &task);
  void afterProcessAssign(const AfterProcessAssign &task);
  void afterLogicalAssign(const AfterLogicalAssign &task);
  void afterNullishHolder(const AfterNullishHolder &task);
  void varTargetTypeKnown(const VarTargetTypeKnown &task);
  void afterGenericTarget(const AfterGenericTarget &task);
  void afterArrayPosition(const AfterArrayPosition &task);
  void afterInferredField(const AfterInferredField &task);
  void checkAssignDivisor(const CheckAssignDivisor &task);
  void afterStringCompound(const AfterStringCompound &task);
  void afterIndexDeleteKey(const AfterIndexDeleteKey &task);
  void afterNullishVariable(const AfterNullishVariable &task);
  void afterNumericCompound(const AfterNumericCompound &task);
  void finishNullishVariable(const FinishNullishVariable &task);
  void afterIndexTargetObject(const AfterIndexTargetObject &task);
  void afterIndexDeleteObject(const AfterIndexDeleteObject &task);
  void afterRecordFieldNullish(const AfterRecordFieldNullish &task);
  void afterTypedArrayPosition(const AfterTypedArrayPosition &task);
  void afterMemberTargetObject(const AfterMemberTargetObject &task);

private:
  AssignmentRules assignmentRules;
  AssignValues assignValues;
  AssignTargets assignTargets;
  NullishAssigns nullishAssigns;
  UpdatesAndDeletes updatesAndDeletes;
};

}
