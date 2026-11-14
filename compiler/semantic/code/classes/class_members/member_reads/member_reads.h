#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/class_facts/class_facts.h"
#include "compiler/semantic/code/classes/class_members/class_member_rules/class_member_rules.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>
#include <vector>

namespace tz {

class MemberReads {
public:
  MemberReads(SemanticState &owner, ClassFacts &sharedFacts,
              ClassMemberRules &sharedClassMemberRules);

  void checkThis(const CheckThis &task);
  void classValueType(const ClassValueType &task);
  void checkClassValue(const CheckClassValue &task);
  void checkClassMember(const CheckClassMember &task);
  void afterMethodVariants(const AfterMethodVariants &task);
  void checkOverloadMember(const CheckOverloadMember &task);
  void afterOverloadVariants(const AfterOverloadVariants &task);
  void afterMemberFunctionType(const AfterMemberFunctionType &task);

private:
  using ResultThisReadiness = Result<VarSymbol *, CodeClassesRegistryReadyThisError>;
  using ResultThisDeclared = Result<const VarSymbol *, CodeClassesRegistryCheckThisError>;
  using ResultThisComesFrom = Result<const FunctionDecl *, CodeClassesRegistryThisOfError>;
  using ResultObjectReadyToUse = Result<llvm::StringRef, CodeClassesRegistryObjectReadyError>;
  using ResultPlainMethodValue = Result<const FuncSymbol *, CodeClassesRegistryMethodValueError>;
  using ResultMethodsOfObjects =
      Result<std::vector<FunctionDecl *>, CodeClassesRegistryInstanceMethodsError>;

  void startMethodMember(MemberExpr *member, ClassDecl *classDecl, const Type *expected,
                         bool isViaThis);
  void pushFieldMember(MemberExpr *member, const ClassField *field, const ClassDecl *owner,
                       bool isViaThis);


  bool hasStaticField(const ClassDecl *classDecl, llvm::StringRef name) const;


  ResultThisReadiness checkReadyThis(VarSymbol *variable) const;
  ResultThisComesFrom checkThisOf(const FunctionDecl *current) const;


  ResultObjectReadyToUse checkObjectReady(bool isViaThis, llvm::StringRef name) const;
  ResultPlainMethodValue checkMethodValue(const FuncSymbol *function, llvm::StringRef name) const;
  ResultMethodsOfObjects checkInstanceMethods(const ClassDecl *classDecl,
                                              llvm::StringRef name) const;


  std::string thisMemberText() const;
  MethodMember methodForm(ClassDecl *classDecl, FuncSymbol *function) const;
  ResultThisDeclared checkThisDeclared(const FunctionDecl *source, const VarSymbol *variable) const;

  ClassFacts &facts;
  SemanticState &state;
  ClassMemberRules &classMemberRules;
};

}
