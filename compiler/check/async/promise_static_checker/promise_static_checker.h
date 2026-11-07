#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class PromiseStaticChecker {
public:
  explicit PromiseStaticChecker(Checker &owner);

  bool checkPromiseList(Expr *&arg, const Type *wantedElement, const Type *&element,
                        std::vector<const Type *> &tuple, llvm::StringRef fn);
  Expr *checkPromiseStatic(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  static const Type *awaited(const Type *type);

  Checker &checker;
};

}
