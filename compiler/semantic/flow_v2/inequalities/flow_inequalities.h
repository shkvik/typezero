#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"

namespace tz {

class SemanticState;

class FlowInequalities {
public:
  explicit FlowInequalities(SemanticState &owner);

  void addLinearFacts(const BinaryExpr *binary, Flow &whenTrue, Flow &whenFalse) const;

private:
  void addLess(Facts &facts, const Linear &smaller, const Linear &larger) const;
  void addAtMost(Facts &facts, const Linear &smaller, const Linear &larger) const;
  void addOrderFacts(BinaryOp op, const Linear &left, const Linear &right, Facts &trueFacts,
                     Facts &falseFacts) const;
  void addDifferFacts(BinaryOp op, const Linear &left, const Linear &right, Facts &trueFacts,
                      Facts &falseFacts) const;


  bool isConstantHolding(BinaryOp op, int64_t left, int64_t right) const;

  SemanticState &state;
};

}
