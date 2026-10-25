#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// new Map and new Set.
class DictNewChecker {
public:
  explicit DictNewChecker(Sema &sema);

  // `new Map(...)`, `new Set(...)`, Object.fromEntries, Map.groupBy and
  // Object.groupBy.
  bool inferPairs(Expr *arg, const Type *&key, const Type *&value);
  Expr *checkDictSource(Expr *arg, bool pairs, const Type *&element, llvm::StringRef what);
  Expr *checkDictNew(NewExpr *expr, const Type *expected);

private:
  Sema &sema_;
};

} // namespace tz
