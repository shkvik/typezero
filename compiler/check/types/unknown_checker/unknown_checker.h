#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class UnknownChecker {
public:
  explicit UnknownChecker(Checker &checker);

  static bool isObjectReference(const Type *type);
  static bool holdsUnknown(const Type *type);
  std::string unknownHint(const Expr *expr, const Type *to);
  void reportUnknownUse(const Expr *object, const std::string &what);
  Expr *checkUnknownMember(MemberExpr *member);
  Expr *checkUnknownMethod(CallExpr *call, MemberExpr *member);
  void addUnknownFacts(const Expr *cond, Checker::Flow &whenTrue, Checker::Flow &whenFalse) const;

private:
  Checker &checker_;
};

}
