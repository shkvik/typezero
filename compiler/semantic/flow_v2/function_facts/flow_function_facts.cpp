#include "compiler/semantic/flow_v2/function_facts/flow_function_facts.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr int kThisPosition = -1;
constexpr int kNoPosition = -2;
constexpr int kFirstParam = 0;
constexpr uint64_t kMaxTermLiteral = uint64_t(1) << 31;

}

FlowFunctionFacts::FlowFunctionFacts(SemanticState &owner)
    : state(owner) {}

const VarSymbol *FlowFunctionFacts::argumentVariable(const Expr *expr) const {
  const Expr *inner = expr;
  auto *conversion = dyn_cast<CastExpr>(inner);
  while (conversion != nullptr) {
    if (!conversion->isImplicit() || conversion->shortCircuit) {
      return nullptr;
    }
    inner = conversion->operand;
    conversion = dyn_cast<CastExpr>(inner);
  }
  auto *name = dyn_cast<NameExpr>(inner);

  return name != nullptr ? name->var() : nullptr;
}

const Expr *FlowFunctionFacts::withoutImplicitCasts(const Expr *expr) const {
  const Expr *inner = expr;
  auto *conversion = dyn_cast<CastExpr>(inner);
  while (conversion != nullptr) {
    if (!conversion->isImplicit()) {
      return nullptr;
    }
    inner = conversion->operand;
    conversion = dyn_cast<CastExpr>(inner);
  }

  return inner;
}

std::optional<Linear> FlowFunctionFacts::argumentTerm(const Expr *expr) const {
  const auto *inner = this->withoutImplicitCasts(expr);
  if (inner == nullptr) {
    return std::nullopt;
  }
  auto *unary = dyn_cast<UnaryExpr>(inner);
  bool isNegated = unary != nullptr && unary->op == UnaryOp::Neg;
  const Expr *value = isNegated ? unary->operand : inner;
  auto *literal = dyn_cast<IntLitExpr>(value);
  auto *name = dyn_cast<NameExpr>(value);
  if (literal != nullptr && literal->value < kMaxTermLiteral) {
    auto magnitude = int64_t(literal->value);

    return Linear{Term::zero(), isNegated ? -magnitude : magnitude};
  }
  if (name != nullptr && name->var() != nullptr && !isNegated) {
    return Linear{Term::of(name->var()), 0};
  }

  return std::nullopt;
}

bool FlowFunctionFacts::isLookingBoolean(const Expr *expr) const {
  for (const Expr *part : ExprParts::logicParts(expr)) {
    if (!this->isLookingLikeTest(part)) {
      return false;
    }
  }

  return true;
}

bool FlowFunctionFacts::isLookingLikeTest(const Expr *expr) const {
  auto *unary = dyn_cast<UnaryExpr>(expr);
  auto *binary = dyn_cast<BinaryExpr>(expr);
  if (isa<BoolLitExpr>(expr)) {
    return true;
  }
  if (unary != nullptr) {
    return unary->op == UnaryOp::Not;
  }
  if (binary != nullptr) {
    return this->isTestOperator(binary->op);
  }

  return false;
}

bool FlowFunctionFacts::isTestOperator(BinaryOp op) const {
  bool isComparison = op == BinaryOp::Eq || op == BinaryOp::Ne || op == BinaryOp::Lt ||
                      op == BinaryOp::Le || op == BinaryOp::Gt || op == BinaryOp::Ge;

  return isComparison || op == BinaryOp::InstanceOf || op == BinaryOp::In;
}

bool FlowFunctionFacts::isPredicateNeeded(const FuncSymbol *function) const {
  const auto &predicates = this->state.flow.predicates;
  if (predicates.findPredicate(function) != nullptr) {
    return false;
  }
  if (!this->isSummarizable(function, function->decl)) {
    return false;
  }

  return !function->inferring || !predicates.isSummarizing(function);
}

const FuncSymbol *FlowFunctionFacts::findPredicateNeed(const Expr *cond) const {
  auto *call = dyn_cast<CallExpr>(cond);
  if (call == nullptr || call->builtin() != Builtin::None) {
    return nullptr;
  }
  const FuncSymbol *function = call->func() != nullptr ? call->func() : call->method();
  bool isApplied = function != nullptr && !call->virtualCall() && call->unionMethods().empty();

  return isApplied && this->isPredicateNeeded(function) ? function : nullptr;
}

const FuncSymbol *FlowFunctionFacts::findFilterNeed(const Expr *callback) const {
  const auto *function = this->callbackFunction(callback);

  return function != nullptr && this->isPredicateNeeded(function) ? function : nullptr;
}

bool FlowFunctionFacts::isSummarizable(const FuncSymbol *function,
                                       const FunctionDecl *functionDecl) const {
  if (functionDecl == nullptr || functionDecl->checked || functionDecl->body == nullptr) {
    return false;
  }
  if (functionDecl->external() || functionDecl->isGenerator || functionDecl->isInstance) {
    return false;
  }

  return !function->hasReturnType() || function->getKnownReturnType()->isBool();
}

void FlowFunctionFacts::applyPredicate(const CallExpr *call, Flow &whenTrue,
                                       Flow &whenFalse) const {
  const FuncSymbol *function = call->func() != nullptr ? call->func() : call->method();
  if (function == nullptr || call->virtualCall() || !call->unionMethods().empty()) {
    return;
  }
  auto applied = PredicateCall{
      .call = call,
      .member = dyn_cast<MemberExpr>(call->callee),
      .summary = this->state.flow.predicates.findPredicate(function),
  };
  if (applied.summary != nullptr) {
    this->applySide(applied, applied.summary->whenTrue, whenTrue);
    this->applySide(applied, applied.summary->whenFalse, whenFalse);
  }
  if (!function->predicate.has_value()) {
    return;
  }
  const auto *variable = this->predicateVariable(applied, function->predicate->param);
  if (variable != nullptr) {
    this->narrowParamTo(whenTrue, variable, function->predicate->type);
  }
}

const Expr *FlowFunctionFacts::predicateArgument(const PredicateCall &applied, int index) const {
  const CallExpr *call = applied.call;
  if (index == kThisPosition) {
    bool isMethodCall = call->method() != nullptr && applied.member != nullptr;

    return isMethodCall ? applied.member->object : nullptr;
  }
  if (size_t(index) >= call->args.size()) {
    return nullptr;
  }

  return call->args[size_t(index)];
}

const VarSymbol *FlowFunctionFacts::predicateVariable(const PredicateCall &applied,
                                                      int index) const {
  const auto *arg = this->predicateArgument(applied, index);
  if (arg == nullptr) {
    return nullptr;
  }
  const auto *variable = this->argumentVariable(arg);
  bool isStable = variable != nullptr && !this->state.flow.changes.isChangedByCalls(variable);

  return isStable ? variable : nullptr;
}

void FlowFunctionFacts::narrowParamTo(Flow &flow, const VarSymbol *variable,
                                      const Type *type) const {
  const auto *now = this->state.flow.tracked.typeIn(flow, variable);
  if (this->state.types.relations.isImplicitlyConvertible(now, type)) {
    return;
  }
  if (this->state.types.relations.isImplicitlyConvertible(type, now)) {
    this->state.flow.narrowing.narrowVariable(flow, variable, type);
    return;
  }
  const Type *value = now->nonOptional();
  if (!value->isUnion()) {
    return;
  }
  auto kept = this->convertibleMembers(value, type);
  const Type *narrowed = kept.empty() ? nullptr : Type::unionOf(kept);
  this->state.flow.narrowing.narrowVariable(flow, variable, narrowed);
}

std::vector<const Type *> FlowFunctionFacts::convertibleMembers(const Type *value,
                                                                const Type *type) const {
  auto kept = std::vector<const Type *>();
  for (const Type *member : value->members()) {
    if (this->state.types.relations.isImplicitlyConvertible(member, type)) {
      kept.push_back(member);
    }
  }

  return kept;
}

void FlowFunctionFacts::applySide(const PredicateCall &applied, const PredicateSide &side,
                                  Flow &flow) const {
  if (side.unreachable) {
    flow.unreachable = true;
    return;
  }
  this->applyNarrowed(applied, side, flow);
  this->applyDefined(applied, side, flow);
  this->applyFields(applied, side, flow);
  this->applyNarrowedFields(applied, side, flow);
  this->applyFacts(applied, side, flow);
}

void FlowFunctionFacts::applyNarrowed(const PredicateCall &applied, const PredicateSide &side,
                                      Flow &flow) const {
  for (auto [param, type] : side.narrowed) {
    const auto *variable = this->predicateVariable(applied, param);
    if (variable != nullptr) {
      this->narrowParamTo(flow, variable, type);
    }
  }
}

void FlowFunctionFacts::applyDefined(const PredicateCall &applied, const PredicateSide &side,
                                     Flow &flow) const {
  for (int param : side.defined) {
    const auto *variable = this->predicateVariable(applied, param);
    if (variable != nullptr && variable->type->isOptional()) {
      flow.defined.insert(variable);
    }
  }
}

void FlowFunctionFacts::applyFields(const PredicateCall &applied, const PredicateSide &side,
                                    Flow &flow) const {
  for (const auto &[param, names] : side.fields) {
    const auto *variable = this->predicateVariable(applied, param);
    if (variable != nullptr) {
      auto path = FieldPath{.var = variable, .names = names};
      this->state.flow.tracked.defineField(flow, path);
    }
  }
}

void FlowFunctionFacts::applyNarrowedFields(const PredicateCall &applied, const PredicateSide &side,
                                            Flow &flow) const {
  for (const PredicateSide::NarrowedField &field : side.narrowedFields) {
    const auto *variable = this->predicateVariable(applied, field.param);
    if (variable != nullptr) {
      auto path = FieldPath{.var = variable, .names = field.names};
      this->narrowFieldTo(flow, path, field.type);
    }
  }
}

bool FlowFunctionFacts::isPathEntry(const FieldPath &path,
                                    const std::pair<FieldPath, const Type *> &entry) const {
  return entry.first == path;
}

void FlowFunctionFacts::narrowFieldTo(Flow &flow, const FieldPath &path, const Type *type) const {
  auto isEntry = std::bind_front(&FlowFunctionFacts::isPathEntry, this, std::cref(path));
  llvm::erase_if(flow.narrowedFields, isEntry);
  flow.narrowedFields.push_back({path, type});
}

void FlowFunctionFacts::applyFacts(const PredicateCall &applied, const PredicateSide &side,
                                   Flow &flow) const {
  for (const Facts::Fact &fact : side.facts) {
    auto left = this->factTerm(applied, fact.lhs);
    auto right = this->factTerm(applied, fact.rhs);
    if (left.has_value() && right.has_value()) {
      flow.facts.add(left->term, right->term, fact.offset + right->offset - left->offset);
    }
  }
}

int FlowFunctionFacts::summaryParam(const Predicate *summary, const VarSymbol *variable) const {
  for (size_t index = 0; index < summary->params.size(); ++index) {
    if (summary->params[index] == variable) {
      return int(index);
    }
  }

  return kNoPosition;
}

std::optional<Linear> FlowFunctionFacts::factTerm(const PredicateCall &applied,
                                                  const Term &term) const {
  if (term.isZero()) {
    return Linear{Term::zero(), 0};
  }
  int index = this->summaryParam(applied.summary, term.var);
  if (index == kNoPosition) {
    return std::nullopt;
  }
  const auto *arg = this->predicateArgument(applied, index);
  if (arg == nullptr) {
    return std::nullopt;
  }
  if (term.length) {
    return this->lengthTerm(applied, index);
  }
  auto value = this->argumentTerm(arg);
  bool hasChanged = value.has_value() && value->term.var != nullptr &&
                    this->state.flow.changes.isChangedByCalls(value->term.var);

  return hasChanged ? std::nullopt : value;
}

std::optional<Linear> FlowFunctionFacts::lengthTerm(const PredicateCall &applied, int index) const {
  const auto *variable = this->predicateVariable(applied, index);
  if (variable == nullptr) {
    return std::nullopt;
  }

  return Linear{Term::lengthOf(variable), 0};
}

const Type *FlowFunctionFacts::filteredType(const Expr *callback, const Type *element) const {
  const auto *function = this->callbackFunction(callback);
  if (function == nullptr) {
    return nullptr;
  }
  const auto *kept = this->keptByPredicate(function, element);
  bool isNarrowing = kept != nullptr && kept != element &&
                     this->state.types.relations.isImplicitlyConvertible(kept, element);

  return isNarrowing ? kept : nullptr;
}

const FuncSymbol *FlowFunctionFacts::callbackFunction(const Expr *callback) const {
  auto *functionExpr = dyn_cast<FunctionExpr>(callback);
  auto *name = dyn_cast<NameExpr>(callback);
  if (functionExpr != nullptr) {
    return functionExpr->fn->sym;
  }
  if (name != nullptr) {
    return this->namedFunction(name);
  }

  return nullptr;
}

const FuncSymbol *FlowFunctionFacts::namedFunction(const NameExpr *name) const {
  if (name->func() != nullptr) {
    return name->func();
  }
  const VarSymbol *variable = name->var();
  if (variable != nullptr && variable->function() != nullptr) {
    return variable->function()->sym;
  }

  return nullptr;
}

const Type *FlowFunctionFacts::keptByPredicate(const FuncSymbol *function,
                                               const Type *element) const {
  bool isOnFirstParam =
      function->predicate.has_value() && function->predicate->param == kFirstParam;
  const auto *summary = this->state.flow.predicates.findPredicate(function);
  if (isOnFirstParam) {
    return function->predicate->type;
  }
  if (summary == nullptr) {
    return nullptr;
  }

  return this->keptBySide(summary->whenTrue, element);
}

const Type *FlowFunctionFacts::keptBySide(const PredicateSide &side, const Type *element) const {
  if (side.unreachable) {
    return nullptr;
  }
  const auto *narrowed = this->firstParamNarrowed(side);
  if (narrowed != nullptr) {
    return narrowed;
  }
  bool isDefined = element->isOptional() && llvm::is_contained(side.defined, kFirstParam);

  return isDefined ? element->inner() : nullptr;
}

const Type *FlowFunctionFacts::firstParamNarrowed(const PredicateSide &side) const {
  const Type *narrowed = nullptr;
  for (auto [param, type] : side.narrowed) {
    if (param == kFirstParam) {
      narrowed = type;
    }
  }

  return narrowed;
}

}
