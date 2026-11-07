#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/stmt/switch_checker/switch_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class SwitchChecker {
public:
  explicit SwitchChecker(Checker &owner);

  void checkSwitch(SwitchStmt *stmt);

private:
  using ResultValueComparable = Result<const Type *, SwitchCheckerValueTypeError>;
  using ResultTestsComparable = Result<const Type *, SwitchCheckerCaseTypeError>;
  using ResultCasesAllHandled = Result<const SwitchStmt *, SwitchCheckerCasesHandledError>;

  static bool isConstantTest(const Expr *test);
  static ResultValueComparable valueType(const Type *type);
  static ResultTestsComparable caseType(const Type *type, const Type *testType);
  static ResultCasesAllHandled casesHandled(const SwitchStmt *stmt,
                                            llvm::ArrayRef<const Type *> left);

  Checker &checker;
};

}
