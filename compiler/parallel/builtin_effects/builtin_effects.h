#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

class BuiltinEffects {
public:
  explicit BuiltinEffects(Analysis &analysis);

  void builtinEffects(const CallExpr *c, const Expr *recv);

private:
  Analysis &analysis_;
};

}
