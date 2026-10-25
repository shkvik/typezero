#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The members of instances: reads, writes and access.
class ClassMemberChecker {
public:
  explicit ClassMemberChecker(Checker &checker);

  // Reports a private or protected member used outside its class.
  bool checkAccess(const ClassDecl *cls, Access access, llvm::StringRef name, llvm::SMLoc loc);
  // `o.x` and `o.m` of an instance: a field, or a method bound to o.
  Expr *checkClassMember(MemberExpr *member, const Type *expected);
  // The field `o.x = v` assigns to; for an instance of a class, null when
  // its type comes from this very assignment, in the initializers.
  const Type *checkFieldTarget(MemberExpr *member, bool compound = false);

private:
  Checker &checker_;
};

} // namespace tz
