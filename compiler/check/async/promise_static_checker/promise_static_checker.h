#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Promise.all, allSettled, race, any, resolve, reject and withResolvers.
class PromiseStaticChecker {
public:
  explicit PromiseStaticChecker(Sema &sema);

  bool checkPromiseList(Expr *&arg, const Type *wantedElement, const Type *&element,
                        std::vector<const Type *> &tuple, llvm::StringRef fn);
  // Promise.resolve and the others; Promise is the object of `member`.
  Expr *checkPromiseStatic(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  Sema &sema_;
};

} // namespace tz
