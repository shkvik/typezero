#include "compiler/semantic/code/classes/class_members/class_members.h"

namespace tz {

ClassMembers::ClassMembers(SemanticState &owner, ClassFacts &sharedFacts)
    : classMemberRules(owner, sharedFacts),
      memberReads(owner, sharedFacts, this->classMemberRules),
      memberTargets(owner, sharedFacts, this->classMemberRules),
      instanceOfChecks(owner),
      staticMemberReads(owner, sharedFacts, this->classMemberRules) {}

void ClassMembers::classValueType(const ClassValueType &task) {
  return this->memberReads.classValueType(task);
}

void ClassMembers::checkClassValue(const CheckClassValue &task) {
  return this->memberReads.checkClassValue(task);
}

void ClassMembers::checkThis(const CheckThis &task) {
  return this->memberReads.checkThis(task);
}

void ClassMembers::checkClassMember(const CheckClassMember &task) {
  return this->memberReads.checkClassMember(task);
}

void ClassMembers::afterMethodVariants(const AfterMethodVariants &task) {
  return this->memberReads.afterMethodVariants(task);
}

void ClassMembers::afterMemberFunctionType(const AfterMemberFunctionType &task) {
  return this->memberReads.afterMemberFunctionType(task);
}

void ClassMembers::checkStaticMember(const CheckStaticMember &task) {
  return this->staticMemberReads.checkStaticMember(task);
}

void ClassMembers::afterStaticVariants(const AfterStaticVariants &task) {
  return this->staticMemberReads.afterStaticVariants(task);
}

void ClassMembers::afterNameFunctionType(const AfterNameFunctionType &task) {
  return this->staticMemberReads.afterNameFunctionType(task);
}

void ClassMembers::checkClassRefMember(const CheckClassRefMember &task) {
  return this->staticMemberReads.checkClassRefMember(task);
}

void ClassMembers::afterClassRefStatic(const AfterClassRefStatic &task) {
  return this->staticMemberReads.afterClassRefStatic(task);
}

void ClassMembers::checkOverloadMember(const CheckOverloadMember &task) {
  return this->memberReads.checkOverloadMember(task);
}

void ClassMembers::afterOverloadVariants(const AfterOverloadVariants &task) {
  return this->memberReads.afterOverloadVariants(task);
}

void ClassMembers::checkFieldTarget(const CheckFieldTarget &task) {
  return this->memberTargets.checkFieldTarget(task);
}

void ClassMembers::checkStaticTarget(const CheckStaticTarget &task) {
  return this->memberTargets.checkStaticTarget(task);
}

void ClassMembers::checkInstanceOfOperator(const CheckInstanceOfOperator &task) {
  return this->instanceOfChecks.checkInstanceOfOperator(task);
}

void ClassMembers::afterInstanceOfLeft(const AfterInstanceOfLeft &task) {
  return this->instanceOfChecks.afterInstanceOfLeft(task);
}

void ClassMembers::instanceOfRight(const InstanceOfRight &task) {
  return this->instanceOfChecks.instanceOfRight(task);
}

void ClassMembers::afterInstanceOfRight(const AfterInstanceOfRight &task) {
  return this->instanceOfChecks.afterInstanceOfRight(task);
}

}
