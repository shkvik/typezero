#pragma once

#include "compiler/interprocedural/flows/flows.h"

namespace tz {

class FlowExprs {
public:
  explicit FlowExprs(Flows &owner);

  static const Expr *ownExpr(const Expr *e);
  Refs expr(Expr *e);
  Refs exprRefs(Expr *e);

private:
  Flows &flows;
};

}
