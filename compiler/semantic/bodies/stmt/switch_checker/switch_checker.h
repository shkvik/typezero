#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/stmt/switch_checker/switch_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SwitchChecker {
public:
  explicit SwitchChecker(SemanticState &owner);

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

  SemanticState &state;
};

}
