#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/instance_of_checker/instance_of_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class CallChecker;
class ExprChecker;
class TopLevelChecker;

class InstanceOfChecker {
public:
  using ResultInstanceTested = Result<Expr *, InstanceOfCheckerCheckInstanceOfError>;

  InstanceOfChecker(SemanticState &owner, SemanticParts &all);

  ResultInstanceTested checkInstanceOf(BinaryExpr *bin);

private:
  using ResultBytesKindTested = Result<Expr *, InstanceOfCheckerCheckBytesInstanceOfError>;

  ResultBytesKindTested checkBytesInstanceOf(BinaryExpr *bin, const Type *bytes);

  SemanticState &state;
  Scopes *scopes = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  TopLevelChecker *topLevelChecker = nullptr;
};

}
