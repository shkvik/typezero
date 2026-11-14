#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/class_facts/class_facts.h"
#include "compiler/semantic/code/classes/class_members/class_member_rules/class_member_rules.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class MemberTargets {
public:
  MemberTargets(SemanticState &owner, ClassFacts &sharedFacts,
                ClassMemberRules &sharedClassMemberRules);

  void checkFieldTarget(const CheckFieldTarget &task);
  void checkStaticTarget(const CheckStaticTarget &task);

private:
  using ResultFieldTypeStated = Result<const Type *, CodeClassesRegistryTypeDeclaredError>;
  using ResultStoredFieldValue = Result<const ClassDecl *, CodeClassesRegistryStatsDateError>;
  using ResultFieldOfTheClass = Result<const ClassField *, CodeClassesRegistryFieldFoundError>;
  using ResultFieldIsWritable = Result<const VarSymbol *, CodeClassesRegistryFieldWritableError>;
  using ResultFieldNotReadOnly = Result<const ClassField *, CodeClassesRegistryFieldWritableError>;
  using ResultAssignableMember =
      Result<const ClassField *, CodeClassesRegistryAssignableFieldError>;

  bool reportUnsetField(const MemberExpr *member, const ClassDecl *owner, bool isViaThis);


  const Type *staticTargetType(Expr *&target, ClassDecl *classDecl);
  const Type *fieldTargetType(MemberExpr *member, ClassDecl *classDecl, AssignForm form);


  ResultFieldIsWritable checkStaticFieldWritable(const VarSymbol *variable) const;
  ResultFieldTypeStated checkTypeDeclared(const VarSymbol *variable, llvm::StringRef name) const;
  ResultFieldOfTheClass checkFieldFound(const ClassField *found, const ClassDecl *classDecl,
                                        llvm::StringRef name) const;


  ResultStoredFieldValue checkStatsDate(ClassDecl *classDecl, MemberExpr *member);
  ResultFieldNotReadOnly checkFieldWritable(const ClassField *field, const ClassDecl *owner,
                                            bool isViaThis, const Type *type) const;
  ResultAssignableMember checkAssignableField(const ClassField *field, const ClassDecl *classDecl,
                                              llvm::StringRef name, const Type *type) const;

  ClassFacts &facts;
  SemanticState &state;
  ClassMemberRules &classMemberRules;
};

}
