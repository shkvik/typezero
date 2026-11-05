#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/classes/class_checker/class_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class ClassChecker {
public:
  explicit ClassChecker(Checker &owner);

  Expr *checkThis(NameExpr *name);
  void checkClassStmt(ClassDecl *cls);
  void checkFieldsSet(llvm::SMLoc loc);
  void checkClassBodies(ClassDecl *cls);
  bool isInitThis(const Expr *expr) const;
  VarSymbol *declareThis(FunctionDecl *fn);
  static bool hasDecorators(const ClassDecl *cls);
  bool thisReady(const Checker::Flow &flow) const;
  std::string firstUnset(const Checker::Flow &flow) const;

private:
  using ResultThisReadiness = Result<VarSymbol *, ClassCheckerReadyThisError>;
  using ResultTopLevelClass = Result<ClassDecl *, ClassCheckerTopLevelClassError>;
  using ResultThisComesFrom = Result<const FunctionDecl *, ClassCheckerThisOfError>;
  using ResultEveryFieldSet = Result<const FunctionDecl *, ClassCheckerFieldsSetError>;
  using ResultFieldHasValue = Result<const ClassField *, ClassCheckerFieldStartValueError>;
  using ResultReturnMatches = Result<const FunctionDecl *, ClassCheckerOverrideReturnError>;

  ResultEveryFieldSet fieldsSet() const;
  ResultThisReadiness readyThis(VarSymbol *var) const;
  ResultTopLevelClass topLevelClass(ClassDecl *cls) const;
  ResultThisComesFrom thisOf(const FunctionDecl *current) const;
  ResultReturnMatches overrideReturn(const ClassDecl *cls, const FunctionDecl *method);
  ResultFieldHasValue fieldStartValue(const ClassDecl *cls, const ClassField &field) const;

  Checker &checker;
};

}
