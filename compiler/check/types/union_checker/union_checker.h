#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/types/union_checker/union_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class UnionChecker {
public:
  using ResultUnionMemberTyped = Result<Expr *, UnionCheckerCheckUnionMemberError>;
  using ResultUnionMethodTyped = Result<Expr *, UnionCheckerCheckUnionMethodCallError>;
  using ResultUnionFieldTarget = Result<const Type *, UnionCheckerCheckUnionFieldTargetError>;

  explicit UnionChecker(Checker &owner);

  ResultUnionMemberTyped checkUnionMember(MemberExpr *member);
  ResultUnionFieldTarget checkUnionFieldTarget(MemberExpr *member);
  const Type *commonBaseWith(const Type *type, llvm::StringRef name) const;
  ResultUnionMethodTyped checkUnionMethodCall(CallExpr *call, MemberExpr *member);
  std::optional<std::vector<const Type *>> unhandledCases(const SwitchStmt *stmt) const;

private:
  static bool isLiteralSet(const Type *type);
  static llvm::ArrayRef<const Type *> membersOf(const Type *&type);
  static std::string lackingHint(const MemberExpr *member, const Type *type);

  Checker &checker;
};

}
