#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/instance_of_checker/instance_of_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class InstanceOfChecker {
public:
  using ResultInstanceTested = Result<Expr *, InstanceOfCheckerCheckInstanceOfError>;

  explicit InstanceOfChecker(SemanticState &owner);

  ResultInstanceTested checkInstanceOf(BinaryExpr *bin);

private:
  using ResultBytesKindTested = Result<Expr *, InstanceOfCheckerCheckBytesInstanceOfError>;

  ResultBytesKindTested checkBytesInstanceOf(BinaryExpr *bin, const Type *bytes);

  SemanticState &state;
};

}
