#pragma once

#include "compiler/memory/cycle_scanner/cycle_scanner.h"

namespace tz {

// The types expressions put into one another.
class CycleExprs {
public:
  explicit CycleExprs(CycleScanner &scanner);

  void expr(const Expr *expr);
  void target(const Expr *target);
  // An assignment to a field: one that the construction of the object makes
  // does not close a cycle (see constructionWrite).
  void assigned(const AssignExpr *assign);
  // `this.f = v` in the construction of an object (a constructor or the
  // initializers of the fields), before `this` went anywhere, where v does
  // not hold it: f then points at objects made before this one.
  bool constructionWrite(const AssignExpr *assign);

private:
  CycleScanner &scanner_;
};

} // namespace tz
