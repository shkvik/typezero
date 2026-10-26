#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class DictNewChecker {
public:
  explicit DictNewChecker(Checker &checker);

  bool inferPairs(Expr *arg, const Type *&key, const Type *&value);
  Expr *checkDictSource(Expr *arg, bool pairs, const Type *&element, llvm::StringRef what);
  Expr *checkDictNew(NewExpr *expr, const Type *expected);

private:
  Checker &checker_;
};

}
