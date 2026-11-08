#pragma once

#include "compiler/parallel/analysis/analysis_types.h"
#include "compiler/program/ast/ast.h"

namespace tz::parallel {

class BuiltinEffects {
public:
  explicit BuiltinEffects(Analysis &analysis);

  void builtinEffects(const CallExpr *c, const Expr *recv);

private:
  Analysis &analysis;
};

}
