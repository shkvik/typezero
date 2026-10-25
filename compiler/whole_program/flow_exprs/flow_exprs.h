#pragma once

#include "compiler/whole_program/flows/flows.h"

namespace tz {

// The flows of expressions: the references each one holds.
class FlowExprs {
public:
  explicit FlowExprs(Flows &flows);

  static const Expr *ownExpr(const Expr *e);
  // What the value of `e` may hold or be: nothing for a string, a number,
  // or another type that holds no references, whatever it was made of.
  Refs expr(Expr *e);
  Refs exprRefs(Expr *e);

private:
  Flows &flows_;
};

} // namespace tz
