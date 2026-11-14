#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/arithmetic/code_arithmetic_registry_errors.h"
#include "compiler/semantic/code/assignments/assignment_rules/assignment_rules.h"
#include "compiler/semantic/code/assignments/code_assignments_registry_errors.h"
#include "compiler/semantic/declaration/names/declaration_names_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class AssignTargets {
public:
  AssignTargets(SemanticState &owner, AssignmentRules &sharedAssignmentRules);

  void discardChecked();
  void checkAssign(const CheckAssign &task);
  void finishVarTarget(const FinishVarTarget &task);
  void answerVarTarget(const AnswerVarTarget &task);
  void afterTargetAlias(const AfterTargetAlias &task);
  void afterKeyedTarget(const AfterKeyedTarget &task);
  void afterProcessAssign(const AfterProcessAssign &task);
  void varTargetTypeKnown(const VarTargetTypeKnown &task);
  void afterGenericTarget(const AfterGenericTarget &task);
  void afterArrayPosition(const AfterArrayPosition &task);
  void afterIndexTargetObject(const AfterIndexTargetObject &task);
  void afterTypedArrayPosition(const AfterTypedArrayPosition &task);
  void afterMemberTargetObject(const AfterMemberTargetObject &task);

private:
  using ResultDefinedDictType = Result<const Type *, CodeAssignmentsRegistryDictDefinedError>;
  using ResultNoBytesProperty = Result<const Type *, CodeAssignmentsRegistryBytesMemberError>;
  using ResultIndexIsNotBytes = Result<const Type *, CodeAssignmentsRegistryIndexNotBytesError>;
  using ResultIndexIsWritable = Result<const Type *, CodeAssignmentsRegistryIndexWritableError>;
  using ResultObjectIsDefined = Result<const Type *, CodeAssignmentsRegistryObjectDefinedError>;
  using ResultFieldIsWritable = Result<const Type *, CodeAssignmentsRegistryMemberWritableError>;
  using ResultNameIsAssignable = Result<const NameExpr *, CodeAssignmentsRegistryNotImportedError>;
  using ResultVisibleVariable =
      Result<const VarSymbol *, DeclarationNamesRegistryCheckVisibleError>;
  using ResultVariableMutable =
      Result<const VarSymbol *, CodeAssignmentsRegistryVariableMutableError>;
  using ResultVarNotImported =
      Result<const VarSymbol *, CodeAssignmentsRegistryCheckImportAssignError>;

  void startLogicalAssign(AssignExpr *assign);
  void startNullishAssign(AssignExpr *assign);
  void walkVarTargetUse(NameExpr *name, VarSymbol *variable);
  void walkVariableWritable(const NameExpr *name, VarSymbol *variable);
  void startUnionTarget(MemberExpr *member, const Type *type, AssignForm form);
  void startPlainMemberTarget(MemberExpr *member, const Type *type, AssignForm form);


  bool isGenericIndex(const IndexExpr *index) const;
  bool unfoldNarrowedCompound(AssignExpr *assign, BinaryOp op);


  Expr *logicalRead(const Expr *target);
  Expr *indexRead(const IndexExpr *index);


  ResultIndexIsNotBytes checkIndexNotBytes(const Type *type) const;
  ResultObjectIsDefined checkObjectDefined(const Type *type) const;
  ResultIndexIsWritable checkIndexWritable(const Type *type) const;
  ResultDefinedDictType checkDictDefined(const Type *type, const MemberExpr *member) const;
  ResultNoBytesProperty checkBytesMember(const Type *type, const MemberExpr *member) const;
  ResultVisibleVariable checkVisible(const NameExpr *name, const VarSymbol *variable) const;
  ResultVariableMutable checkVariableMutable(const NameExpr *name, const VarSymbol *variable) const;
  ResultFieldIsWritable checkMemberWritable(const Type *type, const MemberExpr *member,
                                            AssignForm form) const;


  ResultNameIsAssignable checkNotImported(const NameExpr *name);
  ResultVarNotImported checkImportAssign(const NameExpr *name, const VarSymbol *variable) const;

  SemanticState &state;
  AssignmentRules &assignmentRules;
};

}
