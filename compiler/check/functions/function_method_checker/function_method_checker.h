#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// f.call and f.apply of functions with a this parameter, and the methods of parameter
// lists.
class FunctionMethodChecker {
public:
  explicit FunctionMethodChecker(Checker &checker);

  // Decorators (sema_decorate.cpp, docs/decorators.md), and what they need:
  // functions with a `this` parameter, called with `f.call(obj, ...)` and
  // `f.apply(obj, args)`, whose arguments are an argument list
  // (sema_arglist.cpp); classes as values.
  Expr *checkThisCall(CallExpr *call, MemberExpr *member);
  // `args.length` and `args.join(sep)` of a list of parameters in generic
  // code; `t.join(sep)` of a tuple.
  Expr *checkListMethod(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

} // namespace tz
