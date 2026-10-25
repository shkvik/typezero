#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// What is known at each point: the variables that hold a value, and what calls change.
class FlowChecker {
public:
  explicit FlowChecker(Sema &sema);

  // What holds after control merges from `a` and `b`.
  static Sema::Flow join(const Sema::Flow &a, const Sema::Flow &b);
  // Records `var = value` (or a declaration with that initializer); `value`
  // is the checked value before its conversion to the variable's type.
  void assigned(const VarSymbol *var, const Expr *value);
  // Records `var += delta`, where delta is a constant.
  void incremented(const VarSymbol *var, int64_t delta);
  // A call to a user function may change globals, the length of any array,
  // and variables that closures assign to.
  void calledFunction();
  void forgetCallEffects(Sema::Flow &flow) const;
  bool changedByCalls(const VarSymbol *var) const;
  // Arrays of type `type` (any of which may be the one changed) may have
  // grown or shrunk; `except` is an array whose facts were already updated.
  void arrayResized(const Type *type, bool grew, bool shrank, const VarSymbol *except = nullptr);
  void setDefined(const VarSymbol *var, bool defined);

private:
  Sema &sema_;
};

} // namespace tz
