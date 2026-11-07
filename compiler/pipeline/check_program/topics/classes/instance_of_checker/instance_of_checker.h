#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/classes/instance_of_checker/instance_of_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class InstanceOfChecker {
public:
  using ResultInstanceTested = Result<Expr *, InstanceOfCheckerCheckInstanceOfError>;

  explicit InstanceOfChecker(CheckState &owner);

  ResultInstanceTested checkInstanceOf(BinaryExpr *bin);

private:
  using ResultBytesKindTested = Result<Expr *, InstanceOfCheckerCheckBytesInstanceOfError>;

  ResultBytesKindTested checkBytesInstanceOf(BinaryExpr *bin, const Type *bytes);

  CheckState &state;
};

}
