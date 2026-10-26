#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class AssignChecker {
public:
  explicit AssignChecker(Checker &checker);

  Expr *checkAssign(AssignExpr *assign);
  Expr *checkLogicalAssign(AssignExpr *assign);
  Expr *checkUpdate(UpdateExpr *update);
  Expr *checkFieldLogicalAssign(AssignExpr *assign);

private:
  Checker &checker_;
  bool logicalAssign_ = false;
};

}
