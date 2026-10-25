#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The type of a literal from where it goes.
class Destination {
public:
  explicit Destination(Sema &sema);

  // The type for the literal that initializes `d`, or null. `later` are the
  // declarators after it in the same declaration.
  const Type *literalDestination(const VarDeclarator &d, llvm::ArrayRef<VarDeclarator> later);
  // A literal whose fields may take their types from where it goes: an object
  // or array literal, `?:` with one, or map's callback returning one.
  static bool literalShaped(const Expr *expr);
  // Whether the literal names the fields of `type`, judged before it is
  // checked; a type that does not fit leaves the error to the place where
  // the object goes.
  bool literalFits(const Expr *expr, const Type *type) const;
  // The type of a function declared in a block the scan enters, from its
  // annotations: what its arguments go to.
  const Type *declaredFunctionType(const FunctionDecl *fn);

private:
  Sema &sema_;
};

} // namespace tz
