#include "compiler/semantic/flow_v2/conditions/flow_conditions.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

FlowConditions::FlowConditions(SemanticState &owner)
    : state(owner) {}

void FlowConditions::addConditionFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  this->state.library.result.addResultFacts(cond, whenTrue, whenFalse);
  this->state.library.json.addJsonFacts(cond, whenTrue, whenFalse);
  this->state.flow.narrowing.addUnionFacts(cond, whenTrue, whenFalse);
  this->state.flow.narrowing.addUnknownFacts(cond, whenTrue, whenFalse);
  this->addOwnFacts(cond, whenTrue, whenFalse);
}

void FlowConditions::addOwnFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  auto *call = dyn_cast<CallExpr>(cond);
  if (call != nullptr && call->builtin() == Builtin::None) {
    this->state.flow.functions.applyPredicate(call, whenTrue, whenFalse);
    return;
  }
  const auto *name = this->state.flow.defined.optionalVariable(cond);
  if (name != nullptr) {
    whenTrue.defined.insert(name->var());
    return;
  }
  auto path = this->state.flow.defined.optionalField(cond);
  if (path.has_value()) {
    this->state.flow.tracked.defineField(whenTrue, *path);
    return;
  }
  if (this->state.flow.defined.isOptionalChain(cond)) {
    this->state.flow.defined.defineChain(cond, whenTrue);
    return;
  }
  auto *binary = dyn_cast<BinaryExpr>(cond);
  if (binary != nullptr) {
    this->addBinaryFacts(binary, whenTrue, whenFalse);
  }
}

void FlowConditions::addBinaryFacts(const BinaryExpr *binary, Flow &whenTrue,
                                    Flow &whenFalse) const {
  BinaryOp op = binary->op;
  bool isEquality = op == BinaryOp::Eq || op == BinaryOp::Ne;
  bool isUndefinedTest =
      isEquality && (isa<UndefinedExpr>(binary->lhs) || isa<UndefinedExpr>(binary->rhs));
  bool isComparison = isEquality || op == BinaryOp::Lt || op == BinaryOp::Le ||
                      op == BinaryOp::Gt || op == BinaryOp::Ge;
  if (isEquality) {
    this->state.flow.defined.addChainFacts(binary, whenTrue, whenFalse);
  }
  if (op == BinaryOp::InstanceOf) {
    this->state.flow.narrowing.addVariableInstanceOfFacts(binary, whenTrue);
    return;
  }
  if (isUndefinedTest) {
    this->state.flow.defined.addUndefinedFacts(binary, whenTrue, whenFalse);
    return;
  }
  if (isEquality) {
    this->state.flow.defined.addConvertedFacts(binary, whenTrue, whenFalse);
    this->state.flow.narrowing.addVariableTypeOfFacts(binary, whenTrue, whenFalse);
  }
  if (isComparison) {
    this->state.flow.inequalities.addLinearFacts(binary, whenTrue, whenFalse);
  }
}

}
