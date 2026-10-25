#pragma once

#include "compiler/lower/errors/throw_scanner/throw_scanner.h"

namespace tz {

// Which calls may throw: of functions, values and built-ins.
class ThrowCalls {
public:
  explicit ThrowCalls(ThrowScanner &scanner);

  void intrinsic(bool caught);
  void resumes(bool caught);
  void callee(const FunctionDecl *fn, bool caught, const CallExpr *site);
  void valueCall(const Type *type, bool caught);
  void value(const Type *type, const FunctionDecl *fn);
  // A function a built-in calls during the call.
  void callback(const Expr *arg, bool caught);
  void target(const Expr *target, bool caught);
  // An integer `/` or `%` by what may be zero, an integer `**` (0 ** -1),
  // and the arithmetic of decimals, which may divide by zero or grow too
  // large.
  static bool operatorThrows(BinaryOp op, const Type *type, const Expr *rhs);
  void call(const CallExpr *c, bool caught);

private:
  ThrowScanner &scanner_;
};

} // namespace tz
