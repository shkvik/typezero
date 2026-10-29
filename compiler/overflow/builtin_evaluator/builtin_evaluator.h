#pragma once

#include "compiler/overflow/overflow_check/overflow_check.h"

namespace tz {

class BuiltinEvaluator {
public:
  explicit BuiltinEvaluator(OverflowCheck &check);

  Val evalBuiltin(const CallExpr *call, State &st);
  Val evalMath(const CallExpr *call, State &st);
  void checkDateParts(const CallExpr *call, const std::vector<Val> &args);

private:
  OverflowCheck &check_;
};

}
