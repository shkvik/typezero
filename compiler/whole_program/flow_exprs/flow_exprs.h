#pragma once

#include "compiler/whole_program/flows/flows.h"

namespace tz {

class FlowExprs {
public:
  explicit FlowExprs(Flows &flows);

  static const Expr *ownExpr(const Expr *e);
  Refs expr(Expr *e);
  Refs exprRefs(Expr *e);

private:
  Flows &flows_;
};

}
