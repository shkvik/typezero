#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ClassMemberChecker {
public:
  explicit ClassMemberChecker(Checker &checker);

  bool checkAccess(const ClassDecl *cls, Access access, llvm::StringRef name, llvm::SMLoc loc,
                   const Expr *receiver = nullptr);
  Expr *checkClassMember(MemberExpr *member, const Type *expected);
  const Type *checkFieldTarget(MemberExpr *member, bool compound = false);

private:
  Checker &checker_;
};

}
