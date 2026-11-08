#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/name_checker/name_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class NameChecker {
public:
  explicit NameChecker(SemanticState &owner);

  Expr *checkName(NameExpr *name);

private:
  using ResultResolvedNameUse = Result<Expr *, NameCheckerUnknownNameError>;
  using ResultVariableVisible = Result<const NameExpr *, NameCheckerNotCapturedError>;
  using ResultNoTypeArguments = Result<const NameExpr *, NameCheckerTypeArgsAbsentError>;

  ResultResolvedNameUse unknownName(NameExpr *name, const ScopeEntry &entry);
  ResultVariableVisible notCaptured(const NameExpr *name, const ScopeEntry &entry) const;
  static ResultNoTypeArguments typeArgsAbsent(const NameExpr *name, const ScopeEntry &entry);

  SemanticState &state;
};

}
