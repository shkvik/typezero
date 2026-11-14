#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/class_facts/class_facts.h"
#include "compiler/semantic/code/classes/class_members/class_member_rules/class_member_rules.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <vector>

namespace tz {

class StaticMemberReads {
public:
  StaticMemberReads(SemanticState &owner, ClassFacts &sharedFacts,
                    ClassMemberRules &sharedClassMemberRules);

  void checkStaticMember(const CheckStaticMember &task);
  void afterStaticVariants(const AfterStaticVariants &task);
  void checkClassRefMember(const CheckClassRefMember &task);
  void afterClassRefStatic(const AfterClassRefStatic &task);
  void afterNameFunctionType(const AfterNameFunctionType &task);

private:
  using ResultFieldValueIsSet = Result<const VarSymbol *, CodeClassesRegistryFieldReadyError>;
  using ResultStaticFieldType = Result<const Type *, CodeClassesRegistryStaticFieldTypedError>;
  using ResultClassRefReadable =
      Result<const MemberExpr *, CodeClassesRegistryCheckClassRefMemberError>;
  using ResultStaticFunctions =
      Result<std::vector<FunctionDecl *>, CodeClassesRegistryStaticMethodsError>;

  void startStaticMethodValue(MemberExpr *member, ClassDecl *classDecl, const Type *expected);
  void pushStaticFieldRead(MemberExpr *member, ClassDecl *classDecl, const ClassField &field,
                           const ClassDecl *owner);


  bool hasOwnNameMethod(const ClassDecl *classDecl) const;


  Expr *afterClassValue(Expr *object, Expr *result);
  Expr *classNameText(MemberExpr *member, const ClassDecl *classDecl);


  ResultFieldValueIsSet checkFieldReady(const ClassDecl *owner, const VarSymbol *variable) const;
  ResultStaticFunctions checkStaticMethods(const ClassDecl *classDecl, llvm::StringRef name) const;
  ResultStaticFieldType checkStaticFieldTyped(const ClassDecl *owner, const VarSymbol *variable,
                                              llvm::StringRef name) const;


  ResultClassRefReadable checkClassRefReadable(const MemberExpr *member,
                                               const ClassDecl *classDecl) const;

  ClassFacts &facts;
  SemanticState &state;
  ClassMemberRules &classMemberRules;
};

}
