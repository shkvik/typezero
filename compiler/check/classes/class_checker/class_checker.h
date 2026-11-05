#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/result/result/result.h"

namespace tz {

enum class ClassCheckerTopLevelClassErrorCode {
  NOT_TOP_LEVEL,
};

struct ClassCheckerTopLevelClassError {
  ClassCheckerTopLevelClassErrorCode code;
  std::string message;
};

enum class ClassCheckerFieldStartValueErrorCode {
  NO_START_VALUE,
  NEVER_SET,
};

struct ClassCheckerFieldStartValueError {
  ClassCheckerFieldStartValueErrorCode code;
  std::string message;
};

enum class ClassCheckerOverrideReturnErrorCode {
  RETURNS_OTHER_TYPE,
};

struct ClassCheckerOverrideReturnError {
  ClassCheckerOverrideReturnErrorCode code;
  std::string message;
};

enum class ClassCheckerFieldsSetErrorCode {
  FIELD_NOT_SET_ON_EVERY_PATH,
};

struct ClassCheckerFieldsSetError {
  ClassCheckerFieldsSetErrorCode code;
  std::string message;
};

enum class ClassCheckerThisOfErrorCode {
  OUTSIDE_METHOD,
  IN_STATIC_FIELD,
  IN_STATIC_METHOD,
  IN_PLAIN_FUNCTION,
};

struct ClassCheckerThisOfError {
  ClassCheckerThisOfErrorCode code;
  std::string message;
};

enum class ClassCheckerReadyThisErrorCode {
  IN_CLOSURE_BEFORE_FIELDS_SET,
  BEFORE_SUPER,
  BEFORE_FIELDS_SET,
};

struct ClassCheckerReadyThisError {
  ClassCheckerReadyThisErrorCode code;
  std::string message;
};

class ClassChecker {
public:
  explicit ClassChecker(Checker &checker);

  void checkClassBodies(ClassDecl *cls);
  void checkClassStmt(ClassDecl *cls);
  static bool hasDecorators(const ClassDecl *cls);
  Expr *checkThis(NameExpr *name);
  VarSymbol *declareThis(FunctionDecl *fn);
  bool thisReady(const Checker::Flow &flow) const;
  std::string firstUnset(const Checker::Flow &flow) const;
  bool isInitThis(const Expr *expr) const;
  void checkFieldsSet(llvm::SMLoc loc);

private:
  Result<ClassDecl *, ClassCheckerTopLevelClassError> topLevelClass(ClassDecl *cls) const;
  Result<const ClassField *, ClassCheckerFieldStartValueError>
  fieldStartValue(const ClassDecl *cls, const ClassField &field) const;
  Result<const FunctionDecl *, ClassCheckerOverrideReturnError>
  overrideReturn(const ClassDecl *cls, const FunctionDecl *method);
  Result<const FunctionDecl *, ClassCheckerFieldsSetError> fieldsSet() const;
  Result<const FunctionDecl *, ClassCheckerThisOfError> thisOf(const FunctionDecl *current) const;
  Result<VarSymbol *, ClassCheckerReadyThisError> readyThis(VarSymbol *var) const;

  Checker &checker_;
};

}
