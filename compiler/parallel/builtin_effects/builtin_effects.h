#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

// What the calls of built-ins read and write, and the callbacks they run.
class BuiltinEffects {
public:
  explicit BuiltinEffects(Analysis &analysis);

  void builtinEffects(const CallExpr *c, const Expr *recv);

private:
  Analysis &analysis_;
};

} // namespace tz::parallel
