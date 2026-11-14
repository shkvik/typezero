#include "compiler/semantic/flow_v2/inequalities/flow_inequalities.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

FlowInequalities::FlowInequalities(SemanticState &owner)
    : state(owner) {}

void FlowInequalities::addLess(Facts &facts, const Linear &smaller, const Linear &larger) const {
  facts.add(smaller.term, larger.term, larger.offset - smaller.offset);
}

void FlowInequalities::addAtMost(Facts &facts, const Linear &smaller, const Linear &larger) const {
  facts.add(smaller.term, larger.term, larger.offset - smaller.offset + 1);
}

void FlowInequalities::addLinearFacts(const BinaryExpr *binary, Flow &whenTrue,
                                      Flow &whenFalse) const {
  BinaryOp op = binary->op;
  auto left = this->state.flow.measurements.linear(binary->lhs);
  auto right = this->state.flow.measurements.linear(binary->rhs);
  if (!left.has_value() || !right.has_value()) {
    return;
  }
  if (left->term.isZero() && right->term.isZero()) {
    bool isHolding = this->isConstantHolding(op, left->offset, right->offset);
    Flow &impossible = isHolding ? whenFalse : whenTrue;
    impossible.unreachable = true;
    return;
  }
  bool isEquality = op == BinaryOp::Ne || op == BinaryOp::Eq;
  bool hasConstant = left->term.isZero() || right->term.isZero();
  if (isEquality && hasConstant) {
    this->addDifferFacts(op, *left, *right, whenTrue.facts, whenFalse.facts);
  }
  this->addOrderFacts(op, *left, *right, whenTrue.facts, whenFalse.facts);
}

bool FlowInequalities::isConstantHolding(BinaryOp op, int64_t left, int64_t right) const {
  if (op == BinaryOp::Lt) {
    return left < right;
  }
  if (op == BinaryOp::Le) {
    return left <= right;
  }
  if (op == BinaryOp::Gt) {
    return left > right;
  }
  if (op == BinaryOp::Ge) {
    return left >= right;
  }
  if (op == BinaryOp::Eq) {
    return left == right;
  }

  return left != right;
}

void FlowInequalities::addDifferFacts(BinaryOp op, const Linear &left, const Linear &right,
                                      Facts &trueFacts, Facts &falseFacts) const {
  const Linear &varying = left.term.isZero() ? right : left;
  const Linear &constant = left.term.isZero() ? left : right;
  Facts &differ = op == BinaryOp::Ne ? trueFacts : falseFacts;
  const auto &known = this->state.walk.flow.getFacts();
  int64_t aboveRoom = varying.offset - constant.offset + 1;
  if (known.canProve(constant.term, varying.term, aboveRoom)) {
    this->addLess(differ, constant, varying);
    return;
  }
  int64_t belowRoom = constant.offset - varying.offset + 1;
  if (known.canProve(varying.term, constant.term, belowRoom)) {
    this->addLess(differ, varying, constant);
  }
}

void FlowInequalities::addOrderFacts(BinaryOp op, const Linear &left, const Linear &right,
                                     Facts &trueFacts, Facts &falseFacts) const {
  if (op == BinaryOp::Lt) {
    this->addLess(trueFacts, left, right);
    this->addAtMost(falseFacts, right, left);
    return;
  }
  if (op == BinaryOp::Le) {
    this->addAtMost(trueFacts, left, right);
    this->addLess(falseFacts, right, left);
    return;
  }
  if (op == BinaryOp::Gt) {
    this->addLess(trueFacts, right, left);
    this->addAtMost(falseFacts, left, right);
    return;
  }
  if (op == BinaryOp::Ge) {
    this->addAtMost(trueFacts, right, left);
    this->addLess(falseFacts, left, right);
    return;
  }
  if (op == BinaryOp::Eq) {
    this->addAtMost(trueFacts, left, right);
    this->addAtMost(trueFacts, right, left);
    return;
  }
  if (op == BinaryOp::Ne) {
    this->addAtMost(falseFacts, left, right);
    this->addAtMost(falseFacts, right, left);
  }
}

}
