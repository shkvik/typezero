#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/arithmetic/code_arithmetic_registry_errors.h"
#include "compiler/semantic/code/assignments/assignment_rules/assignment_rules.h"
#include "compiler/semantic/code/assignments/code_assignments_registry_errors.h"
#include "compiler/semantic/declaration/names/declaration_names_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class NullishAssigns {
public:
  NullishAssigns(SemanticState &owner, AssignmentRules &sharedAssignmentRules);

  void afterIndexNullish(const AfterIndexNullish &task);
  void afterFieldNullish(const AfterFieldNullish &task);
  void afterLogicalAssign(const AfterLogicalAssign &task);
  void afterNullishHolder(const AfterNullishHolder &task);
  void afterNullishVariable(const AfterNullishVariable &task);
  void finishNullishVariable(const FinishNullishVariable &task);
  void afterRecordFieldNullish(const AfterRecordFieldNullish &task);

private:
  using ResultFieldMayBeAbsent = Result<const Type *, CodeAssignmentsRegistryFieldOptionalError>;
  using ResultTargetIsVariable = Result<const Expr *, CodeAssignmentsRegistryVariableTargetError>;
  using ResultVariableOptional = Result<const Type *, CodeAssignmentsRegistryNullishOptionalError>;

  void defineFieldOf(const MemberExpr *member);


  bool isRecordHolder(const Expr *object) const;


  ResultTargetIsVariable checkVariableTarget(const Expr *target) const;
  ResultVariableOptional checkNullishOptional(const NameExpr *name, const Type *type) const;
  ResultFieldMayBeAbsent checkFieldOptional(const MemberExpr *member, const Type *type) const;


  const Type *assignedValueType(const Expr *value) const;

  SemanticState &state;
  AssignmentRules &assignmentRules;
};

}
