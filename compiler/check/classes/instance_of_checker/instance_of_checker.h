#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class InstanceOfChecker {
public:
  explicit InstanceOfChecker(Checker &checker);

  Expr *checkInstanceOf(BinaryExpr *bin);
  Expr *checkBytesInstanceOf(BinaryExpr *bin, const Type *bytes);

private:
  Checker &checker_;
};

}
