#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ConstructorCalls {
public:
  explicit ConstructorCalls(Checker &checker);

  void checkConstructorCalls();
  void scanConstructorCalls(const FunctionDecl *fn, const ClassDecl *cls, const CallExpr *origin,
                            const FunctionDecl *ctor,
                            llvm::SmallPtrSetImpl<const FunctionDecl *> &seen);

private:
  Checker &checker_;
  llvm::SmallPtrSet<const CallExpr *, 4> reportedCalls_;
};

}
