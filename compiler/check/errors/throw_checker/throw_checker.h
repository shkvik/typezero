#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ThrowChecker {
public:
  explicit ThrowChecker(Checker &checker);

  void declareErrorClass();
  bool isErrorType(const Type *type) const;
  void checkThrow(ThrowStmt *stmt);
  void checkTry(TryStmt *stmt);
  Expr *checkErrorStack(MemberExpr *member);
  Expr *checkErrorMethod(CallExpr *call, MemberExpr *member);
  Checker::Flow withoutChanges(const Checker::Flow &flow, const Checker::LoopScan &scan);
  ClassDecl *errorKindClass(llvm::StringRef name) const;

private:
  Checker &checker_;
};

}
