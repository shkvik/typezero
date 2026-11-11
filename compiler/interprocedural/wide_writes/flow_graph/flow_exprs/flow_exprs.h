#pragma once

#include "compiler/interprocedural/flow_graph_builder/flow_graph_builder.h"

namespace tz {

class FlowExprs {
public:
  explicit FlowExprs(FlowGraphBuilder &owner);

  static const Expr *ownExpr(const Expr *e);
  Refs expr(Expr *e);
  Refs exprRefs(Expr *e);

private:
  FlowGraphBuilder &builder;
};

}
