#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class UnionChecker {
public:
  explicit UnionChecker(Checker &owner);

  Expr *checkUnionMember(MemberExpr *member);
  const Type *checkUnionFieldTarget(MemberExpr *member);
  Expr *checkUnionMethodCall(CallExpr *call, MemberExpr *member);
  const Type *commonBaseWith(const Type *type, llvm::StringRef name) const;
  std::optional<std::vector<const Type *>> unhandledCases(const SwitchStmt *stmt) const;

private:
  static llvm::ArrayRef<const Type *> membersOf(const Type *&type);
  static bool isLiteralSet(const Type *type);

  Checker &checker;
};

}
