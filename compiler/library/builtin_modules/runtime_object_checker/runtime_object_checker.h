#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class RuntimeObjectChecker {
public:
  explicit RuntimeObjectChecker(Checker &checker);

  Expr *signalOption(Expr *value, llvm::SMLoc loc);
  Expr *checkRuntimeMember(MemberExpr *member);
  Expr *checkRuntimeMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  const Type *checkRuntimeTarget(MemberExpr *member, bool compound);
  static std::string runtimeMembers(const Type *type);

private:
  Checker &checker_;
};

}
