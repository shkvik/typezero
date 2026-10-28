#pragma once

#include "compiler/memory/cycle_scanner/cycle_scanner.h"

namespace tz {

class CycleExprs {
public:
  explicit CycleExprs(CycleScanner &scanner);

  void expr(const Expr *expr);
  void target(const Expr *target);
  void assigned(const AssignExpr *assign);
  bool constructionWrite(const AssignExpr *assign);

private:
  void closure(const FunctionExpr *expr, bool held);

  CycleScanner &scanner_;
};

}
