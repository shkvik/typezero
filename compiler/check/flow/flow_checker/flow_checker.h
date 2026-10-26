#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class FlowChecker {
public:
  explicit FlowChecker(Checker &checker);

  static Checker::Flow join(const Checker::Flow &a, const Checker::Flow &b);
  void assigned(const VarSymbol *var, const Expr *value);
  void incremented(const VarSymbol *var, int64_t delta);
  void calledFunction();
  void forgetCallEffects(Checker::Flow &flow) const;
  bool changedByCalls(const VarSymbol *var) const;
  void arrayResized(const Type *type, bool grew, bool shrank, const VarSymbol *except = nullptr);
  void setDefined(const VarSymbol *var, bool defined);

private:
  Checker &checker_;
};

}
