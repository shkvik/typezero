#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/class_checker/class_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ClassLinks;
class SemanticParts;
class LateClasses;
class ClassDeclarer;
class TypeRelations;
class GenericChecker;
class InheritChecker;
class ReturnInferrer;
class FunctionChecker;
class NestedFunctions;
class DecoratorChecker;

class ClassChecker {
public:
  using ResultThisReference = Result<Expr *, ClassCheckerCheckThisError>;
  using ResultFieldUnsetYet = Result<std::string, ClassCheckerFirstUnsetError>;
  using ResultNewThisSymbol = Result<VarSymbol *, ClassCheckerDeclareThisError>;
  using ResultClassDeclared = Result<ClassDecl *, ClassCheckerCheckClassStmtError>;
  using ResultInitThisFound = Result<const NameExpr *, ClassCheckerIsInitThisError>;
  using ResultObjectIsReady = Result<const Flow *, ClassCheckerThisReadyError>;
  using ResultHasDecorators = Result<const ClassDecl *, ClassCheckerHasDecoratorsError>;
  using ResultEveryFieldSet = Result<const FunctionDecl *, ClassCheckerCheckFieldsSetError>;

  ClassChecker(SemanticState &owner, SemanticParts &all);

  void walkClassBodies(ClassDecl *classDecl);
  ResultEveryFieldSet checkFieldsSet() const;
  ResultThisReference checkThis(NameExpr *name);
  ResultNewThisSymbol declareThis(FunctionDecl *functionDecl);
  ResultClassDeclared checkClassStmt(ClassDecl *classDecl);
  ResultInitThisFound isInitThis(const Expr *expr) const;
  ResultHasDecorators hasDecorators(const ClassDecl *classDecl) const;
  ResultObjectIsReady checkThisReady(const Flow &flow) const;
  ResultFieldUnsetYet firstUnset(const Flow &flow) const;

private:
  using ResultThisReadiness = Result<VarSymbol *, ClassCheckerReadyThisError>;
  using ResultThisComesFrom = Result<const FunctionDecl *, ClassCheckerThisOfError>;
  using ResultFieldHasValue = Result<const ClassField *, ClassCheckerFieldStartValueError>;
  using ResultReturnMatches = Result<const FunctionDecl *, ClassCheckerOverrideReturnError>;

  bool isDecoratedFunction(const FunctionDecl *functionDecl) const;
  bool isDecoratedParam(const Param &param) const;
  bool isDecoratedField(const ClassField &field) const;
  ResultFieldHasValue checkFieldStartValue(const ClassDecl *classDecl,
                                           const ClassField &field) const;
  ResultReturnMatches checkOverrideReturn(const ClassDecl *classDecl, const FunctionDecl *method);
  void walkFieldStarts(const ClassDecl *classDecl);
  std::vector<FunctionDecl *> classMethods(const ClassDecl *classDecl) const;
  void walkClassMethod(FunctionDecl *functionDecl);
  void walkOverrideReturns(const ClassDecl *classDecl);
  bool isUnsetField(const Flow &flow, const ClassField &field) const;
  ResultThisComesFrom checkThisOf(const FunctionDecl *current) const;
  std::string thisMemberText() const;
  ResultThisReadiness checkReadyThis(VarSymbol *variable) const;

  SemanticState &state;
  ClassLinks *classLinks = nullptr;
  LateClasses *lateClasses = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TypeRelations *typeRelations = nullptr;
  GenericChecker *genericChecker = nullptr;
  InheritChecker *inheritChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  FunctionChecker *functionChecker = nullptr;
  NestedFunctions *nestedFunctions = nullptr;
  DecoratorChecker *decoratorChecker = nullptr;
};

}
