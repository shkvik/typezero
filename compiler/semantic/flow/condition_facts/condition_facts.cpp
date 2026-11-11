#include "compiler/semantic/flow/condition_facts/condition_facts.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/flow/predicate_checker/predicate_checker.h"
#include "compiler/semantic/flow/union_narrowing/union_narrowing.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/unknown_checker/unknown_checker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

ConditionFacts::ConditionFacts(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->fieldFacts = &all.getFieldFacts();
  this->indexProofs = &all.getIndexProofs();
  this->jsonChecker = &all.getJsonChecker();
  this->resultChecker = &all.getResultChecker();
  this->genericChecker = &all.getGenericChecker();
  this->unionNarrowing = &all.getUnionNarrowing();
  this->unknownChecker = &all.getUnknownChecker();
  this->predicateChecker = &all.getPredicateChecker();
}

const VarSymbol *ConditionFacts::variableOf(const Expr *expr) {
  if (auto *conv = dyn_cast<CastExpr>(expr);
      conv && conv->isImplicit() && conv->operand->type->isOptional()) {
    expr = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);

  return name ? name->var() : nullptr;
}

const NameExpr *ConditionFacts::optionalVariable(const Expr *expr) {
  if (auto *assign = dyn_cast<AssignExpr>(expr); assign && !assign->op) {
    expr = assign->target;
  }
  auto *name = dyn_cast<NameExpr>(expr);

  return name && name->var() && name->var()->type->isOptional() ? name : nullptr;
}

void ConditionFacts::addLess(Facts &facts, const Linear &x, const Linear &y, bool orEqual) {
  facts.add(x.term, y.term, y.offset - x.offset + (orEqual ? 1 : 0));
}

bool ConditionFacts::chainDefined(const Expr *e, Flow &flow) const {
  while (auto *conv = dyn_cast<CastExpr>(e)) {
    if (!conv->isImplicit()) {
      break;
    }
    e = conv->operand;
  }
  auto *chain = dyn_cast<OptionalChainExpr>(e);
  if (!chain) {
    return false;
  }
  std::optional<FieldPath> whole = ConditionFacts::definedPath(chain->chain, flow);
  if (whole && !whole->names.empty()) {
    FieldFacts::defineField(flow, *whole);
  }

  return true;
}

std::optional<FieldPath> ConditionFacts::definedPath(const Expr *expr, Flow &flow) {
  std::vector<const Expr *> links;
  const Expr *root = expr;
  const Expr *inner = ConditionFacts::pathInner(root);
  while (inner != nullptr) {
    links.push_back(root);
    root = inner;
    inner = ConditionFacts::pathInner(root);
  }
  std::optional<FieldPath> path = ConditionFacts::pathRoot(root);
  if (!path.has_value()) {
    return std::nullopt;
  }
  for (const Expr *link : llvm::reverse(links)) {
    ConditionFacts::extendPath(link, *path, flow);
  }

  return path;
}

const Expr *ConditionFacts::pathInner(const Expr *expr) {
  if (auto *link = dyn_cast<CastExpr>(expr)) {
    bool passes = link->shortCircuit || link->isImplicit();
    return passes ? link->operand : nullptr;
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    return member->object;
  }

  return nullptr;
}

std::optional<FieldPath> ConditionFacts::pathRoot(const Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr || name->var() == nullptr) {
    return std::nullopt;
  }
  FieldPath path;
  path.var = name->var();

  return path;
}

void ConditionFacts::extendPath(const Expr *link, FieldPath &path, Flow &flow) {
  auto *member = dyn_cast<MemberExpr>(link);
  bool checked = member == nullptr && cast<CastExpr>(link)->shortCircuit;
  if (member != nullptr) {
    path.names.push_back(member->name);
  } else if (checked && path.names.empty()) {
    flow.defined.insert(path.var);
  } else if (checked) {
    FieldFacts::defineField(flow, path);
  }
}

void ConditionFacts::addConditionFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  this->resultChecker->addResultFacts(cond, whenTrue, whenFalse);
  this->jsonChecker->addJsonFacts(cond, whenTrue, whenFalse);
  this->unionNarrowing->addUnionFacts(cond, whenTrue, whenFalse);
  this->unknownChecker->addUnknownFacts(cond, whenTrue, whenFalse);
  auto *call = dyn_cast<CallExpr>(cond);
  if (call != nullptr && call->builtin() == Builtin::None) {
    this->predicateChecker->applyPredicate(call, whenTrue, whenFalse);
    return;
  }
  const NameExpr *name = ConditionFacts::optionalVariable(cond);
  if (name != nullptr) {
    whenTrue.defined.insert(name->var());
    return;
  }
  std::optional<FieldPath> path = this->optionalField(cond);
  if (path.has_value()) {
    FieldFacts::defineField(whenTrue, *path);
    return;
  }
  if (this->chainDefined(cond, whenTrue)) {
    return;
  }
  auto *bin = dyn_cast<BinaryExpr>(cond);
  if (bin != nullptr) {
    this->addBinaryFacts(bin, whenTrue, whenFalse);
  }
}

std::optional<FieldPath> ConditionFacts::optionalField(const Expr *expr) const {
  if (expr->type == nullptr || !expr->type->isOptional()) {
    return std::nullopt;
  }

  return this->fieldFacts->fieldPath(expr);
}

void ConditionFacts::addBinaryFacts(const BinaryExpr *bin, Flow &whenTrue, Flow &whenFalse) const {
  BinaryOp op = bin->op;
  bool equality = op == BinaryOp::Eq || op == BinaryOp::Ne;
  if (equality) {
    this->addChainFacts(bin, whenTrue, whenFalse);
  }
  if (op == BinaryOp::InstanceOf) {
    this->addInstanceOfFacts(bin, whenTrue);
    return;
  }
  if (equality && (isa<UndefinedExpr>(bin->lhs) || isa<UndefinedExpr>(bin->rhs))) {
    this->addUndefinedFacts(bin, whenTrue, whenFalse);
    return;
  }
  if (equality) {
    ConditionFacts::addConvertedFacts(bin, whenTrue, whenFalse);
    this->addTypeOfFacts(bin, whenTrue, whenFalse);
  }
  bool comparison = equality || op == BinaryOp::Lt || op == BinaryOp::Le || op == BinaryOp::Gt ||
                    op == BinaryOp::Ge;
  if (comparison) {
    this->addLinearFacts(bin, whenTrue, whenFalse);
  }
}

const Expr *ConditionFacts::withoutPlainCasts(const Expr *expr) {
  const Expr *given = expr;
  auto *conv = dyn_cast<CastExpr>(given);
  while (conv != nullptr && conv->isImplicit() && !conv->shortCircuit) {
    given = conv->operand;
    conv = dyn_cast<CastExpr>(given);
  }

  return given;
}

void ConditionFacts::addChainFacts(const BinaryExpr *bin, Flow &whenTrue, Flow &whenFalse) const {
  BinaryOp op = bin->op;
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    const Expr *given = ConditionFacts::withoutPlainCasts(other);
    if (isa<UndefinedExpr>(given)) {
      this->chainDefined(side, op == BinaryOp::Ne ? whenTrue : whenFalse);
    } else if (given->type != nullptr && !given->type->isOptional() &&
               !given->type->isUndefined()) {
      this->chainDefined(side, op == BinaryOp::Eq ? whenTrue : whenFalse);
    }
  }
}

void ConditionFacts::addInstanceOfFacts(const BinaryExpr *bin, Flow &whenTrue) const {
  const NameExpr *name = ConditionFacts::optionalVariable(bin->lhs);
  if (name != nullptr) {
    whenTrue.defined.insert(name->var());
  } else {
    std::optional<FieldPath> path = this->optionalField(bin->lhs);
    if (path.has_value()) {
      FieldFacts::defineField(whenTrue, *path);
    }
  }
  const Type *cls = bin->rhs->type;
  if (cls != nullptr && cls->isConstructor()) {
    cls = cls->constructed();
  }
  const VarSymbol *var = ConditionFacts::variableOf(bin->lhs);
  if (var == nullptr || cls == nullptr) {
    return;
  }
  const Type *own = var->type->nonOptional();
  if (cls->isClass() && own->isObject() && cls != own && cls->isSubtypeOf(own)) {
    whenTrue.narrowed.push_back({var, cls});
  }
  if (cls->isBuffer() && own->bytes() == Type::Bytes::Uint8) {
    whenTrue.narrowed.push_back({var, cls});
  }
}

void ConditionFacts::addUndefinedFacts(const BinaryExpr *bin, Flow &whenTrue,
                                       Flow &whenFalse) const {
  BinaryOp op = bin->op;
  const Expr *side = isa<UndefinedExpr>(bin->rhs) ? bin->lhs : bin->rhs;
  const NameExpr *name = ConditionFacts::optionalVariable(side);
  if (name != nullptr) {
    (op == BinaryOp::Ne ? whenTrue : whenFalse).defined.insert(name->var());
    (op == BinaryOp::Ne ? whenFalse : whenTrue).undefinedVars.insert(name->var());
    return;
  }
  std::optional<FieldPath> path = this->optionalField(side);
  if (path.has_value()) {
    FieldFacts::defineField(op == BinaryOp::Ne ? whenTrue : whenFalse, *path);
  }
}

void ConditionFacts::addConvertedFacts(const BinaryExpr *bin, Flow &whenTrue, Flow &whenFalse) {
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    auto *conv = dyn_cast<CastExpr>(other);
    const NameExpr *name = ConditionFacts::optionalVariable(side);
    bool plainValue = name != nullptr && conv != nullptr && conv->isImplicit() &&
                      !conv->operand->type->isOptional() && !conv->operand->type->isUndefined();
    if (plainValue) {
      (bin->op == BinaryOp::Eq ? whenTrue : whenFalse).defined.insert(name->var());
    }
  }
}

void ConditionFacts::addTypeOfFacts(const BinaryExpr *bin, Flow &whenTrue, Flow &whenFalse) const {
  BinaryOp op = bin->op;
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    auto *typeOf = dyn_cast<UnaryExpr>(side);
    auto *text = dyn_cast<StringLitExpr>(other);
    if (typeOf == nullptr || typeOf->op != UnaryOp::TypeOf || text == nullptr) {
      continue;
    }
    const VarSymbol *var = ConditionFacts::variableOf(typeOf->operand);
    if (var != nullptr && var->type->nonOptional()->isParam()) {
      Flow &holds = op == BinaryOp::Eq ? whenTrue : whenFalse;
      Flow &differs = op == BinaryOp::Eq ? whenFalse : whenTrue;
      this->addParamTypeOfFacts(var, text->value, holds, differs);
      continue;
    }
    const NameExpr *name = ConditionFacts::optionalVariable(typeOf->operand);
    if (name == nullptr) {
      continue;
    }
    if (text->value == u"undefined") {
      (op == BinaryOp::Eq ? whenFalse : whenTrue).defined.insert(name->var());
      (op == BinaryOp::Eq ? whenTrue : whenFalse).undefinedVars.insert(name->var());
    } else {
      (op == BinaryOp::Eq ? whenTrue : whenFalse).defined.insert(name->var());
    }
  }
}

void ConditionFacts::addParamTypeOfFacts(const VarSymbol *var, const std::u16string &text,
                                         Flow &holds, Flow &differs) const {
  const Type *param = var->type->nonOptional();
  const Type *narrowed = this->paramTypeOfNarrowed(param, text);
  if (narrowed != nullptr) {
    holds.narrowed.push_back({var, narrowed});
    if (var->type->isOptional()) {
      holds.defined.insert(var);
    }
  }
  if (var->type->isOptional()) {
    return;
  }
  const Type *rest = this->otherAlternatives(param, text);
  if (rest != nullptr) {
    differs.narrowed.push_back({var, rest});
  }
}

const Type *ConditionFacts::paramTypeOfNarrowed(const Type *param,
                                                const std::u16string &text) const {
  std::string wanted = ToUtf8::toUtf8(text);
  if (text == u"string" && ConditionFacts::admits(param, ConditionFacts::isStringType)) {
    return Type::string();
  }
  if (text == u"boolean" && ConditionFacts::admits(param, ConditionFacts::isBoolType)) {
    return Type::boolTy();
  }
  if (param->constraint() == nullptr) {
    return this->numberNamed(param, wanted);
  }

  return nullptr;
}

bool ConditionFacts::isStringType(const Type *t) {
  return t->isString();
}

bool ConditionFacts::isBoolType(const Type *t) {
  return t->isBool();
}

bool ConditionFacts::admits(const Type *param, bool (*test)(const Type *)) {
  const Type *constraint = param->constraint();
  if (constraint != nullptr) {
    return test(constraint->nonOptional());
  }
  llvm::ArrayRef<const Type *> alternatives = param->alternatives();
  if (alternatives.empty()) {
    return true;
  }
  for (const Type *t : alternatives) {
    if (test(t)) {
      return true;
    }
  }

  return false;
}

const Type *ConditionFacts::numberNamed(const Type *param, llvm::StringRef name) const {
  for (const Type *t : param->alternatives()) {
    if (t->isNumeric() && name == TypeSpelling::typeOfName(t)) {
      return this->genericChecker->numberParam(param, t);
    }
  }
  const Type *named = UnknownChecker::typeOfNarrowed(std::u16string(name.begin(), name.end()));
  if (param->alternatives().empty() && named != nullptr && named->isNumeric()) {
    return this->genericChecker->numberParam(param, named);
  }

  return nullptr;
}

const Type *ConditionFacts::otherAlternatives(const Type *param, const std::u16string &text) const {
  llvm::SmallVector<const Type *, 4> others;
  for (const Type *t : param->alternatives()) {
    llvm::StringRef name = TypeSpelling::typeOfName(t);
    if (std::u16string(name.begin(), name.end()) != text) {
      others.push_back(t);
    }
  }
  if (others.empty()) {
    return nullptr;
  }
  llvm::StringRef kind = TypeSpelling::typeOfName(others.front());
  for (const Type *t : others) {
    if (TypeSpelling::typeOfName(t) != kind) {
      return nullptr;
    }
  }
  if (kind == "string") {
    return Type::string();
  }
  if (kind == "boolean") {
    return Type::boolTy();
  }

  return this->numberNamed(param, kind);
}

void ConditionFacts::addLinearFacts(const BinaryExpr *bin, Flow &whenTrue, Flow &whenFalse) const {
  BinaryOp op = bin->op;
  std::optional<Linear> l = this->indexProofs->linear(bin->lhs);
  std::optional<Linear> r = this->indexProofs->linear(bin->rhs);
  if (!l.has_value() || !r.has_value()) {
    return;
  }
  if (l->term.isZero() && r->term.isZero()) {
    bool holds = ConditionFacts::constantHolds(op, l->offset, r->offset);
    (holds ? whenFalse : whenTrue).unreachable = true;
    return;
  }
  bool equality = op == BinaryOp::Ne || op == BinaryOp::Eq;
  if (equality && (l->term.isZero() || r->term.isZero())) {
    this->addDifferFacts(op, *l, *r, whenTrue.facts, whenFalse.facts);
  }
  ConditionFacts::addOrderFacts(op, *l, *r, whenTrue.facts, whenFalse.facts);
}

bool ConditionFacts::constantHolds(BinaryOp op, int64_t a, int64_t b) {
  return op == BinaryOp::Lt   ? a < b
         : op == BinaryOp::Le ? a <= b
         : op == BinaryOp::Gt ? a > b
         : op == BinaryOp::Ge ? a >= b
         : op == BinaryOp::Eq ? a == b
                              : a != b;
}

void ConditionFacts::addDifferFacts(BinaryOp op, const Linear &l, const Linear &r, Facts &t,
                                    Facts &f) const {
  const Linear &x = l.term.isZero() ? r : l;
  const Linear &c = l.term.isZero() ? l : r;
  Facts &differ = op == BinaryOp::Ne ? t : f;
  if (this->state.getFlowState().getFacts().proves(c.term, x.term, x.offset - c.offset + 1)) {
    ConditionFacts::addLess(differ, c, x, false);
  } else if (this->state.getFlowState().getFacts().proves(x.term, c.term,
                                                          c.offset - x.offset + 1)) {
    ConditionFacts::addLess(differ, x, c, false);
  }
}

void ConditionFacts::addOrderFacts(BinaryOp op, const Linear &l, const Linear &r, Facts &t,
                                   Facts &f) {
  if (op == BinaryOp::Lt) {
    ConditionFacts::addLess(t, l, r, false);
    ConditionFacts::addLess(f, r, l, true);
    return;
  }
  if (op == BinaryOp::Le) {
    ConditionFacts::addLess(t, l, r, true);
    ConditionFacts::addLess(f, r, l, false);
    return;
  }
  if (op == BinaryOp::Gt) {
    ConditionFacts::addLess(t, r, l, false);
    ConditionFacts::addLess(f, l, r, true);
    return;
  }
  if (op == BinaryOp::Ge) {
    ConditionFacts::addLess(t, r, l, true);
    ConditionFacts::addLess(f, l, r, false);
    return;
  }
  if (op == BinaryOp::Eq) {
    ConditionFacts::addLess(t, l, r, true);
    ConditionFacts::addLess(t, r, l, true);
    return;
  }
  if (op == BinaryOp::Ne) {
    ConditionFacts::addLess(f, l, r, true);
    ConditionFacts::addLess(f, r, l, true);
  }
}
}
