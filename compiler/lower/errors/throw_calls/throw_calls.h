#pragma once

#include "compiler/lower/errors/throw_scanner/throw_scanner.h"

namespace tz {

class ThrowCalls {
public:
  explicit ThrowCalls(ThrowScanner &scanner);

  void intrinsic(bool caught);
  void resumes(bool caught);
  void callee(const FunctionDecl *fn, bool caught, const CallExpr *site);
  void valueCall(const Type *type, bool caught);
  void value(const Type *type, const FunctionDecl *fn);
  void callback(const Expr *arg, bool caught);
  void target(const Expr *target, bool caught);
  static bool operatorThrows(BinaryOp op, const Type *type, const Expr *rhs);
  void call(const CallExpr *c, bool caught);

private:
  ThrowScanner &scanner_;
};

}
