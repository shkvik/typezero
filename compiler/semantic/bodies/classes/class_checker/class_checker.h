#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/class_checker/class_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

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

  void checkClassBodies(ClassDecl *cls);
  ResultEveryFieldSet checkFieldsSet() const;
  ResultThisReference checkThis(NameExpr *name);
  ResultNewThisSymbol declareThis(FunctionDecl *fn);
  ResultClassDeclared checkClassStmt(ClassDecl *cls);
  ResultInitThisFound isInitThis(const Expr *expr) const;
  ResultHasDecorators hasDecorators(const ClassDecl *cls) const;
  ResultObjectIsReady thisReady(const Flow &flow) const;
  ResultFieldUnsetYet firstUnset(const Flow &flow) const;

private:
  using ResultThisReadiness = Result<VarSymbol *, ClassCheckerReadyThisError>;
  using ResultThisComesFrom = Result<const FunctionDecl *, ClassCheckerThisOfError>;
  using ResultFieldHasValue = Result<const ClassField *, ClassCheckerFieldStartValueError>;
  using ResultReturnMatches = Result<const FunctionDecl *, ClassCheckerOverrideReturnError>;

  ResultThisReadiness readyThis(VarSymbol *var) const;
  ResultThisComesFrom thisOf(const FunctionDecl *current) const;
  ResultReturnMatches overrideReturn(const ClassDecl *cls, const FunctionDecl *method);
  ResultFieldHasValue fieldStartValue(const ClassDecl *cls, const ClassField &field) const;

  SemanticState &state;
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
