#pragma once

#include "compiler/interprocedural/overflow/overflow_check/overflow_check.h"

namespace tz {

class BuiltinEvaluator {
public:
  explicit BuiltinEvaluator(OverflowCheck &check);

  Val evalBuiltin(const CallExpr *call, State &st);
  Val evalMath(const CallExpr *call, State &st);
  void checkDateParts(const CallExpr *call, const std::vector<Val> &args);

private:
  double monthStart(double months) const;
  Num dateTime(const std::array<Num, 7> &parts) const;
  OverflowCheck &check;
};

}
