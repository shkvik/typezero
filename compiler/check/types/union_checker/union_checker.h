#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class UnionChecker {
public:
  explicit UnionChecker(Checker &checker);

  Expr *checkUnionMember(MemberExpr *member);
  const Type *checkUnionFieldTarget(MemberExpr *member);
  Expr *checkUnionMethodCall(CallExpr *call, MemberExpr *member);
  const Type *commonBaseWith(const Type *type, llvm::StringRef name) const;
  std::optional<std::vector<const Type *>> unhandledCases(const SwitchStmt *stmt) const;

private:
  Checker &checker_;
};

}
