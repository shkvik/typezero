#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// throw, try, catch and finally, and the class Error (docs/errors.md).
class ThrowChecker {
public:
  explicit ThrowChecker(Checker &checker);

  // Exceptions (sema_error.cpp, docs/errors.md). Error is a class of the
  // language that every file sees, declared before them (declareErrorClass).
  // TypeError, RangeError and SyntaxError are, until classes inherit, that
  // class too, with a name and a class number of their own (kErrorKinds).
  void declareErrorClass();
  // An Error, of any kind: what `throw` takes and `catch` gives. With
  // inheritance, also an instance of a class derived from Error.
  bool isErrorType(const Type *type) const;
  void checkThrow(ThrowStmt *stmt);
  void checkTry(TryStmt *stmt);
  // `e.stack` of an Error, which is made from the path of the error.
  Expr *checkErrorStack(MemberExpr *member);
  // `e.toString()`; null if `member` is not that.
  Expr *checkErrorMethod(CallExpr *call, MemberExpr *member);
  // `flow` without what code `scan` scanned may change.
  Checker::Flow withoutChanges(const Checker::Flow &flow, const Checker::LoopScan &scan);
  // The class of the built-in kind `name` (Error, TypeError...), or null.
  ClassDecl *errorKindClass(llvm::StringRef name) const;

private:
  Checker &checker_;
};

} // namespace tz
