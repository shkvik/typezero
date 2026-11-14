#include "compiler/semantic/flow_v2/defined_values/flow_defined_values.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

FlowDefinedValues::FlowDefinedValues(SemanticState &owner)
    : state(owner) {}

const NameExpr *FlowDefinedValues::optionalVariable(const Expr *expr) const {
  auto *assign = dyn_cast<AssignExpr>(expr);
  bool isPlainAssign = assign != nullptr && !assign->op.has_value();
  const Expr *target = isPlainAssign ? assign->target : expr;
  auto *name = dyn_cast<NameExpr>(target);
  bool isOptional = name != nullptr && name->var() != nullptr && name->var()->type->isOptional();

  return isOptional ? name : nullptr;
}

bool FlowDefinedValues::isOptionalChain(const Expr *expr) const {
  const auto *bare = ExprParts::withoutImplicitCasts(expr);

  return isa<OptionalChainExpr>(bare);
}

void FlowDefinedValues::defineChain(const Expr *expr, Flow &flow) const {
  const auto *bare = ExprParts::withoutImplicitCasts(expr);
  auto *chain = dyn_cast<OptionalChainExpr>(bare);
  if (chain == nullptr) {
    return;
  }
  auto whole = this->definedPath(chain->chain, flow);
  if (whole.has_value() && !whole->names.empty()) {
    this->state.flow.tracked.defineField(flow, *whole);
  }
}

std::optional<FieldPath> FlowDefinedValues::definedPath(const Expr *expr, Flow &flow) const {
  auto links = std::vector<const Expr *>();
  const Expr *root = expr;
  const auto *inner = this->pathInner(root);
  while (inner != nullptr) {
    links.push_back(root);
    root = inner;
    inner = this->pathInner(root);
  }
  auto path = this->pathRoot(root);
  if (!path.has_value()) {
    return std::nullopt;
  }
  for (const Expr *link : llvm::reverse(links)) {
    this->extendPath(link, *path, flow);
  }

  return path;
}

const Expr *FlowDefinedValues::pathInner(const Expr *expr) const {
  auto *link = dyn_cast<CastExpr>(expr);
  auto *member = dyn_cast<MemberExpr>(expr);
  if (link != nullptr) {
    bool isPassing = link->shortCircuit || link->isImplicit();

    return isPassing ? link->operand : nullptr;
  }
  if (member != nullptr) {
    return member->object;
  }

  return nullptr;
}

std::optional<FieldPath> FlowDefinedValues::pathRoot(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr || name->var() == nullptr) {
    return std::nullopt;
  }

  return FieldPath{.var = name->var()};
}

void FlowDefinedValues::extendPath(const Expr *link, FieldPath &path, Flow &flow) const {
  auto *member = dyn_cast<MemberExpr>(link);
  if (member != nullptr) {
    path.names.push_back(member->name);
    return;
  }
  bool isChecked = cast<CastExpr>(link)->shortCircuit;
  if (isChecked && path.names.empty()) {
    flow.defined.insert(path.var);
    return;
  }
  if (isChecked) {
    this->state.flow.tracked.defineField(flow, path);
  }
}

std::optional<FieldPath> FlowDefinedValues::optionalField(const Expr *expr) const {
  if (expr->type == nullptr || !expr->type->isOptional()) {
    return std::nullopt;
  }

  return this->state.flow.tracked.fieldPath(expr);
}

void FlowDefinedValues::defineOptional(const Expr *expr, Flow &flow) const {
  const auto *name = this->optionalVariable(expr);
  if (name != nullptr) {
    flow.defined.insert(name->var());
    return;
  }
  auto path = this->optionalField(expr);
  if (path.has_value()) {
    this->state.flow.tracked.defineField(flow, *path);
  }
}

const Expr *FlowDefinedValues::withoutPlainCasts(const Expr *expr) const {
  const Expr *given = expr;
  auto *conversion = dyn_cast<CastExpr>(given);
  while (conversion != nullptr && conversion->isImplicit() && !conversion->shortCircuit) {
    given = conversion->operand;
    conversion = dyn_cast<CastExpr>(given);
  }

  return given;
}

bool FlowDefinedValues::isDefinedValue(const Type *type) const {
  return type != nullptr && !type->isOptional() && !type->isUndefined();
}

void FlowDefinedValues::addChainFacts(const BinaryExpr *binary, Flow &whenTrue,
                                      Flow &whenFalse) const {
  Flow &whenEqual = binary->op == BinaryOp::Eq ? whenTrue : whenFalse;
  Flow &whenUnequal = binary->op == BinaryOp::Ne ? whenTrue : whenFalse;
  this->addSideChainFacts(binary->lhs, binary->rhs, whenEqual, whenUnequal);
  this->addSideChainFacts(binary->rhs, binary->lhs, whenEqual, whenUnequal);
}

void FlowDefinedValues::addSideChainFacts(const Expr *side, const Expr *other, Flow &whenEqual,
                                          Flow &whenUnequal) const {
  const auto *given = this->withoutPlainCasts(other);
  bool isUndefined = isa<UndefinedExpr>(given);
  if (isUndefined) {
    this->defineChain(side, whenUnequal);
    return;
  }
  if (this->isDefinedValue(given->type)) {
    this->defineChain(side, whenEqual);
  }
}

void FlowDefinedValues::addUndefinedFacts(const BinaryExpr *binary, Flow &whenTrue,
                                          Flow &whenFalse) const {
  Flow &whenEqual = binary->op == BinaryOp::Eq ? whenTrue : whenFalse;
  Flow &whenUnequal = binary->op == BinaryOp::Ne ? whenTrue : whenFalse;
  const Expr *side = isa<UndefinedExpr>(binary->rhs) ? binary->lhs : binary->rhs;
  const auto *name = this->optionalVariable(side);
  if (name != nullptr) {
    whenUnequal.defined.insert(name->var());
    whenEqual.undefinedVars.insert(name->var());
    return;
  }
  auto path = this->optionalField(side);
  if (path.has_value()) {
    this->state.flow.tracked.defineField(whenUnequal, *path);
  }
}

void FlowDefinedValues::addConvertedFacts(const BinaryExpr *binary, Flow &whenTrue,
                                          Flow &whenFalse) const {
  Flow &whenEqual = binary->op == BinaryOp::Eq ? whenTrue : whenFalse;
  this->addSideConvertedFacts(binary->lhs, binary->rhs, whenEqual);
  this->addSideConvertedFacts(binary->rhs, binary->lhs, whenEqual);
}

void FlowDefinedValues::addSideConvertedFacts(const Expr *side, const Expr *other,
                                              Flow &whenEqual) const {
  auto *conversion = dyn_cast<CastExpr>(other);
  const auto *name = this->optionalVariable(side);
  bool isPlainValue = name != nullptr && conversion != nullptr && conversion->isImplicit() &&
                      this->isDefinedValue(conversion->operand->type);
  if (isPlainValue) {
    whenEqual.defined.insert(name->var());
  }
}

}
