#include "compiler/semantic/code/assignments/code_assignments_registry.h"

namespace tz {

CodeAssignmentsRegistry::CodeAssignmentsRegistry(SemanticState &owner)
    : assignmentRules(owner),
      assignValues(owner, this->assignmentRules),
      assignTargets(owner, this->assignmentRules),
      nullishAssigns(owner, this->assignmentRules),
      updatesAndDeletes(owner, this->assignmentRules) {}

void CodeAssignmentsRegistry::checkAssign(const CheckAssign &task) {
  return this->assignTargets.checkAssign(task);
}

void CodeAssignmentsRegistry::afterTargetAlias(const AfterTargetAlias &task) {
  return this->assignTargets.afterTargetAlias(task);
}

void CodeAssignmentsRegistry::afterProcessAssign(const AfterProcessAssign &task) {
  return this->assignTargets.afterProcessAssign(task);
}

void CodeAssignmentsRegistry::afterLogicalAssign(const AfterLogicalAssign &task) {
  return this->nullishAssigns.afterLogicalAssign(task);
}

void CodeAssignmentsRegistry::afterNullishHolder(const AfterNullishHolder &task) {
  return this->nullishAssigns.afterNullishHolder(task);
}

void CodeAssignmentsRegistry::afterRecordFieldNullish(const AfterRecordFieldNullish &task) {
  return this->nullishAssigns.afterRecordFieldNullish(task);
}

void CodeAssignmentsRegistry::afterIndexNullish(const AfterIndexNullish &task) {
  return this->nullishAssigns.afterIndexNullish(task);
}

void CodeAssignmentsRegistry::afterFieldNullish(const AfterFieldNullish &task) {
  return this->nullishAssigns.afterFieldNullish(task);
}

void CodeAssignmentsRegistry::afterNullishVariable(const AfterNullishVariable &task) {
  return this->nullishAssigns.afterNullishVariable(task);
}

void CodeAssignmentsRegistry::finishNullishVariable(const FinishNullishVariable &task) {
  return this->nullishAssigns.finishNullishVariable(task);
}

void CodeAssignmentsRegistry::finishVarTarget(const FinishVarTarget &task) {
  return this->assignTargets.finishVarTarget(task);
}

void CodeAssignmentsRegistry::varTargetTypeKnown(const VarTargetTypeKnown &task) {
  return this->assignTargets.varTargetTypeKnown(task);
}

void CodeAssignmentsRegistry::discardChecked() {
  return this->assignTargets.discardChecked();
}

void CodeAssignmentsRegistry::answerVarTarget(const AnswerVarTarget &task) {
  return this->assignTargets.answerVarTarget(task);
}

void CodeAssignmentsRegistry::afterIndexTargetObject(const AfterIndexTargetObject &task) {
  return this->assignTargets.afterIndexTargetObject(task);
}

void CodeAssignmentsRegistry::afterGenericTarget(const AfterGenericTarget &task) {
  return this->assignTargets.afterGenericTarget(task);
}

void CodeAssignmentsRegistry::afterTypedArrayPosition(const AfterTypedArrayPosition &task) {
  return this->assignTargets.afterTypedArrayPosition(task);
}

void CodeAssignmentsRegistry::afterKeyedTarget(const AfterKeyedTarget &task) {
  return this->assignTargets.afterKeyedTarget(task);
}

void CodeAssignmentsRegistry::afterArrayPosition(const AfterArrayPosition &task) {
  return this->assignTargets.afterArrayPosition(task);
}

void CodeAssignmentsRegistry::afterMemberTargetObject(const AfterMemberTargetObject &task) {
  return this->assignTargets.afterMemberTargetObject(task);
}

void CodeAssignmentsRegistry::afterAssignTarget(const AfterAssignTarget &task) {
  return this->assignValues.afterAssignTarget(task);
}

void CodeAssignmentsRegistry::afterInferredField(const AfterInferredField &task) {
  return this->assignValues.afterInferredField(task);
}

void CodeAssignmentsRegistry::afterPlainValue(const AfterPlainValue &task) {
  return this->assignValues.afterPlainValue(task);
}

void CodeAssignmentsRegistry::afterStringCompound(const AfterStringCompound &task) {
  return this->assignValues.afterStringCompound(task);
}

void CodeAssignmentsRegistry::afterNumericCompound(const AfterNumericCompound &task) {
  return this->assignValues.afterNumericCompound(task);
}

void CodeAssignmentsRegistry::checkAssignDivisor(const CheckAssignDivisor &task) {
  return this->assignValues.checkAssignDivisor(task);
}

void CodeAssignmentsRegistry::compoundFacts(const CompoundFacts &task) {
  return this->assignValues.compoundFacts(task);
}

void CodeAssignmentsRegistry::afterAssignValue(const AfterAssignValue &task) {
  return this->assignValues.afterAssignValue(task);
}

void CodeAssignmentsRegistry::assignedLength(const AssignedLength &task) {
  return this->assignValues.assignedLength(task);
}

void CodeAssignmentsRegistry::lengthFacts(const LengthFacts &task) {
  return this->assignValues.lengthFacts(task);
}

void CodeAssignmentsRegistry::assignedField(const AssignedField &task) {
  return this->assignValues.assignedField(task);
}

void CodeAssignmentsRegistry::checkUpdate(const CheckUpdate &task) {
  return this->updatesAndDeletes.checkUpdate(task);
}

void CodeAssignmentsRegistry::afterUpdateTarget(const AfterUpdateTarget &task) {
  return this->updatesAndDeletes.afterUpdateTarget(task);
}

void CodeAssignmentsRegistry::checkDeleteValue(const CheckDeleteValue &task) {
  return this->updatesAndDeletes.checkDeleteValue(task);
}

void CodeAssignmentsRegistry::afterMemberDelete(const AfterMemberDelete &task) {
  return this->updatesAndDeletes.afterMemberDelete(task);
}

void CodeAssignmentsRegistry::afterOtherDelete(const AfterOtherDelete &task) {
  return this->updatesAndDeletes.afterOtherDelete(task);
}

void CodeAssignmentsRegistry::afterIndexDeleteObject(const AfterIndexDeleteObject &task) {
  return this->updatesAndDeletes.afterIndexDeleteObject(task);
}

void CodeAssignmentsRegistry::afterIndexDeleteKey(const AfterIndexDeleteKey &task) {
  return this->updatesAndDeletes.afterIndexDeleteKey(task);
}

}
