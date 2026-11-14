#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/literals/code_literals_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>
#include <vector>

namespace tz {

class MethodLiterals {
public:
  explicit MethodLiterals(SemanticState &owner);

  void nextLiteralMember(const NextLiteralMember &task);
  void afterLiteralField(const AfterLiteralField &task);
  void walkLiteralMethod(const WalkLiteralMethod &task);
  void walkLiteralMethods(const WalkLiteralMethods &task);
  void matchLiteralTarget(const MatchLiteralTarget &task);
  void afterLiteralMethods(const AfterLiteralMethods &task);
  void afterLiteralMethodReturn(const AfterLiteralMethodReturn &task);

private:
  using ResultValueIsTyped = Result<const Type *, CodeLiteralsRegistryFieldValueError>;
  using ResultMethodSimple = Result<const FunctionDecl *, CodeLiteralsRegistryPlainMethodError>;
  using ResultMembersMatch = Result<const ObjectLitExpr *, CodeLiteralsRegistryMembersMatchError>;
  using ResultPropertyFits =
      Result<const ObjectLitExpr::Property *, CodeLiteralsRegistryPropertyFitsError>;

  void afterLiteralMembers(MethodLiteralDraft draft);
  void declareLiteralMethods(ClassDecl *classDecl, const Type *target);
  void pushLiteralMethodDeclaration(FunctionDecl *functionDecl, const Type *target);
  void addLiteralMethod(ClassDecl *classDecl, const ObjectLitExpr::Property &property);


  bool hasMethodNamed(const ClassDecl *classDecl, llvm::StringRef name) const;


  ResultMethodSimple checkPlainMethod(const FunctionDecl *functionDecl) const;
  ResultValueIsTyped checkFieldValue(const Type *valueType, llvm::StringRef label) const;
  ResultMembersMatch checkMembersMatch(const ObjectLitExpr *literal, const Type *target) const;
  ResultPropertyFits checkPropertyFits(const ObjectLitExpr::Property &property, const Type *target,
                                       const Type *methodType) const;


  std::string literalTypeName(const std::vector<Field> &fields,
                              llvm::ArrayRef<FunctionDecl *> methods) const;

  SemanticState &state;
};

}
