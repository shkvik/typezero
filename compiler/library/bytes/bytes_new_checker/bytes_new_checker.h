#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class BytesNewChecker {
public:
  explicit BytesNewChecker(Checker &checker);

  Expr *checkBytesNew(NewExpr *expr, const Type *type);
  Expr *checkBytesValues(CallExpr *call, ArrayLitExpr *lit, const Type *type);
  bool checkBytesSource(Expr *source, const Type *target);

private:
  Checker &checker_;
};

}
