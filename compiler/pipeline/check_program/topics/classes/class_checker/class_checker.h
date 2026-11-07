#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/classes/class_checker/class_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

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

  explicit ClassChecker(CheckState &owner);

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

  CheckState &state;
};

}
