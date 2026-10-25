#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The members of instances: reads, writes and access.
class ClassMemberChecker {
public:
  explicit ClassMemberChecker(Sema &sema);

  // Reports a private or protected member used outside its class.
  bool checkAccess(const ClassDecl *cls, Access access, llvm::StringRef name, llvm::SMLoc loc);
  // `o.x` and `o.m` of an instance: a field, or a method bound to o.
  Expr *checkClassMember(MemberExpr *member, const Type *expected);
  // The field `o.x = v` assigns to; for an instance of a class, null when
  // its type comes from this very assignment, in the initializers.
  const Type *checkFieldTarget(MemberExpr *member, bool compound = false);

private:
  Sema &sema_;
};

} // namespace tz
