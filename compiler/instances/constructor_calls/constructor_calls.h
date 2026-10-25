#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The constructors a program calls, checked once every class is known.
class ConstructorCalls {
public:
  explicit ConstructorCalls(Checker &checker);

  // Reports a method that a constructor calls on `this` (directly or through
  // other methods) and that a class down the chain overrides: it would run
  // before that class has set its fields.
  void checkConstructorCalls();
  void scanConstructorCalls(const FunctionDecl *fn, const ClassDecl *cls, const CallExpr *origin,
                            const FunctionDecl *ctor,
                            llvm::SmallPtrSetImpl<const FunctionDecl *> &seen);

private:
  Checker &checker_;
  // The calls of constructors reported by checkConstructorCalls.
  llvm::SmallPtrSet<const CallExpr *, 4> reportedCalls_;
};

} // namespace tz
