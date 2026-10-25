#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The objects the runtime makes: their members and methods.
class RuntimeObjectChecker {
public:
  explicit RuntimeObjectChecker(Sema &sema);

  // The option `signal` of a call, an AbortSignal of the library
  // (library/events/web.tz) or undefined, or undefined for none: what the
  // runtime gets of it, its side of the signal (Builtin::LibNativeSignal).
  Expr *signalOption(Expr *value, llvm::SMLoc loc);
  // The properties, methods and assignments of the objects of the runtime
  // (Type::Runtime); none of them can be assigned to.
  Expr *checkRuntimeMember(MemberExpr *member);
  Expr *checkRuntimeMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  const Type *checkRuntimeTarget(MemberExpr *member, bool compound);
  static std::string runtimeMembers(const Type *type);

private:
  Sema &sema_;
};

} // namespace tz
