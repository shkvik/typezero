#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Promise.all, allSettled, race, any, resolve, reject and withResolvers.
class PromiseStaticChecker {
public:
  explicit PromiseStaticChecker(Checker &checker);

  bool checkPromiseList(Expr *&arg, const Type *wantedElement, const Type *&element,
                        std::vector<const Type *> &tuple, llvm::StringRef fn);
  // Promise.resolve and the others; Promise is the object of `member`.
  Expr *checkPromiseStatic(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  Checker &checker_;
};

} // namespace tz
