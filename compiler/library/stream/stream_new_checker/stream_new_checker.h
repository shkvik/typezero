#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class StreamNewChecker {
public:
  explicit StreamNewChecker(Checker &checker);

  Expr *checkStreamNew(NewExpr *expr, ClassDecl *generic, const Type *expected);

private:
  Checker &checker_;
};

}
