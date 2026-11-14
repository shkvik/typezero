#pragma once

#include "compiler/semantic/code/classes/class_facts/class_facts.h"
#include "compiler/semantic/code/classes/class_members/class_member_rules/class_member_rules.h"
#include "compiler/semantic/code/classes/class_members/instance_of_checks/instance_of_checks.h"
#include "compiler/semantic/code/classes/class_members/member_reads/member_reads.h"
#include "compiler/semantic/code/classes/class_members/member_targets/member_targets.h"
#include "compiler/semantic/code/classes/class_members/static_member_reads/static_member_reads.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class ClassMembers {
public:
  ClassMembers(SemanticState &owner, ClassFacts &sharedFacts);

  void checkThis(const CheckThis &task);
  void classValueType(const ClassValueType &task);
  void checkClassValue(const CheckClassValue &task);
  void instanceOfRight(const InstanceOfRight &task);
  void checkClassMember(const CheckClassMember &task);
  void checkFieldTarget(const CheckFieldTarget &task);
  void checkStaticMember(const CheckStaticMember &task);
  void checkStaticTarget(const CheckStaticTarget &task);
  void afterMethodVariants(const AfterMethodVariants &task);
  void afterStaticVariants(const AfterStaticVariants &task);
  void checkClassRefMember(const CheckClassRefMember &task);
  void afterClassRefStatic(const AfterClassRefStatic &task);
  void checkOverloadMember(const CheckOverloadMember &task);
  void afterInstanceOfLeft(const AfterInstanceOfLeft &task);
  void afterInstanceOfRight(const AfterInstanceOfRight &task);
  void afterNameFunctionType(const AfterNameFunctionType &task);
  void afterOverloadVariants(const AfterOverloadVariants &task);
  void afterMemberFunctionType(const AfterMemberFunctionType &task);
  void checkInstanceOfOperator(const CheckInstanceOfOperator &task);

private:
  ClassMemberRules classMemberRules;
  MemberReads memberReads;
  MemberTargets memberTargets;
  InstanceOfChecks instanceOfChecks;
  StaticMemberReads staticMemberReads;
};

}
