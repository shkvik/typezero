#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// What is known at each point: the variables that hold a value, and what calls change.
class FlowChecker {
public:
  explicit FlowChecker(Checker &checker);

  // What holds after control merges from `a` and `b`.
  static Checker::Flow join(const Checker::Flow &a, const Checker::Flow &b);
  // Records `var = value` (or a declaration with that initializer); `value`
  // is the checked value before its conversion to the variable's type.
  void assigned(const VarSymbol *var, const Expr *value);
  // Records `var += delta`, where delta is a constant.
  void incremented(const VarSymbol *var, int64_t delta);
  // A call to a user function may change globals, the length of any array,
  // and variables that closures assign to.
  void calledFunction();
  void forgetCallEffects(Checker::Flow &flow) const;
  bool changedByCalls(const VarSymbol *var) const;
  // Arrays of type `type` (any of which may be the one changed) may have
  // grown or shrunk; `except` is an array whose facts were already updated.
  void arrayResized(const Type *type, bool grew, bool shrank, const VarSymbol *except = nullptr);
  void setDefined(const VarSymbol *var, bool defined);

private:
  Checker &checker_;
};

} // namespace tz
