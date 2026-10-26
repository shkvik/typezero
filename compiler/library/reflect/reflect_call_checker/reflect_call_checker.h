#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ReflectCallChecker {
public:
  explicit ReflectCallChecker(Checker &checker);

  Expr *checkReflectArgs(Expr *args);
  FunctionDecl *knownReflectMethod(ClassDecl *cls, llvm::StringRef name, bool &several);
  const FunctionDecl *heldReflectMethod(const Expr *f);
  Expr *directReflectApply(CallExpr *call, const FunctionDecl *fn);
  Expr *checkReflectCall(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  Checker &checker_;
};

}
