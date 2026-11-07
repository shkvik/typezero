#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/classes/instance_of_checker/instance_of_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class InstanceOfChecker {
public:
  using ResultInstanceTested = Result<Expr *, InstanceOfCheckerCheckInstanceOfError>;

  explicit InstanceOfChecker(Checker &owner);

  ResultInstanceTested checkInstanceOf(BinaryExpr *bin);

private:
  using ResultBytesKindTested = Result<Expr *, InstanceOfCheckerCheckBytesInstanceOfError>;

  ResultBytesKindTested checkBytesInstanceOf(BinaryExpr *bin, const Type *bytes);

  Checker &checker;
};

}
