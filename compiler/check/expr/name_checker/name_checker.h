#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/expr/name_checker/name_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class NameChecker {
public:
  explicit NameChecker(Checker &owner);

  Expr *checkName(NameExpr *name);

private:
  using ResultResolvedNameUse = Result<Expr *, NameCheckerUnknownNameError>;
  using ResultVariableVisible = Result<const NameExpr *, NameCheckerNotCapturedError>;
  using ResultNoTypeArguments = Result<const NameExpr *, NameCheckerTypeArgsAbsentError>;

  ResultResolvedNameUse unknownName(NameExpr *name, const ScopeEntry &entry);
  ResultVariableVisible notCaptured(const NameExpr *name, const ScopeEntry &entry) const;
  static ResultNoTypeArguments typeArgsAbsent(const NameExpr *name, const ScopeEntry &entry);

  Checker &checker;
};

}
