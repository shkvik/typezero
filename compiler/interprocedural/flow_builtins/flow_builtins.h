#pragma once

#include "compiler/interprocedural/flow_graph_builder/flow_graph_builder.h"

namespace tz {

class FlowBuiltins {
public:
  explicit FlowBuiltins(FlowGraphBuilder &owner);

  Refs builtin(CallExpr *c, MemberExpr *member, const Refs &receiver,
               const std::vector<Refs> &args);

private:
  FlowGraphBuilder &builder;
};

}
